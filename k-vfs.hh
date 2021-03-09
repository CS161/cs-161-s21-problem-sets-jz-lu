#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-devices.hh"
#include "k-wait.hh"

#define BBUF_CAP    256 // Pipe bounded buffr capacity

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    kb_c_vnode();

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile* mf_;                               // in-memory file, lock defined in here

    memfile_vnode(int mode, memfile* mf);

    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

// Bounded buffer data structure for pipe.
struct pipe_bbuf {
    char bbuf[BBUF_CAP];
    int len_ = 0;                               // Num meaningful chars in buffer
    spinlock lock_;
    wait_queue rdq_, wrq_;                      // Read and write wait queues

    bool pipe_empty();
    bool pipe_full();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct pipe_vnode:public vnode {
    pipe_bbuf* bbuf_ = nullptr;                 // Shared bounded buffer
    pipe_vnode(int mode, pipe_bbuf* bbuf);      // Write end allocates bbuf
    ~pipe_vnode();                              // Write end frees bbuf

    pipe_bbuf* get_bbuf();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};



#endif