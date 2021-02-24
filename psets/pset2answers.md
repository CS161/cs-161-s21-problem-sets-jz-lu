CS 161 Problem Set 2 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

Answers to written questions
----------------------------
### Part C: Design of synchronization


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