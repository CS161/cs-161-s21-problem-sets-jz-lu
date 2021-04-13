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
        wq.show();
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
    wq.q_.push_back(this);
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[waiter] Enqueued into the wait queue\n");
        wq.show();
    }

    // Unlock the wait queue.
    wq.lock_.unlock(irqs);
}

inline void waiter::block() {
    assert(p_ == current());

    if (p_->pstate_ == proc::ps_blocked) {
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[block] State blocked, yielding, ticks=%ld\n", (unsigned long) ticks);
        } 
        p_->yield();
    }
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[block] State ready to clear, clearing\n");
    }
    clear();
}

inline void waiter::clear() {
    // Lock the wait queue.
    if (WAITQ_PARANOIA >= 3) {
        log_printf("[clear] Clearing for process PID=%d\n", p_->id_);
    }
    auto irqs = wq_->lock_.lock();

    if (WAITQ_PARANOIA >= 2) {
        wq_->show();
    }

    // Wake up the process.
    wake();

    if (links_.is_linked()) {
        wq_->q_.erase(this);
        if (WAITQ_PARANOIA >= 2) {
            log_printf("[Clear] Found this waiter with PID=%d on queue, popping off\n",
                this->p_->id_);
        }
    }

    if (WAITQ_PARANOIA >= 2) {
        log_printf("[Clear] Clearing complete.\n");
        wq_->show();
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
            log_printf("[waiter] [block_until] About to prepare\n");
        }
        prepare(wq);
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] [block_until] Prepared. Predicate VA=%p\n", predicate);
        }
        if (predicate() || p_->exit_signal_) {
            if (WAITQ_PARANOIA >= 3) {
                log_printf("[k-wait] [block_until] Predicate immediately passed without block() called\n");
            }
            break;
        }
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] [block_until] About to block\n");
        }
        block();
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[k-wait] [block_until] Blocked\n");
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
        if (predicate() || p_->exit_signal_) {
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
    while (auto w = q_.pop_front()) {
        w->wake();
    }
}

// wait_queue::wake_one(p)
//    Lock the wait queue, then pop off a process if it matches the given one.
//    Does nothing if it cannot find the input process on the queue.
inline void wait_queue::wake_one(proc* p) {
    spinlock_guard guard(lock_);
    if (WAITQ_PARANOIA >= 2) {
        log_printf("[wake_all] [%p] Waking process VA=%p, pid=%d now\n", this, p, p->id_);
        show();
    }
    for (auto it = q_.front(); it; it = q_.next(it)) {
        if (it->p_ == p) {
            q_.erase(it);
            it->wake();
            break;
        }
    }
    if (WAITQ_PARANOIA >= 2) {
        show();
    }
}


// wait_queue::show()
//  Shows current state of the queue.
inline void wait_queue::show() {
    log_printf("WaitQ State: HEAD --> [");
    for (auto it = q_.front(); it; it = q_.next(it)) {
        log_printf("%d ", it->p_->id_);
    }
    log_printf("] <-- TAIL\n");
}

// ====== TIME HEAP STUFF ====== //

inline hwaiter::hwaiter() {
}

inline hwaiter::~hwaiter() {
    // optional error-checking code
}

inline void hwaiter::prepare(wait_heap& wh, uint64_t wakeup_time) {
    // Mark the waiter as serving the current process
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] [prepare] wh VA=%p, PA=0x%x\n", &wh, kptr2pa(&wh));
    }
    p_ = current();

    wh_ = &wh;
    wakeup_time_ = wakeup_time;
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] Set p_ to current(), with PID=%d and wakeup time %lu\n", 
            p_->id_, wakeup_time_);
    }

    // Lock the wait heap.
    auto irqs = wh_->lock_.lock();
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] locked\n");
    }
    p_->pstate_ = proc::ps_blocked;
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] Set p_->pstate_\n");
    }

    // Push the waiter onto the wait heap.
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] Inserting process PID=%d into wait heap\n", p_->id_);
    }
    wh_->insert(this);

    // Unlock the wait heap.
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] unlocking\n");
    }
    wh_->lock_.unlock(irqs);
}

inline void hwaiter::block() {
    assert(p_ == current());

    if (p_->pstate_ == proc::ps_blocked) {
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP] [block] yielding, ticks=%ld\n", (unsigned long) ticks);
        } 
        p_->yield();
    }
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP] [block] clearing\n");
    }
    clear();
}

inline void hwaiter::clear() {
    // Lock the wait heap.
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-clear] Clearing for process PID=%d\n", p_->id_);
    }
    auto irqs = wh_->lock_.lock();

    if (WAITH_PARANOIA >= 2) {
        wh_->show();
    }

    // Wake up the process.
    wake();

    // Pop the waiter off of the heap, if it is on.
    // (This is for design completeness--in practice this is never taken.)
    if (wh_->is_on_heap(this)) {
        if (WAITH_PARANOIA >= 2) {
            log_printf("[HEAP-clear] Found this waiter (PID=%d) on heap, clearing it\n", p_->id_);
        }
        hwaiter* temps[WAITNPROC] = {0};
        int ntemps = 0;
        for (int i = 0; i < wh_->size(); ++i) {
            hwaiter* hw = wh_->pop(false);
            if (hw == this) {
                break;
            } 

            temps[ntemps] = hw;
            ++ntemps;
        }
        for (int i = 0; i < ntemps; ++i) {
            wh_->insert(temps[i]);
        }
    }
    
    if (WAITH_PARANOIA >= 2) {
        log_printf("[HEAP-clear] Clearing complete\n");
        wh_->show();
    }

    // Unlock the wait heap.
    wh_->lock_.unlock(irqs);
}

inline void hwaiter::wake() {
    p_->wake();
}

// hwaiter::block_until(wh, wakeup_time, predicate)
//    Block on `wq` until `predicate()` returns true.
template <typename F>
inline void hwaiter::block_until(wait_heap& wh, uint64_t wakeup_time, F predicate) {
    while (true) {
        if (WAITQ_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] About to prepare process PID=%d\n", p_->id_);
        }
        prepare(wh, wakeup_time);
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] Prepared PID=%d. Predicate VA=%p\n", p_->id_, predicate);
        }
        if (predicate() || p_->exit_signal_) {
            if (p_->thgrp_->e_intr_ != 0) {
                log_printf("[HEAP-waiter] Predcheck passed. Signal to parent PID=%d\n", p_->id_);
            }
            break;
        }
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] Predcheck failed: About to block PID=%d\n", 
                p_->id_);
        }
        block();
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] Blocked PID=%d\n", p_->id_);
        }
    }
    clear();
}

// hwaiter::block_until(wh, wakeup_time, predicate, lock, irqs)
//    Block on `wq` until `predicate()` returns true. The `lock`
//    must be locked; it is unlocked before blocking (if blocking
//    is necessary). All calls to `predicate` have `lock` locked,
//    and `lock` is locked on return.
template <typename F>
inline void hwaiter::block_until(wait_heap& wh, uint64_t wakeup_time, F predicate,
                                spinlock& lock, irqstate& irqs) {
    while (true) {
        prepare(wh, wakeup_time);
        if (predicate() || p_->exit_signal_) {
            break;
        }
        lock.unlock(irqs);
        block();
        irqs = lock.lock();
    }
    clear();
}

// hwaiter::block_until(wh, wakeup_time, predicate, guard)
//    Block on `wq` until `predicate()` returns true. The `guard`
//    must be locked on entry; it is unlocked before blocking (if
//    blocking is necessary) and locked on return.
template <typename F>
inline void hwaiter::block_until(wait_heap& wh, uint64_t wakeup_time, F predicate,
                                spinlock_guard& guard) {
    block_until(wh, wakeup_time, predicate, guard.lock_, guard.irqs_);
}




// swap(w1, w2)
//   Helper function. Swaps heap waiter pointers. Assumes locked.
void wait_heap::swap(hwaiter** w1, hwaiter** w2) {
    assert(lock_.is_locked());
    hwaiter* temp = *w1; 
    *w1 = *w2; 
    *w2 = temp; 
}

// wait_heap::size()
//    Returns num waiters on heap. Assumes locked.
inline int wait_heap::size() {
    assert(lock_.is_locked());
    return nwaiters_;
}

// wait_heap::size_under_lock()
//    Returns num waiters on heap.
inline int wait_heap::size_under_lock() {
    spinlock_guard guard(lock_);
    return nwaiters_;
}

// wait_heap::show()
//  Shows current state of the heap, in array representation. Assumes locked.
inline void wait_heap::show() {
    if (WAITH_PARANOIA < 3) {
        return;
    }
    assert(lock_.is_locked());
    log_printf("WaitH State (PID:waketime) :: ROOT --> [");
    for (int i = 0; i < nwaiters_; ++i) {
        log_printf("%d:%d ", waiter_arr_[i]->p_->id_, waiter_arr_[i]->wakeup_time_);
    }
    log_printf("] <-- LEAVES\n");
}

// wait_heap::left(int parent)
//    Returns left child index of parent. Assumes locked.
inline int wait_heap::left(int parent) {
    assert(lock_.is_locked());
    return 2*parent + 1;
}

// wait_heap::right(int parent)
//    Returns right child index of parent. Assumes locked.
inline int wait_heap::right(int parent) {
    assert(lock_.is_locked());
    return 2*parent + 2;
}

// wait_heap::parent(int child)
//     Returns parent index of a node. Assumes locked.
inline int wait_heap::parent(int child) {
    assert(lock_.is_locked());
    return (child - 1)/2;
}

// wait_heap::heapify(i)
//    Heapify the wait_heap with respect to index i.
//    The heap algorithm assumes children of i are min-heaped. Assumes locked.
inline void wait_heap::heapify(int i) {
    assert(lock_.is_locked());
    if (WAITH_PARANOIA >= 3) {
        log_printf("[wait_heap] Heapifying on index %d. %d waiters total,\n", i, nwaiters_);
        show();
    }
    assert(i >= 0);
    int l = left(i); 
    int r = right(i); 
    int smallest = i; 
    if (l < nwaiters_ && waiter_arr_[l]->wakeup_time_ < waiter_arr_[i]->wakeup_time_) {
        smallest = l;
    }
    if (r < nwaiters_ && waiter_arr_[r]->wakeup_time_ < waiter_arr_[smallest]->wakeup_time_) {
        smallest = r; 
    }
    if (smallest != i) { 
        swap(&waiter_arr_[i], &waiter_arr_[smallest]); 
        heapify(smallest); 
    }
}

// wait_heap::insert(w)
//    Inserts a new waiter into the heap. Assumes locked.
inline void wait_heap::insert(hwaiter *hw) {
    assert(lock_.is_locked());
    assert(nwaiters_ < WAITNPROC); 
    if (WAITH_PARANOIA >= 3) {
        log_printf("[wait_heap] Inserting waiter with wakeup time %lu\n", hw->wakeup_time_);
        show();
    }
  
    // Add the new waiter.
    int i = nwaiters_++;
    waiter_arr_[i] = hw;
  
    // Move new node up until heap is consistent.
    while (i != 0 && waiter_arr_[parent(i)]->wakeup_time_ > waiter_arr_[i]->wakeup_time_) { 
       swap(&waiter_arr_[i], &waiter_arr_[parent(i)]);
       i = parent(i);
    } 

    if (WAITH_PARANOIA >= 3) {
        log_printf("[wait_heap] Insertion complete. New heap state below.\n");
        show();
    }
}

// is_on_heap(hwaiter* w)
//    Checks if a particular waiter is on the heap. Assumes locked.
inline bool wait_heap::is_on_heap(hwaiter* hw) {
    assert(lock_.is_locked());
    for (int i = 0; i < nwaiters_; ++i) {
        if (waiter_arr_[i] == hw) {
            return true;
        }
    }
    return false;
}

// wait_heap::top_waketime()
//    Gets the wakeup time of the top of the heap.
inline uint64_t wait_heap::top_waketime() {
    spinlock_guard guard(lock_);
    if (!nwaiters_) {
        if (WAITH_PARANOIA >= 2) {
            log_printf("[wait-heap] top_waketime called but no waiters are on the heap\n");
        }
        return 0;
    }
    if (WAITH_PARANOIA >= 2) {
        log_printf("[wait_heap] Showing top wakeup time: proc PID=%d, time=%d\n",
            waiter_arr_[0]->p_->id_, waiter_arr_[0]->wakeup_time_);
        show();
    }
    return waiter_arr_[0]->wakeup_time_;
}

// wait_heap::pop()
//    Pops off the top waiter and calls wake. Assumes locked.
inline hwaiter* wait_heap::pop(bool wake) {
    assert(lock_.is_locked());
    if (!nwaiters_) {
        return nullptr;
    }
    if (WAITH_PARANOIA >= 2) {
        log_printf("[wait_heap] Popping process PID=%d with wakeup time %d now\n", 
            waiter_arr_[0]->p_->id_, waiter_arr_[0]->wakeup_time_);
        show();
    }
  
    // Pop off the min.
    hwaiter *root = waiter_arr_[0];
    if (nwaiters_ == 1) {
        --nwaiters_;
    }
    else {
        waiter_arr_[0] = waiter_arr_[--nwaiters_];
        if (WAITH_PARANOIA >= 3) {
            log_printf("[wait_heap] [pop] Heapifying on 0 with %d remaining waiters\n", nwaiters_);
        }
        heapify(0); 
    }

    // Wake the process.
    if (wake) {
        if (WAITH_PARANOIA >= 2) {
            log_printf("[wait_heap] Waking process PID=%d with wakeup time %d now\n",
            root->p_->id_, root->wakeup_time_);
        }
        root->wake();
    }

    if (WAITH_PARANOIA >= 2) {
        show();
    }

    return root;
}

// wait_heap::lock_and_pop()
//    Pops off the top waiter and calls wake.
inline hwaiter* wait_heap::lock_and_pop(bool wake) {
    spinlock_guard guard(lock_);
    return pop(wake);
}

// wait_heap::flush()
//    Empties entire heap, waking processes if desired.
inline void wait_heap::flush(bool wake) {
    spinlock_guard guard(lock_);
    if (!wake) {
        nwaiters_ = 0;
        return;
    }
    for (int i = 0; i < nwaiters_; ++i) {
        waiter_arr_[i]->wake();
    }
    nwaiters_ = 0;
}

#endif
