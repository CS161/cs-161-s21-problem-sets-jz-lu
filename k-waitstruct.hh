#ifndef CHICKADEE_K_WAITSTRUCT_HH
#define CHICKADEE_K_WAITSTRUCT_HH
#include "k-list.hh"
#define WAITNPROC 16 // Redefinition from kernel.hh to avoid including everything
struct proc;
struct wait_queue;
struct wait_heap;
struct spinlock;
struct irqstate;
struct spinlock_guard;

// k-waitstruct.hh
//    Includes the struct definitions for `waiter` and `wait_queue`.
//    The inline functions declared here are defined in `k-wait.hh`.

struct waiter {
    proc* p_ = nullptr;             // Process that this waiter is serving
    wait_queue* wq_;                // Parent queue that this waiter belongs to
    list_links links_;              // Linked list implementation variable for wait_queue

    explicit inline waiter();
    inline ~waiter();
    NO_COPY_OR_ASSIGN(waiter);
    inline void prepare(wait_queue& wq);
    inline void block();
    inline void clear();
    inline void wake();

    template <typename F>
    inline void block_until(wait_queue& wq, F predicate);
    template <typename F>
    inline void block_until(wait_queue& wq, F predicate,
                            spinlock& lock, irqstate& irqs);
    template <typename F>
    inline void block_until(wait_queue& wq, F predicate,
                            spinlock_guard& guard);
};

struct wait_queue {
    list<waiter, &waiter::links_> q_;
    mutable spinlock lock_;

    inline void wake_all();
    inline void wake_one(proc* p);
    inline void show();
};

struct hwaiter {
    proc* p_ = nullptr;             // Process that this waiter is serving
    uint64_t wakeup_time_ = 0;      // Wakeup time for this process
    wait_heap* wh_;                 // Parent queue that this waiter belongs to
    list_links links_;              // Linked list implementation variable for wait_queue

    explicit inline hwaiter();
    inline ~hwaiter();
    NO_COPY_OR_ASSIGN(hwaiter);
    inline void prepare(wait_heap& wh, uint64_t wakeup_time);
    inline void block();
    inline void clear();
    inline void wake();

    template <typename F>
    inline void block_until(wait_heap& wh, uint64_t wakeup_time, F predicate);
    template <typename F>
    inline void block_until(wait_heap& wh, uint64_t wakeup_time, F predicate,
                            spinlock& lock, irqstate& irqs);
    template <typename F>
    inline void block_until(wait_heap& wh, uint64_t wakeup_time, F predicate,
                            spinlock_guard& guard);
};


// Binary heap implementation for waiting processes.
struct wait_heap {
    int nwaiters_ = 0;
    mutable spinlock lock_;
    hwaiter* waiter_arr_[WAITNPROC] = {0};
    inline void swap(hwaiter** w1, hwaiter **w2);
    inline int size();
    inline void show(); // Debugging purposes only
    inline int left(int parent);
    inline int right(int parent);
    inline int parent(int child);
    inline void heapify(int index);

    inline void insert(hwaiter* w);
    inline bool is_on_heap(hwaiter* w);
    inline uint64_t top_waketime();
    inline hwaiter* pop(bool wake);
};


#endif
