CS 161 Problem Set 5 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

Answers to written questions
----------------------------
The answer to the Problem Set writeup prompt regarding synchronization is given in *Synchronization Invariants* below. The remainder is documentation about our thread design.

# Threads

## New Design of Thread Architecture
When implementing threads it becomes apparent that certain metadata are shared among a group of threads, while others are per-thread. Group-shared data include file descriptors, thread group ID (process ID), linked list of thread pointers, and a per-process lock. It is thus convenient to combine them all into one struct and pass pointers to it into new threads when cloning. Note that we generally want each thread to be able to traverse directories independently, so we give each thread its own `cwd`.
```c++
struct thgrp {
    spinlock proc_lock_;
    pid_t tgid_;                        // what used to be id_ (now id_ is thread ID)
    vnode* fdtable[MAXFD];              // file descriptors
    list<proc, &proc::thlink_> th;      // list of pointers to threads
    int nth = 1;                        // num threads
    // ... process hierarchy stuff
};
```
This design implies that `list_links thlink_` must be added to `struct proc`. See *Synchronization Invariants* for documentation on per-process locking strategies. We increase `MAXFD` from 8 to 16 since with multiple threads it is reasonable to want more entries. The constructor of `struct proc` now takes in a `thgrp` pointer in addition to a `cwd` pointer, incrementing `nth` and adding `this` to `th`. It is the job of `syscall_fork()` to initialize the thread group ID.

Henceforth we shall use `proc::id_` as the *thread ID*, and `sys_getpid()` will instad return `proc::thgrp::tgid_`. The per-thread ID shall be used to index `ptable`, so allocating and freeing IDs there does not change. Since the per-process ID no longer requires a specific bound (as it no longer serves as the index to any table), we can assign it by simply keeping a global atomic variable `curpid` that increments it; creating a new process will involve an ID assignment as below. This value will not be reset except at boot time.
```c++
// after allocation...
p->tgid_ = curpid++;
```


### Thread Exit
To exit a thread, decrement `thgrp::nth_`, add thread to list of zombies, and pop it off the list of active threads. If the calling thread is the only thread in the group, then texit just calls `syscall_exit()`; otherwise, just delete the current working directory `pwd_` and yield.


### Changes to Process Exit
To exit, the thread `t0` first locks and issues a "death call", which alerts all other threads in the group that they should stop what they are doing and die. The thread issues this call by changing an attribute of the thread `proc::exit_signal_` to `E_INTR`, then blocking until all threads have exited. The scheduler examines the current thread whenever it is triggered and upon detecting the signal, sets `proc::pstate_ = ps_exiting` and wakes up `t0`. As threads exit, `t0` cleans them by moving them to the zombie list and deleting their `struct cwd`. Threads that are blocked will also eventually receive the death call, as `waiter::block_until()` now checks both the predicate and the signal, and yields the process to the scheduler if the signal is detected. Using this method, all threads are eventually (not immediately, but still quickly and efficiently) killed so the exit can proceed. At this point the process is single-threaded, so the remainder of exit proceeds as before.


## Synchronization Invariants
1. `ptable_lock` must precede per-process thread group locks `thgrp::thgrp_lock_`; however, in almost all cases, except for 1-2 lines on occasion, the two locks will not be simultaneously held. 
2. The thread group `fdtable_, th_` must be protected by `thgrp::thgrp_lock_` after the process has become enqueued on a CPU, i.e. after `boot_process_start() / syscall_fork()` completes. Moreover, new page table mappings in `sys_page_alloc()` and other memory-allocating system calls require the thread group lock to avoid races. However, general accesses to the page table (except when synchronizing with the memviewer, which has already been handled from Problem Set 2) will not be locked. We checked earlier with the teaching staff, citing the fact that this would cause performance drawbacks if every memory access was locked and the fact that generally these sorts of race conditions were left to the user to synchronize, and they approved this design. Note that there can be no security issues arising with not locking the memory checking functions in the system calls; this might only occur if processes had the ability to free memory, but in Chickadee they do not, and by the time `syscall_exit()` executes frees no other thread may be running. This design would change if processes had a user-level memory-free system call.
3. `nth_` is atomic and may be changed or read (or `compare_exchange_strong()`) atomically by the scheduler and kernel tasks without holding a lock, if it is the only thing being changed and can be done in a single operation. However, the elements of `thgrp_` need not be locked if there is only one thread remaining. In particular, `boot_process_start(), syscall_fork(), syscall_execv()` need not do any per-`thgrp` locking at all, and `syscall_exit()` need not lock after it has woken up from waiting for all threads except for the calling thread to stop running.
3. To synchronize the exiting of all threads at once via `syscall_exit()`, we add a new state `proc::ps_exiting` and decree that once a process state has become `ps_exiting`, it may no longer become runnable again; `cpustate::schedule()` is endowed with the privilege of changing exit-signaled processes to have `pstate_ = ps_exiting`, which will be checked by `syscall_exit()`. Otherwise, the process state of `proc p` may only be changed by the kernel task for `p`, or by an atomic compare and exchange process, such as that given in `proc::wake()` which is not necessarily called by the kernel task `p`.


Grading notes
-------------
Extra, extra, extra, extra, ...., extra credit?
Introducing...Aakash-bot!
Type `make run-oracle` then try asking a question.