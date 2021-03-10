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
    char bbuf_[BBUF_CAP];
    int pos_ = 0;                               // Offset of next char to read
    int len_ = 0;                               // Num unread chars in buf
    bool write_closed_ = false;                 // Whether or not the buffer is writeable
    bool read_closed_ = false;                  // Whether or not the buffer is readable
    spinlock lock_;
    wait_queue rdq_, wrq_;                      // Read and write wait queues

    // The below assumes locked.
    bool pipe_empty();
    bool pipe_full();
    void close_write();
    void close_read();

    // The below does not assume locked.
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct pipe_vnode:public vnode {
    pipe_bbuf* bbuf_ = nullptr;                 // Shared bounded buffer
    pipe_vnode* partner_ = nullptr;             // Partner vnode (e.g. read's partner is write node)
    pipe_vnode(int mode, pipe_bbuf* bbuf);      // Write end allocates bbuf
    ~pipe_vnode();                              // Write end frees bbuf

    pipe_bbuf* get_bbuf();
    int set_partner(pipe_vnode* p);
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};



#endif
