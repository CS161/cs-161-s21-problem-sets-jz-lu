CS 161 Problem Set 3 VFS Design Document
========================================

1. Structures
The `struct proc` will have a fd table of `MAX_PFDT = 8` entries. The first three entries are reserved respectively for `stdin, stdout, stderr` to the terminal, and the rest are free. The constructor of a `struct proc` will initialize the first three entries to the appropriate pointers if the entries are null, and `fork()` is tasked with copying the fd table entries (after the first 3) to children.
```c++
struct proc {
    // Member variables...
    int fdtable[MAX_PFDT] = {0}
    proc() {
        if (!(fdtable[0] && fdtable[1] && fdtable[2])) {
            // Initialize to the special keyboard-console nodes.
            // TODO
        }
    }
}
```
Each element of the fd table points to a `block` structure (either the special terminal I/O block `tty_block` or a generic `f_block`), which holds all of the important information.
```c++
#define O_RDONLY 0x1
#define O_WRONLY 0x2
#define O_RDWR   O_RDONLY | O_WRONLY

struct vnode {
    int mode_ = 0; // Read, write, or both
    off_t offset_ = 0; // Offset from file, to be incremented on read/writes
    int refcount_ = 0; // Number of processes with entry in fd table pointing here
    mutable spinlock vnlock_; // Lock the vnode
}

struct kb_c_vnode:public vnode {

}

struct file_vnode:public vnode {

}
```
2. VFS functionalities

3. Syscall functionalities and add-ins to current functions
`syscall_open(filename, mode)` checks that the file exists, that there is an available entry in the process fd table, and that the file is not already open in the particular mode. It then TODO

`syscall_read()`

`syscall_write()`

`syscall_close()`

`syscall_dup2(oldfd, newfd)` checks that `newfd` and `oldfd` are acceptable numbers (i.e. not out of array index range) and that `oldfd` indexes to a non-null value; if either is the case return `E_BADF`. Check that `newfd` is null; if it is not, then call `syscall_close(newfd)` before proceeding. Sets `proc::fdtable[newfd] = proc::fdtable[oldfd]`. Returns `newfd` upon success.

`syscall_fork()` updates to copy the per-process file descriptor table from parent to child. This is a set of pointers, so it will be a deep copy of the pointers.

Note that no additional per-`struct proc` locking is necessary here, even in the multithreaded case, since every (kernel-visible) thread has its own `struct proc` and the current thread is in the middle of the present syscall.

4. Synchronization and locking

5. Future work

6. Concerns
