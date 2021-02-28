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
        log_printf("[clear] Clearing for process PID=%d\n", p_->id_);
    }
    auto irqs = wq_->lock_.lock();

    if (WAITQ_PARANOIA >= 2) {
        wq_->show();
    }

    // Wake up the process.
    wake();

    // Dequeue the waiter off the the wait queue, if it is enqueued.
    for (waiter *it = wq_->q_.front(); it; it = wq_->q_.next(it)) {
        if (it == this) {
            wq_->q_.erase(it);
            break;
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
        log_printf("[wake_all] [%s] Waking all now.\n", 
            this == &parent_child_queue ? "PCQ" : "TW");
    }
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
    }
    for (auto it = q_.front(); it; it = q_.next(it)) {
        if (it->p_ == p) {
            q_.erase(it);
            it->wake();
            break;
        }
    }
}


// wait_queue::show()
//  Shows current state of the queue.
inline void wait_queue::show() {
    log_printf("WaitQ State: HEAD --> [");
    for (auto it = q_.front(); it; it = q_.next(it)) {
        log_printf("%i ", it->p_->id_);
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
        log_printf("[HEAP-waiter] Set p_ to current(), VA=%p, PA=0x%x\n", p_, kptr2pa(p_));
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

    // Push the waiter onto the wait queue.
    if (WAITH_PARANOIA >= 3) {
        log_printf("[HEAP-waiter] About to enqueue into the wait queue\n");
    }
    wh_->insert(this);

    // Unlock the wait queue.
    wh_->lock_.unlock(irqs);
}

inline void hwaiter::block() {
    assert(p_ == current(), "If the lettuce comes on top of the salad, I send it back!\n");

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
    // Lock the wait queue.
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
            log_printf("[HEAP-clear] Found this waiter on heap, clearing it\n");
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

    // Unlock the wait queue.
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
            log_printf("[HEAP-waiter] About to prepare\n");
        }
        prepare(wh, wakeup_time);
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] Prepared. Predicate VA=%p\n", predicate);
        }
        if (predicate()) {
            break;
        }
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] About to block\n");
        }
        block();
        if (WAITH_PARANOIA >= 3) {
            log_printf("[HEAP-waiter] Blocked\n");
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
        if (predicate()) {
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
//   Helper function. Swaps heap waiter pointers.
void wait_heap::swap(hwaiter** w1, hwaiter** w2) {
    hwaiter* temp = *w1; 
    *w1 = *w2; 
    *w2 = temp; 
}

// wait_heap::size()
//    Returns num waiters on heap
inline int wait_heap::size() {
    return nwaiters_;
}

// wait_heap::show()
//  Shows current state of the heap, in array representation.
inline void wait_heap::show() {
    log_printf("WaitH State: ROOT --> [");
    for (int i = 0; i < nwaiters_; ++i) {
        log_printf("%i ", waiter_arr_[i]);
    }
    log_printf("] <-- LEAVES\n");
}

// wait_heap::left(int parent)
//    Returns left child index of parent.
inline int wait_heap::left(int parent) {
    return 2*parent + 1;
}

// wait_heap::right(int parent)
//    Returns right child index of parent.
inline int wait_heap::right(int parent) {
    return 2*parent + 2;
}

// wait_heap::parent(int child)
//     Returns parent index of a node.
inline int wait_heap::parent(int child) {
    return (child - 1)/2;
}

// wait_heap::heapify(i)
//    Heapify the wait_heap with respect to index i.
//    The heap algorithm assumes children of i are min-heaped.
inline void wait_heap::heapify(int i) { 
    assert(i >= 0 && i < nwaiters_);
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
//    Inserts a new waiter into the heap.
inline void wait_heap::insert(hwaiter *hw) {
    assert(nwaiters_ < WAITNPROC); 
  
    // Add the new waiter.
    int i = nwaiters_++;
    waiter_arr_[i] = hw;
  
    // Move new node up until heap is consistent.
    while (i != 0 && waiter_arr_[parent(i)]->wakeup_time_ > waiter_arr_[i]->wakeup_time_) { 
       swap(&waiter_arr_[i], &waiter_arr_[parent(i)]);
       i = parent(i);
    } 
}

// is_on_heap(hwaiter* w)
//    Checks if a particular waiter is on the heap.
inline bool wait_heap::is_on_heap(hwaiter* hw) {
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
    if (!nwaiters_) {
        return 0;
    }
    return waiter_arr_[0]->wakeup_time_;
}

// wait_heap::wake_top()
//    Pops off the top waiter and calls wake.
inline hwaiter* wait_heap::pop(bool wake) {
    spinlock_guard guard(lock_);
    assert(nwaiters_ > 0);
  
    // Pop off the min.
    hwaiter *root = waiter_arr_[0];
    if (nwaiters_ == 1) {
        --nwaiters_;
    }
    else {
        waiter_arr_[0] = waiter_arr_[--nwaiters_]; 
        nwaiters_--; 
        heapify(0); 
    }

    // Wake the process.
    if (wake) {
        root->wake();
    }
    return root;
}


#endif
