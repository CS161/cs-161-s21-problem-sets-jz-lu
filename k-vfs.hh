#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-devices.hh"

struct vnode {
    int mode_ = 0; // Read, write, or both
    off_t offset_ = 0; // Offset from file, to be incremented on read/writes
    int refcount_ = 0; // Number of processes with entry in fd table pointing here
    // Locks declared in inheriters.

    vnode(int mode);
    ~vnode();
    bool readable();
    bool writeable();

    // Computes the actual I/O size as min of available size and desired size.
    size_t io_sz(size_t start, size_t cap, size_t sz);

    void close();

    // To be defined in derived structs.
    // * NOTE: validation is assumed to be done at the syscall level
    // * so the VFS I/O functions assume valid input.

    // Write sz bytes to the file. All-or-none paradigm: either write everything
    // or fail.
    virtual uintptr_t write(uintptr_t addr, size_t sz);

    // Read at most sz bytes from the file. Best case paradigm: read as much
    // as possible and return what is read. 
    virtual uintptr_t read(uintptr_t addr, size_t sz);
};

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    kb_c_vnode();

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile::memfile* mf_; // in-memory file, lock defined in here

    memfile_vnode(int mode, memfile::memfile* mf);

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};



#endif