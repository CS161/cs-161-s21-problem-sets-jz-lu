CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------
### Part C: Design of ppid synchronization
We add three `struct proc` member variables: `proc::ppid_`, `proc::childpids_[NPROC]`, and `proc::nchildren_`. (Strictly speaking `nchildren_` isn't necessary since we could always traverse the array and stop when the value is zero, but it eliminates the need to walk through the array every time we need to check the number of children left for `waitpid()`.) `fork()` adds the new child `proc::id_` to its `proc::childpids_[]` array, incrementing `proc::nchildren_`, and sets the initial `proc::ppid_` of the child before it is run, so no synchronization is needed there. In exit, under synchronization described below, the exiting process' parent's `parent->childpids_[]` array is walked through until the exited child's pid is found in `O(c)` time, since the loop break condition is `index < nchildren_`, and it is deleted and `parent->nchildren_` is decremented. (`exit()` also calls `wake_one(parent)` for purposes of part G.) The primary use for the child array is for the `pid==0` case in `waitpid` where we must walk through the children and returnt he first exited one.

To synchronize, we use the coarser-grained `ptable` lock. Since `fork()` and `exit()` require `ptable_lock` to be held anyway, due to modification of the process table, it is convenient to extend that to the `ppid` case. Thus in order to read/write any of the three metadata above, `ptable_lock` must be held. We thus place them in `exit()` and `syscall_getppid()` (`fork()` was already locked).

It is possible in build a per-process lock instead, but that would require `exit()` to hold up to three locks (`ptable`, a time heap lock if using the heap, and the new `proc::p_lock`) at different places and would be difficult to debug, so we dispense with that method in this problem set.

### Part D
We again use the `ptable_lock`, this time to check for the process state of a given `struct proc`; refer to the above for discussion.

### Part F
The child cannot wake up the parent in `exit()`, for if it did it would incur a race condition wherein the child may be in the middle of executing `yield_noreturn()` on the kernel stack while the parent frees the child `struct proc` in waitpid. Thus we define a new state, `proc::ps_transition` that the child becomes in exiting, and once the child finishes yielding and`schedule()` is called on a CPU stack, the scheduler changes the state to `proc::ps_free` and wakes up the parent.

We counted the number of times the scheduler called `resume()` during the various `make run-testXXX` using the naive polling strategy implemented before part F and with the true blocking after F. *To run this yourself, turn on `TRUEBLOCK_TESTING` in `kernel.hh` and run; turn on `USING_PSEUDO_BLOCKING` as well to test the polling case.* Implementing true blocking had the following effects on the number of calls to `resume()` (does not include resumptions from exceptions and syscalls, only from the scheduler, since that is what true blocking primarily affects).
1. `testmsleep`: `resume()`d 100771 times under polling, 710 times under blocking. Using our time heap (see *Extra Credit* in grading notes) the number of calls reduced to about 407, which is a further improvement.
2. `testwaitpid`: `resume()`d 2462 times under polling, 1344 times under blocking.
3. `testeintr`: `resume()`d 3322 times under polling, 1768 times under blocking.
In all cases, but especially in `msleep()`, one observes a dramatic reduction in wasteful work done by the CPU via the calls to `resume()`.
Note that our implementation in signaling uses a `wake_one()` extra wait queue function we created, which further reduces the number of wakes and resumptions for child signaling.

Grading notes
-------------
Halting: due to compile-time flags, it is necessary to `make clean` before running `make HALT=1 run-testhalt` before testing halting, and then cleaning again after if you don't want halt to exit for subsequent tests. If QEMU doesn't exist like you expect, try cleaning and recompiling, which worked for us.

**Extra Credit Attempts**:
1. Stack alignment (retroactive): in `k-exception.S` we added assembly-level assertions, which are of the form below. In essence, for an entry point entered via a `call`, there will be a return address pushed onto the stack before the entry is made, and therefore upon function entry the stack pointer is 8 bytes off 16 aligned if and only if it is exactly 16 byte aligned before the `call` instruction. Therefore we use the following assertion for `call` (i.e. to `exception` and `syscall`).
```
// check stack alignment
testq $0xF, %rsp               // check rsp is 16 aligned (call adds 8 before entry)
jnz panic_nonrunnable
call _ZN4proc9exceptionEP8regstate
```
If instead a `jmp` is made, no return address is pushed on and therefore the stack pointer just before the `jmp` should be 8 bytes off of 16 aligned, since it is the function entry point. Thus we use the following modified assert.
```
// Check stack alignment
addq $0x8, %rsp
testq $0xF, %rsp
jnz panic_nonrunnable
subq $0x8, %rsp         // Check rsp is 8 off 16 aligned
jmp _Z12kernel_startPKc
```
Two places, `init_ap()` and `schedule()` were not properly aligned; they were 8 bytes off. Thus we fix it by subtracting `%rsp` by 8 bytes before the entry point is hit. Since these are both jumps and will not return, there is no need to worry about adding 8 bytes back to `%rsp` upon return.
```
// check (AND FIX!) stack alignment first
testq $0xF, %rsp
jnz panic_nonrunnable
subq $0x8, %rsp                 // Since jmp instead of call should be 8 bytes off 16 aligned
// do the jump
```

2. Time heap: in addition to implementing the time wheel, which is used by default, we implemented a binary time heap. The data structures are given in `k-waitstruct.hh` and defined in `k-wait.hh`. Since most of the implementation follows a standard binary heap, we refrain from discussing the details here. The key detail is that unlike a normal heap popping results in the popped waiter being woken up. In the timer interrupt handler, we flush the heap to wake relevant sleeping processes whose time has expired, and in `exit()` we flush as well to wake processes whose children have set a child interrupt according to part G. To use the time heap, turn on `USING_TIME_HEAP` in `kernel.hh`; the `struct wait_heap time_heap` is defined atop `kernel.cc`. Note that testing it with `make run-testeintr` will very rarely (1 in 20 or 30 or so) cause a failure due to overhead latency of the heap, which is essentially due to the inherent child signaling race condition that we are free to disregard according to the problem set.
```
struct wait_heap {
    int nwaiters_ = 0;                               // Size of the heap
    mutable spinlock lock_;
    hwaiter* waiter_arr_[WAITNPROC] = {0};           // Array representation of heap
    inline void swap(hwaiter** w1, hwaiter **w2);
    inline int size();
    inline int size_under_lock();                    // Get size under lock
    inline void show();                              // Debugging purposes only
    inline int left(int parent);
    inline int right(int parent);
    inline int parent(int child);
    inline void heapify(int index);

    inline void insert(hwaiter* w);
    inline bool is_on_heap(hwaiter* w);              // Check if a waiter is on the heap
    inline uint64_t top_waketime();
    inline hwaiter* pop(bool wake);                  // Pop a waiter; wake it if wake=true
    inline hwaiter* lock_and_pop(bool wake);
    inline void flush(bool wake);                    // Flush the heap; wake them if wake=true
};
```