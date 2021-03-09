CS 161 Problem Set 3 VFS Design Document
========================================

1. Structures
The `struct proc` will have a new member `fdtable[MAX_FDT]` of `MAX_FDT = 8` entries. The first three entries are reserved respectively for `stdin, stdout, stderr` to the terminal, and the rest are free. The constructor of a `struct proc` will initialize the first three entries to the appropriate pointers if the entries are null, and `fork()` is tasked with copying the fd table entries (after the first 3) to children.
```c++
struct proc {
    // Member variables...
    void* fdtable[MAX_PFDT] = {0};
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
Each element of the fd table points to an inherited type of `vnode` structure, which holds all of the important information.
```c++
#define OF_RDONLY 0x1
#define OF_WRONLY 0x2
#define OF_RDWR   O_RDONLY | O_WRONLY
void* global_c_vnode;

T kernel_start() {
    // ...
    global_c_vnode = kalloc(sizeof(kb_c_vnode));
    assert(global_c_vnode);
    // ...
};

// ...

// Generic parent class that everyone inherits from.
struct vnode {
    int mode_ = 0; // Read, write, or both
    off_t offset_ = 0; // Offset from file, to be incremented on read/writes
    int refcount_ = 0; // Number of processes with entry in fd table pointing here
    // Locks declared in inheriters.

    vnode(int mode) {
        mode_ = mode;
    }

    bool readable() {
        return mode_ & O_RDONLY;
    }
    bool writeable() {
        return mode_ & O_WRONLY;
    }
    virtual uintptr_t write(uintptr_t addr, size_t sz);
    virtual uintptr_t read(uintptr_t addr, size_t sz);
}

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    // The kbd and csl are global, so copies need not be made here.

    kb_c_vnode() : vnode(O_RDWR) {}

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile* mf_; // in-memory file, lock defined in here

    memfile_vnode(int mode, memfile* mf)
        : vnode(mode) {
        mf_ = mf;
    }

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

#define BBUF_CAP    256
struct pipe_bbuf {
    char bbuf[BBUF_CAP];
    int len_ = 0; // buffer write length
    spinlock lock_;
}

struct pipe_vnode:public vnode {
    pipe_bbuf* bbuf_ = nullptr;
    pipe_vnode(int mode, pipe_bbuf* bbuf)
        : vnode(int mode) {
        assert(mode == O_RDONLY || mode == O_WRONLY);
        if (!bbuf) {
            bbuf_ = knew<pipe_bbuf>();
        } else {
            bbuf_ = bbuf; // read end should use same buf as write end
        }
    }

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};
```
The per-process fd tables are of course dynamically allocated in `struct proc`; the constructor is to do work as described above and the destructor is to walk through the fd table and "free" the vnodes (that is, it should walk through the table, decrement the `refcount` of each non-null `vnode`, and subsequently call `kfree` if the refcount is updated to 0 and the vnode is not to terminal). `vnodes` is dynamically allocated and freed via the slab allocator created from PSet 1 Extra Credit. It is the job of `kernel_start()` to allocate the vnode for the console; it is stored in a global pointer `void* global_c_vnode` for easy access.

2. VFS functionalities
The constructors and destructors for the nodes are defined above. *All of the below read/write functions assume that validation was done by the corresponding `syscall`.* The specifics are below, but the end of every function will increment offset, and unlock before returning.

Console `write(uintptr_t addr, size_t sz)`: executes the current code in `syscall_write` with any minor modifications.

Console `read(uintptr_t addr, size_t sz)`: executes the current code in `syscall_read` with any minor modifications.

Memfile `write(uintptr_t addr, size_t sz)`: locks the node, using the `memfile` structure, checks whether `sz` bytes of free memory is available, and increase the length of the file (and capacity, if needed, returning fail if out of space). Do a `memcpy` of `sz` bytes from `addr` to `memfile::data_`.

Memfile `read(uintptr_t addr, size_t sz)`: locks the node, using the `memfile` structure, computes the `rd_sz = min(memfile::len_ - offset_, sz)`, and do a `memcpy` of `rd_sz` bytes from `memfile::data_` to `addr`.

Pipe `write(uintptr_t addr, size_t sz)`: validates `sz <= BBUF_CAP` (return error if not) and locks the buffer, using the `pipe_bbuf` structure. Sleeps if the buffer is full. If the buffer empty space is insufficient, return error. Do a `memcpy` of `sz` bytes from `addr` to `memfile::data_`. Increment `pipe_bbuf::len_` by `wr_sz`.

Pipe `read(uintptr_t addr, size_t sz)`: locks the buffer, using the `pipe_bbuf` structure, compute the `rd_sz = min(capacity_ - ((unsigned char*) addr - data_), sz)`, and do a `memcpy` of `rd_sz` bytes from `memfile::data_` to `addr`. Sleeps if buffer is empty.

3. Syscall functionalities and add-ins to current functions

`syscall_open(filename, mode)` checks that there is an available entry in the process fd table. Parses the `mode` to extract `O_CREAT` and `O_TRUNC` as `bool`s, then uses the `memfs::initfs_lookup(filename, create)` to generate the proper fd. Uses the return index into the `initfs` array and extracts a `memfile*` pointer. Checks that the `fdtable[]` does not already have a vnode of the same mode and `memfile*` pointer; return error if so. Declares a `mf_vnode` pointer and allocates memory via the slab allocator, passing in the `memfile*` pointer. Adds the vnode pointer into the next available slot in the `fdtable[]` and returns the index.

`syscall_read(fd, addr, sz)` asserts (memory range permissions/overflow, and) that the input fd points to a valid read-`vnode` in the fd table, and then calls one of two VFS `read()` functions based on the input fd. Calls `fdtable[fd]->read(addr, sz)`.

`syscall_write()` asserts (memory range permissions/overflow, and) that the input fd points to a valid write-`vnode` in the fd table, and then calls one of two VFS `write()` functions based on the input fd. Calls `fdtable[fd]->write()` after casting it appropriately.

`syscall_close(fd)` asserts that the fd indexes to a non-null, open file in the process `fdtable[]`. Supposing checks pass, decrement the refcount accordingly, and if the refcount hits zero afterwards, call `kfree()` on the `vnode` pointer. Set `fdtable[fd] = nullptr`.

`syscall_dup2(oldfd, newfd)` checks that `newfd` and `oldfd` are acceptable numbers (i.e. not out of array index range) and that `oldfd` indexes to a non-null value; if either is the case return `E_BADF`. Check that `newfd` is null; if it is not, then call `syscall_close(newfd)` before proceeding. Sets `proc::fdtable[newfd] = proc::fdtable[oldfd]`. Returns `newfd` upon success.

`syscall_fork()` updates to copy the per-process file descriptor table from parent to child. This is a set of pointers, so it will be a deep copy of the pointers.

Note that no additional per-`struct proc` locking is necessary here, even in the multithreaded case, since every (kernel-visible) thread has its own `struct proc` and the current thread is in the middle of the present syscall.

4. Synchronization and locking
At the moment, accesses to `proc::fdtable[]` are not locked (see Future Work). Accesses to a `memfile` are locked at the `memfile` level, not the `memfile_vnode` level, since if the latter case were used it would be difficult to prevent multiple processes, each with their own `vnode` pointing to the same underlying `memfile*`, from racing a read/write. The `keyboardstate` structure has its own lock as well. The `pipe_vnode` locks via the bounded buffer lock, so that reads and writes to a pipe are serialized. Note that under this paradigm a `vnode` never holds any locks, since if it did it would not actually prevent any processes from racing as each process can own a `vnode` that all point to the same file structure.

5. Future work
Add a per-`struct proc` lock to lock the `proc::fdtable[]` to support multithreaded processes sharing a `fdtable[]`. Any access to the `fdtable[]` will be under this lock. As any access to a `vnode` must occur through the `fdtable[]` of a `struct proc`, it suffices to lock only the table and not the `vnode`, as only different threads of the same process have access to the same `vnode`. Since a per-process lock will eventually be put in use, it may be of interest, time permitting, to make some other locking strategies finer-grained to the per-process structure.

6. Concerns

Is a "read" just reading off the beginning of the data array plus the `vnode::offset_`? Or is there something else to it? Similarly, is a "write" to the end of the file, or to the beginning plus `vnode::offset_`? This does not apply for console, right, as every read/write is to the end and separated?

How should `vnode::offset` work for a `vnode` that can do reads and writes? Should there be two different offsets, one for read and one for write, or should there just be one offset?

If pipe is partially available, should we block immediately or write/read what we can and then block?

If multiple threads are sharing a `proc::fdtable[]` does the refcount increase by 1 for each thread or just 1 for all threads?

Should we cast the vnode type dynamically by checking the `fd`, or is there a more elegant solution?

How is the constructor and destructor called in `kalloc` and `kfree`? This is necessary for the `struct proc` and `pipe_bbuf` allocations.