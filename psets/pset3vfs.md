CS 161 Problem Set 3 VFS Design Document
========================================

1. Structures
The `struct proc` will have a new member `fdtable[MAX_FDT]` of `MAX_FDT = 8` entries. The first three entries are reserved respectively for `stdin, stdout, stderr` to the terminal, and the rest are free. The constructor of a `struct proc` will initialize the first three entries to the appropriate pointers if the entries are null, and `fork()` is tasked with copying the fd table entries (after the first 3) to children.
```c++
struct proc {
    // Member variables...
    int fdtable[MAX_PFDT] = {0}
    proc() {
        if (!(fdtable[0] && fdtable[1] && fdtable[2])) {
            // Initialize to the special keyboard-console nodes.
            fdtable[0] = fdtable[1] = fdtable[2] = (void*) &global_cnode;
        }
    }
    ~proc() {
        assert(cnode->refcount >= 0);
        for (int i = 0; i < MAX_FDT; ++i) {
            syscall_close(i); // Close all files on the way out
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
    // Locks declared in inheriters.
}

struct kb_c_vnode:public vnode {
    keyboardstate* cf_; // console file, lock defined in here

    uintptr_t cf_write(uintptr_t addr, size_t sz);
    uintptr_t cf_read(uintptr_t addr, size_t sz);
} global_cnode;

struct memfile_vnode:public vnode {
    memfile* mf_; // in-memory file
    spinlock mf_lock_; // protects functions below

    memfile_vnode(memfile* mf) {
        mf_ = mf;
    }

    uintptr_t mf_write(uintptr_t addr, size_t sz);
    uintptr_t mf_read(uintptr_t addr, size_t sz);
};
```
The per-process fd tables are of course dynamically allocated in `struct proc`; the constructor is to do work as described above and the destructor is to walk through the fd table and "free" the vnodes (that is, it should walk through the table, decrement the `refcount` of each non-null `vnode`, and subsequently call `kfree` if the refcount is updated to 0 and the vnode is not to terminal). `vnodes` is dynamically allocated and freed via the slab allocator created from PSet 1 Extra Credit. It is the job of `kernel_start()` to allocate the vnode for the console; it is stored in a global pointer `void* CONSOLE_VNODE` for easy access.

2. VFS functionalities
The constructors and destructors for the nodes are defined above. *All of the below read/write functions assume that validation was done by the corresponding `syscall`.* The specifics are below, but the end of every function will increment offset, and unlock before returning.

`cf_write(uintptr_t addr, size_t sz)`: locks the node and executes the current code in `syscall_write` with minor modifications.

`cf_read(uintptr_t addr, size_t sz)`: locks the node and executes the current code in `syscall_read` with minor modifications (i.e. removing the intermediate locks). 

`mf_write(uintptr_t addr, size_t sz)`: locks the node, using the `memfile` structure, compute the `wr_sz = min(capacity_ - len_, sz)`, and do a `memcpy` of `wr_sz` bytes from `addr` to `memfile::data_`. Increment `memfile::len_` by `wr_sz`.

`mf_read(uintptr_t addr, size_t sz)`: locks the node, using the `memfile` structure, compute the `rd_sz = min(capacity_ - ((unsigned char*) addr - data_), sz)`, and do a `memcpy` of `rd_sz` bytes from `memfile::data_` to `addr`.

3. Syscall functionalities and add-ins to current functions

`syscall_open(filename, mode)` checks that there is an available entry in the process fd table. Parses the `mode` to extract `O_CREAT` and `O_TRUNC` as `bool`s, then uses the `memfs::initfs_lookup(filename, create)` to generate the proper fd. Uses the return index into the `initfs` array and extracts a `memfile*` pointer. Checks that the `fdtable[]` does not already have a vnode of the same mode and `memfile*` pointer; return error if so. Declares a `mf_vnode` pointer and allocates memory via the slab allocator, passing in the `memfile*` pointer. Adds the vnode pointer into the next available slot in the `fdtable[]` and returns the index.

`syscall_read(fd, addr, sz)` asserts (memory range permissions/overflow, and) that the input fd points to a valid read-`vnode` in the fd table, and then calls one of two VFS `read()` functions based on the input fd. Calls `fdtable[fd]->read(addr, sz)`.

`syscall_write()` asserts (memory range permissions/overflow, and) that the input fd points to a valid write-`vnode` in the fd table, and then calls one of two VFS `write()` functions based on the input fd. Calls `fdtable[fd]->write()` after casting it appropriately.

`syscall_close(fd)` asserts that the fd indexes to a non-null, open file in the process `fdtable[]`. Supposing checks pass, decrement the refcount accordingly, and if the refcount hits zero afterwards, call `kfree()` on the `vnode` pointer. Set `fdtable[fd] = nullptr`.

`syscall_dup2(oldfd, newfd)` checks that `newfd` and `oldfd` are acceptable numbers (i.e. not out of array index range) and that `oldfd` indexes to a non-null value; if either is the case return `E_BADF`. Check that `newfd` is null; if it is not, then call `syscall_close(newfd)` before proceeding. Sets `proc::fdtable[newfd] = proc::fdtable[oldfd]`. Returns `newfd` upon success.

`syscall_fork()` updates to copy the per-process file descriptor table from parent to child. This is a set of pointers, so it will be a deep copy of the pointers.

Note that no additional per-`struct proc` locking is necessary here, even in the multithreaded case, since every (kernel-visible) thread has its own `struct proc` and the current thread is in the middle of the present syscall.

4. Synchronization and locking


5. Future work

6. Concerns

If multiple processes/threads are writing to the same file, should they have their own copy of a vnode, since the offsets can and should in the general case be different? In this case the locks wouldn't be the same unless we defined the lock at the memfs level and not the vnode level, and the two processes would race all over each other.

Should we cast the vnode type dynamically by checking the `fd`, or is there a more elegant solution?

What does "offset" mean in terminal context?
