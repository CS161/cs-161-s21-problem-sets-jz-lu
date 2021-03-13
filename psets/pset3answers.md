CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

## Questions


Answers to written questions
----------------------------
## Changes to the design document
### Part B
1. We neglected to mention that `fork()` should also increment the `refcount` of each copied `vnode`.
2. At the time of design we did not think about process VFS cleanup upon exit. `syscall_exit()` must walk through the `proc::fdtable[]` and close all open file descriptors in the same way `syscall_close()` does it.
3. The `kb_c_vnode` does not require a pointer to the keyboard and console states as the document stated, as they are global singletons.

### Part C
The bounded buffer implementation follows closely the implementation from CS 61 linked on the problem set statement; as such, we refrain from restating all of the logic for the bounded buffer. We built the pipe `vnode` system in our design document early on, so any updates are below.
1. In the document it was decreed that the writer pipe `vnode` should allocate and free the buffer. It turns out the natural way is slightly more complicated. We added two additional boolean metadata to the bounded buffer: `read_closed` and `write_closed`. The constructor of the `write` class shall as defined in the design document allocate the bounded buffer, but the destructor shall only free the buffer if both of the above booleans are true. The destructor for the write end shall, all under the shared bounded-buffer lock, set `write_closed` to true, then check if the dual `read_closed` is true, freeing `bbuf` if so. Otherwise, it will just exit. The read end destructor does the same, switching the bools above.
2. In the document we declared that the `close` syscall would call `kfree()`. Since we need destructors to be called, `delete` will be used instead. Due to the specifications of the C++ linker, the destructor of the base `vnode` struct is declared as virtual and all virtual functions in the base struct are subsequently defined, as the struct is no longer pure virtual.

### Part D
Updates to the design:
1. Validation of the filename string is a priori ambiguous because the size of the string isn't passed explicitly. If the user gives a gigantic string that has no end (probably on accident by passing in a pointer to unintended chunks of memory) we should fail. As such we set the maximum filename size to `MAX_FILENAME_LEN = 64` in `kernel.hh` to match that of `memfile::namesize`. We then compute the size by walking through the string until we find the null terminator or if we determine the string is too long. Given a size from there, we can call the I/O validator from Part A to finish validation.

**Locking strategy**: TODO.

Grading notes
-------------
