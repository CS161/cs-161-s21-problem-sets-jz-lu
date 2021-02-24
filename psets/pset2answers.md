CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------
### Part C: Design of ppid synchronization
We add three `struct proc` member variables: `proc::ppid_`, `proc::childpids_[NPROC]`, and `proc::nchildren_`. (Strictly speaking `nchildren_` isn't necessary since we could always traverse the array and stop when the value is zero, but it eliminates the need to walk through the array every time we need to check the number of children left for `waitpid()`.) `fork()` adds the new child `proc::id_` to its `proc::childpids_[]` array, incrementing `proc::nchildren_`, and sets the initial `proc::ppid_` of the child before it is run, so no synchronization is needed there. In exit, under synchronization described below, the exiting process' parent's `parent->childpids_[]` array is walked through until the exited child's pid is found in `O(c)` time, since the loop break condition is `index < nchildren_`, and it is deleted and `parent->nchildren_` is decremented. (`exit()` also calls `wake_one(parent)` for purposes of part G.) The primary use for the child array is for the `pid==0` case in `waitpid` where we must walk through the children and returnt he first exited one.

To synchronize, we use the coarser-grained `ptable` lock. Since `fork()` and `exit()` require `ptable_lock` to be held anyway, due to modification of the process table, it is convenient to extend that to the `ppid` case. Thus in order to read/write any of the three metadata above, `ptable_lock` must be held. We thus place them in `exit()` and `syscall_getppid()` (`fork()` was already locked).

It is possible in build a per-process lock instead, but that would require `exit()` to hold three locks (`ptable`, `wait_queue` when calling `wake_one(parent)`, and the new `proc::p_lock`) at different places and would be difficult to debug, so we dispense with that method in this problem set.


### Part F
We counted the number of times the scheduler called `resume()` using the naive polling strategy implemented before part F and with the true blocking after F. Implementing true blocking had the following effects on the number of calls to `resume()` (does not include resumptions from exceptions and syscalls, only from the scheduler, since that is what true blocking primarily affects).
1. `msleep()`: `resume()`d 126151 times under polling, 714 times under blocking.
2. `waitpid()`: `resume()`d 2462 times under polling, 1344 times under blocking.
3. Child signal interrupts: `resume()`d 3322 times under polling, 1810 times under blocking.
In all cases, but especially in `msleep()`, one observes a dramatic reduction in wasteful work done by the CPU via the calls to `resume()`.
Note that our implementation in signaling uses a `wake_one()` extra wait queue function we created, which further reduces the number of wakes and resumptions for child signaling.

Grading notes
-------------
Halting: due to compile-time flags, it is usually necessary to `make clean` before running `make HALT=1 run-testhalt` before testing halting, and then cleaning again after if you don't want halt to exit for subsequent tests. If QEMU doesn't exist like you expect, try cleaning and recompiling, which worked for us.