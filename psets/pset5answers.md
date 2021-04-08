CS 161 Problem Set 5 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset5collab.md`.

Answers to written questions
----------------------------

## Changes to VFS and Chickadee FS
From Problem Set 3, our setup of the per-process file descriptor table `int proc::fdtable[]` was a static array within `struct proc`. However, since threads each have their own `struct proc` but must share a `struct proc`, we replace the static array with `int* proc::fdtable`, which like `struct cwd` is to be allocated by the creator of the `struct proc`. As such, during `boot_process_start()` and `syscall_fork()` we can explicitly allocate (and check for allocation failures) `proc::fdtable`, while in the new `syscall_clone()` we can pass in the pointer to the new `struct proc` for sharing between threads. We increased `MAXFD` from `8` to `16` since with multiple threads it is reasonable to want more entries.

Since it is likely that users would want each thread to be able to traverse directories independently, we give each thread its own `cwd`; in other words, working directories are a per-thread structure.

Grading notes
-------------
