/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "pthread_internal.h"
#include "private/bionic_futex.h"
#include "private/bionic_pthread.h"
#include "private/kernel_sigset_t.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

// System calls.
extern "C" int __rt_sigtimedwait(const sigset_t*, siginfo_t*, const struct timespec*, size_t);
extern "C" int __timer_create(clockid_t, sigevent*, __kernel_timer_t*);
extern "C" int __timer_delete(__kernel_timer_t);
extern "C" int __timer_getoverrun(__kernel_timer_t);
extern "C" int __timer_gettime(__kernel_timer_t, itimerspec*);
extern "C" int __timer_settime(__kernel_timer_t, int, const itimerspec*, itimerspec*);

// Most POSIX timers are handled directly by the kernel. We translate SIGEV_THREAD timers
// into SIGEV_THREAD_ID timers so the kernel handles all the time-related stuff and we just
// need to worry about running user code on a thread.

// We can't use SIGALRM because too many other C library functions throw that around, and since
// they don't send to a specific thread, all threads are eligible to handle the signal and we can
// end up with one of our POSIX timer threads handling it (meaning that the intended recipient
// doesn't). glibc uses SIGRTMIN for its POSIX timer implementation, so in the absence of any
// reason to use anything else, we use that too.
static const int TIMER_SIGNAL = SIGRTMIN;

struct PosixTimer {
  __kernel_timer_t kernel_timer_id;

  int sigev_notify;

  // These fields are only needed for a SIGEV_THREAD timer.
  pthread_t callback_thread;
  void (*callback)(sigval_t);
  sigval_t callback_argument;
  volatile int exiting;
};

static __kernel_timer_t to_kernel_timer_id(timer_t timer) {
  return reinterpret_cast<PosixTimer*>(timer)->kernel_timer_id;
}

static void* __timer_thread_start(void* arg) {
  PosixTimer* timer = reinterpret_cast<PosixTimer*>(arg);

  kernel_sigset_t sigset;
  sigaddset(sigset.get(), TIMER_SIGNAL);

  while (true) {
    // Wait for a signal...
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    int rc = __rt_sigtimedwait(sigset.get(), &si, NULL, sizeof(sigset));
    if (rc == -1) {
      continue;
    }

    if (si.si_code == SI_TIMER) {
      // This signal was sent because a timer fired, so call the callback.
      timer->callback(timer->callback_argument);
    } else if (si.si_code == SI_TKILL) {
      // This signal was sent because someone wants us to exit.
      timer->exiting = 1;
      __futex_wake(&timer->exiting, INT32_MAX);
      return NULL;
    }
  }
}

static void __timer_thread_stop(PosixTimer* timer) {
  pthread_kill(timer->callback_thread, TIMER_SIGNAL);

  // We can't pthread_join because POSIX says "the threads created in response to a timer
  // expiration are created detached, or in an unspecified way if the thread attribute's
  // detachstate is PTHREAD_CREATE_JOINABLE".
  while (timer->exiting == 0) {
    __futex_wait(&timer->exiting, 0, NULL);
  }
}

// http://pubs.opengroup.org/onlinepubs/9699919799/functions/timer_create.html
int timer_create(clockid_t clock_id, sigevent* evp, timer_t* timer_id) {
  PosixTimer* new_timer = reinterpret_cast<PosixTimer*>(malloc(sizeof(PosixTimer)));
  if (new_timer == NULL) {
    return -1;
  }

  new_timer->sigev_notify = (evp == NULL) ? SIGEV_SIGNAL : evp->sigev_notify;

  // If not a SIGEV_THREAD timer, the kernel can handle it without our help.
  if (new_timer->sigev_notify != SIGEV_THREAD) {
    if (__timer_create(clock_id, evp, &new_timer->kernel_timer_id) == -1) {
      free(new_timer);
      return -1;
    }

    *timer_id = new_timer;
    return 0;
  }

  // Otherwise, this must be SIGEV_THREAD timer...
  new_timer->callback = evp->sigev_notify_function;
  new_timer->callback_argument = evp->sigev_value;
  new_timer->exiting = 0;

  // Check arguments that the kernel doesn't care about but we do.
  if (new_timer->callback == NULL) {
    free(new_timer);
    errno = EINVAL;
    return -1;
  }

  // Create this timer's thread.
  pthread_attr_t thread_attributes;
  if (evp->sigev_notify_attributes == NULL) {
    pthread_attr_init(&thread_attributes);
  } else {
    thread_attributes = *reinterpret_cast<pthread_attr_t*>(evp->sigev_notify_attributes);
  }
  pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_DETACHED);

  // We start the thread with TIMER_SIGNAL blocked by blocking the signal here and letting it
  // inherit. If it tried to block the signal itself, there would be a race.
  kernel_sigset_t sigset;
  sigaddset(sigset.get(), TIMER_SIGNAL);
  kernel_sigset_t old_sigset;
  pthread_sigmask(SIG_BLOCK, sigset.get(), old_sigset.get());

  int rc = pthread_create(&new_timer->callback_thread, &thread_attributes, __timer_thread_start, new_timer);

  pthread_sigmask(SIG_SETMASK, old_sigset.get(), NULL);

  if (rc != 0) {
    free(new_timer);
    errno = rc;
    return -1;
  }

  sigevent se = *evp;
  se.sigev_signo = TIMER_SIGNAL;
  se.sigev_notify = SIGEV_THREAD_ID;
  se.sigev_notify_thread_id = __pthread_gettid(new_timer->callback_thread);
  if (__timer_create(clock_id, &se, &new_timer->kernel_timer_id) == -1) {
    __timer_thread_stop(new_timer);
    free(new_timer);
    return -1;
  }

  // Give the thread a meaningful name.
  // It can't do this itself because the kernel timer isn't created until after it's running.
  char name[32];
  snprintf(name, sizeof(name), "POSIX interval timer %d", to_kernel_timer_id(new_timer));
  pthread_setname_np(new_timer->callback_thread, name);

  *timer_id = new_timer;
  return 0;
}

// http://pubs.opengroup.org/onlinepubs/9699919799/functions/timer_delete.html
int timer_delete(timer_t id) {
  int rc = __timer_delete(to_kernel_timer_id(id));
  if (rc == -1) {
    return -1;
  }

  PosixTimer* timer = reinterpret_cast<PosixTimer*>(id);

  // Make sure the timer's thread has exited before we free the timer data.
  if (timer->sigev_notify == SIGEV_THREAD) {
    __timer_thread_stop(timer);
  }

  free(timer);

  return 0;
}

// http://pubs.opengroup.org/onlinepubs/9699919799/functions/timer_getoverrun.html
int timer_gettime(timer_t id, itimerspec* ts) {
  return __timer_gettime(to_kernel_timer_id(id), ts);
}

// http://pubs.opengroup.org/onlinepubs/9699919799/functions/timer_getoverrun.html
int timer_settime(timer_t id, int flags, const itimerspec* ts, itimerspec* ots) {
  return __timer_settime(to_kernel_timer_id(id), flags, ts, ots);
}

// http://pubs.opengroup.org/onlinepubs/9699919799/functions/timer_getoverrun.html
int timer_getoverrun(timer_t id) {
  return __timer_getoverrun(to_kernel_timer_id(id));
}
