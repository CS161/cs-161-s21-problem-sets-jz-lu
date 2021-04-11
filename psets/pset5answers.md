CS 161 Problem Set 5 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

Answers to written questions
----------------------------

## New Design of Thread Architecture
When implementing threads it becomes apparent that certain metadata are shared among a group of threads, while others are per-thread. Group-shared data include file descriptors, thread group ID (process ID), linked list of thread pointers, and a per-process lock. It is thus convenient to combine them all into one struct and pass pointers to it into new threads when cloning. Note that we generally want each thread to be able to traverse directories independently, so we give each thread its own `cwd`.
```c++
struct thgrp {
    spinlock proc_lock_;
    pid_t tgid_;                        // what used to be id_ (now id_ is thread ID)
    vnode* fdtable[MAXFD];                 // file descriptors
    list<proc, &proc::thlink_> th;       // list of pointers to threads
    int nth = 1;                        // num threads
};
```
This design implies that `list_links thlink_` must be added to `struct proc`. See *Synchronization Invariants* for documentation on per-process locking strategies. We increase `MAXFD` from 8 to 16 since with multiple threads it is reasonable to want more entries. The constructor of `struct proc` now takes in a `thgrp` pointer in addition to a `cwd` pointer, incrementing `nth` and adding `this` to `th`. It is the job of `syscall_fork()` to initialize the thread group ID.

Henceforth we shall use `proc::id_` as the *thread ID*, and `sys_getpid()` will instad return `proc::thgrp::tgid_`. The per-thread ID shall be used to index `ptable`, so allocating and freeing IDs there does not change. Since the per-process ID no longer requires a specific bound (as it no longer serves as the index to any table), we can assign it by simply keeping a global atomic variable `curpid` that increments it; creating a new process will involve an ID assignment as below.
```c++
// after allocation...
p->tgid_ = curpid++;
```


### Thread Exit


### Changes to Process Exit


## Synchronization Invariants
`ptable_lock` must precede per-process thread group locks `thgrp::thgrp_lock_`. The thread group `fdtable_, nth_, th_` must be protected by `thgrp::thgrp_lock_` after the process has become runnable, i.e. after `boot_process_start() / syscall_fork()`.


Grading notes
-------------
