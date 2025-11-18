/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore* sema, unsigned value)
{
    ASSERT(sema != NULL);

    sema->value = value;
    list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void sema_down(struct semaphore* sema)
{
    enum intr_level old_level;

    ASSERT(sema != NULL);
    ASSERT(!intr_context());

    old_level = intr_disable();
    while (sema->value == 0) {
        list_insert_ordered(&sema->waiters, &thread_current()->elem, more_mvp_func, NULL);
        thread_block();
    }
    sema->value--;
    intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore* sema)
{
    enum intr_level old_level;
    bool success;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    if (sema->value > 0) {
        sema->value--;
        success = true;
    } else
        success = false;
    intr_set_level(old_level);

    return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up(struct semaphore* sema)
{
    enum intr_level old_level;

    ASSERT(sema != NULL);

    old_level = intr_disable();
    if (!list_empty(&sema->waiters)) {
        // waitlist 내에 대기 하던 중에 priority 가 바뀌는 것을 고려하여 최댓값을 찾안주는게 안전함
        // 그냥 맨 앞에거 갖다쓰면 priority-donate-sema 계속 실패
        struct list_elem* max_elem = list_begin(&sema->waiters);
        struct list_elem* e;
        for (e = list_next(max_elem); e != list_end(&sema->waiters); e = list_next(e)) {
            if (list_entry(e, struct thread, elem)->priority > list_entry(max_elem, struct thread, elem)->priority) {
                max_elem = e;
            }
        }

        // 2. 찾은 스레드를 리스트에서 제거하고 unblock 합니다.
        struct thread* t = list_entry(max_elem, struct thread, elem);
        list_remove(max_elem);
        thread_unblock(t);

        sema->value++;

        // 3. [핵심 수정] Preemption(선점) 구현
        // 방금 깨운 스레드(t)가 현재 스레드보다 우선순위가 높다면 CPU 양보
        // intr_context() 체크는 인터럽트 핸들러 내부가 아닐 때만 yield 하기 위함 (안전장치)
        if (!intr_context() && t->priority > thread_current()->priority) {
            thread_yield();
        }
    } else sema->value++;
    intr_set_level(old_level);
    
}

static void sema_test_helper(void* sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void)
{
    struct semaphore sema[2];
    int i;

    printf("Testing semaphores...");
    sema_init(&sema[0], 0);
    sema_init(&sema[1], 0);
    thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
    for (i = 0; i < 10; i++) {
        sema_up(&sema[0]);
        sema_down(&sema[1]);
    }
    printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void* sema_)
{
    struct semaphore* sema = sema_;
    int i;

    for (i = 0; i < 10; i++) {
        sema_down(&sema[0]);
        sema_up(&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock* lock)
{
    ASSERT(lock != NULL);

    lock->holder = NULL;
    sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock* lock)
{
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(!lock_held_by_current_thread(lock));

    struct thread* curr = thread_current();

    if (lock->semaphore.value == 0 && is_valid(lock->holder)) {
        curr->waiting_lock = lock; // 막혔고 안에 사람 있으면 일단 대기 등록

        if (curr->priority > lock->holder->priority) { // 내가 더 크면 기부하기
            struct thread* holder = lock->holder;
            list_push_back(&holder->donations, &curr->donation_elem);
            thread_recalculate_priority(holder); // 재귀적으로 중첩된 모든 holder의 우선순위를 갱신
        }
    }
    sema_down(&lock->semaphore);

    lock->holder = curr;
    curr->waiting_lock = NULL;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock* lock)
{
    bool success;

    ASSERT(lock != NULL);
    ASSERT(!lock_held_by_current_thread(lock));

    success = sema_try_down(&lock->semaphore);
    if (success)
        lock->holder = thread_current();
    return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock* lock)
{
    ASSERT(lock != NULL);
    ASSERT(lock_held_by_current_thread(lock));

    struct thread* curr = thread_current(); // (curr == lock->holder)

    // 1. 이 락(lock)을 기다리면서 나(curr)에게 기부했던 모든 스레드(donor)를 나의 donations 리스트에서 제거
    struct list_elem* e = list_begin(&curr->donations);
    while (e != list_end(&curr->donations)) {
        struct thread* donor = list_entry(e, struct thread, donation_elem);
        struct list_elem* next = list_next(e); // list_remove 대비

        if (donor->waiting_lock == lock) {
            list_remove(e);
        }
        e = next;
    }
    thread_recalculate_priority(curr);

    lock->holder = NULL;
    sema_up(&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock* lock)
{
    ASSERT(lock != NULL);

    return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem {
    struct list_elem elem;      /* List element. */
    struct semaphore semaphore; /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition* cond)
{
    ASSERT(cond != NULL);

    list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition* cond, struct lock* lock)
{
    // whenever a thread needs to wait for a condition
    // it creates a semaphore_elem waiter to put into the condition waiting list
    struct semaphore_elem waiter;

    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(lock_held_by_current_thread(lock));
    // a semaphore_elem struct keeps track of a semaphore and its elem
    // after initializing the semaphore,
    // it saves its elem in the condition waiting list
    // and sema downs its semaphore which puts it into sleep
    // it releases its key before sema down so that another thread can access it while it goes to sleep
    sema_init(&waiter.semaphore, 0);
    list_push_back(&cond->waiters, &waiter.elem);
    lock_release(lock);
    sema_down(&waiter.semaphore);
    // the thread is woken up when it its semaphore_elem struct's elem in the condition waiting list
    // is signaled by cond_signal(cond) or cond_broadcast()
    // the cond_signal(cond) has to check its list of semaphore_elems' in its waiting list
    // check their semaphore waiting list to get the thread's elem and its prirority
    lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition* cond, struct lock* lock UNUSED)
{
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);
    ASSERT(!intr_context());
    ASSERT(lock_held_by_current_thread(lock));

    if (!list_empty(&cond->waiters)) {

        struct list_elem* max_sem_elem = NULL;
        int max_priority = PRI_MIN;
        struct list_elem* e;

        // traverse through waiting list of condition lock
        for (e = list_begin(&cond->waiters); e != list_end(&cond->waiters); e = list_next(e)) {
            // get the semaphore_elem sem_e that's been saved by threads waiting for condition
            struct semaphore_elem* sem_e = list_entry(e, struct semaphore_elem, elem);
            ASSERT(!list_empty(&sem_e->semaphore.waiters));
            // access thread by checking the semaphore waiting lists which is accessible by semaphore_elem
            struct thread* t = list_entry(list_front(&sem_e->semaphore.waiters), struct thread, elem);
            // if the thread's priority is bigger than current, update max priority, max_sem_elem
            if (t->priority > max_priority) {
                max_priority = t->priority;
                max_sem_elem = e;
            }
        }
        // remove the corresponding sem_elem from the condition waiting list
        list_remove(max_sem_elem);
        // wake up the sleeping thread by sema_up which unblocks the thread sleeping in it's own initialized semaphore
        // waiting list
        sema_up(&list_entry(max_sem_elem, struct semaphore_elem, elem)->semaphore);
    }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition* cond, struct lock* lock)
{
    ASSERT(cond != NULL);
    ASSERT(lock != NULL);

    while (!list_empty(&cond->waiters))
        cond_signal(cond, lock);
}
