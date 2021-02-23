#ifndef CHICKADEE_K_WAIT_HH
#define CHICKADEE_K_WAIT_HH
#include "kernel.hh"
#include "k-waitstruct.hh"

// k-wait.hh
//    Defines `waiter` and `wait_queue` member functions.
//    `k-waitstruct.hh` defines the `waiter` and `wait_queue` types.
//    (Separating the structures and functions into different header files
//    avoids problems with circular dependencies.)


inline waiter::waiter() {
}

inline waiter::~waiter() {
    // optional error-checking code
}

inline void waiter::prepare(wait_queue& wq) {
    // Mark the waiter as serving the current process
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] [prepare] wq VA=%p, PA=0x%x\n", &wq, kptr2pa(&wq));
    }
    p_ = current();
    wq_ = &wq;
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] Set p_ to current(), VA=%p, PA=0x%x\n", p_, kptr2pa(p_));
    }

    // Lock the associated wait queue.
    auto irqs = wq.lock_.lock();
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] locked\n");
    }
    p_->pstate_ = proc::ps_blocked;
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] Set p_->pstate_\n");
    }

    // Push the waiter onto the wait queue.
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] About to enqueue into the wait queue\n");
    }
    wq.q_.push_back(this);

    // Unlock the wait queue.
    wq.lock_.unlock(irqs);
}

inline void waiter::block() {
    assert(p_ == current(), "You have yee'd your last haw!\n");

    if (p_->pstate_ == proc::ps_blocked) {
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[block] yielding, ticks=%ld\n", (unsigned long) ticks);
        } 
        p_->yield();
    }
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[block] clearing\n");
    }
    clear();
}

inline void waiter::clear() {
    // Lock the wait queue.
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[clear] clearing\n");
    }
    auto irqs = wq_->lock_.lock();

    // Wake up the process.
    wake();

    // Dequeue the waiter off the the wait queue, if it is enqueued.
    for (waiter *it = wq_->q_.front(); it; it = wq_->q_.next(it)) {
        if (it == this) {
            wq_->q_.erase(it);
            break;
        }
    }

    // Unlock the wait queue.
    wq_->lock_.unlock(irqs);
}

inline void waiter::wake() {
    p_->wake();
}


// waiter::block_until(wq, predicate)
//    Block on `wq` until `predicate()` returns true.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate) {
    while (true) {
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] About to prepare\n");
        }
        prepare(wq);
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] Prepared. Predicate VA=%p\n", predicate);
        }
        if (predicate()) {
            break;
        }
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] About to block\n");
        }
        block();
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] Blocked\n");
        }
    }
    clear();
}

// waiter::block_until(wq, predicate, lock, irqs)
//    Block on `wq` until `predicate()` returns true. The `lock`
//    must be locked; it is unlocked before blocking (if blocking
//    is necessary). All calls to `predicate` have `lock` locked,
//    and `lock` is locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock& lock, irqstate& irqs) {
    while (true) {
        prepare(wq);
        if (predicate()) {
            break;
        }
        lock.unlock(irqs);
        block();
        irqs = lock.lock();
    }
    clear();
}

// waiter::block_until(wq, predicate, guard)
//    Block on `wq` until `predicate()` returns true. The `guard`
//    must be locked on entry; it is unlocked before blocking (if
//    blocking is necessary) and locked on return.
template <typename F>
inline void waiter::block_until(wait_queue& wq, F predicate,
                                spinlock_guard& guard) {
    block_until(wq, predicate, guard.lock_, guard.irqs_);
}

// wait_queue::wake_all()
//    Lock the wait queue, then clear it by waking all waiters.
inline void wait_queue::wake_all() {
    spinlock_guard guard(lock_);
    if (WAITQ_PARANOIA >= 2) {
        log_printf("[wake_all] [%p] Waking all now\n", this);
    }
    while (auto w = q_.pop_front()) {
        w->wake();
    }
}

#endif
