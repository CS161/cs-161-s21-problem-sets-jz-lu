#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-devices.hh"
#include "k-wait.hh"

#define BBUF_CAP    256 // Pipe bounded buffr capacity
#define EOF         0

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    spinlock open_close_lock_;
    kb_c_vnode();
    ~kb_c_vnode();

    int close();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile* mf_;                               // in-memory file, lock defined in here
    spinlock open_close_lock_;

    memfile_vnode(int mode, memfile* mf);
    ~memfile_vnode();

    void set_mf(memfile* mf);
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

// All locks used by pipe vnodes are done via the bbuf lock shared between
// read and write pipe vnodes via the shared bbuf.
struct pipe_vnode:public vnode {
    spinlock open_close_lock_;
    pipe_bbuf* bbuf_ = nullptr;                 // Shared bounded buffer
    pipe_vnode(int mode, pipe_bbuf* bbuf);      // Write end allocates bbuf
    ~pipe_vnode();                              // Write end frees bbuf

    pipe_bbuf* get_bbuf();

    // The below lock when called.
    int close();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};



#endif
