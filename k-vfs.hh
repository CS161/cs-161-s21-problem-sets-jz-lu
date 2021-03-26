#ifndef CHICKADEE_K_VFS_HH
#define CHICKADEE_K_VFS_HH
#include "k-devices.hh"
#include "k-wait.hh"
#include "chickadeefs.hh"

#define BBUF_CAP    256                        // Pipe bounded buffer capacity
#define EOF         0
#define UDS_TIMEOUT 100                        // UDS server wait timeout in msecs

struct kb_c_vnode:public vnode {
    // Note: we will not use inherited offset_ in this struct, 
    // the kbd has its own offset variable called pos_.
    kb_c_vnode();
    ~kb_c_vnode();

    int close();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct memfile_vnode:public vnode {
    memfile* mf_;                               // in-memory file, lock defined in here

    memfile_vnode(int mode, memfile* mf);
    ~memfile_vnode();

    int close();
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
    pipe_bbuf* bbuf_ = nullptr;                 // Shared bounded buffer
    pipe_vnode(int mode, pipe_bbuf* bbuf);      // Write end allocates bbuf
    ~pipe_vnode();                              // Write end frees bbuf

    pipe_bbuf* get_bbuf();

    // The below lock when called.
    int close();
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
};

struct disk_vnode:public vnode {
    chkfs::inode* ino_;
    disk_vnode(chkfs::inode* ino, int mode);

    int close();
    static chkfs::inode* create_file(const char* filename);
    uintptr_t write(uintptr_t addr, size_t sz);
    uintptr_t read(uintptr_t addr, size_t sz);
    off_t lseek(off_t off, int origin);
    bool seek_invalid(off_t off, int origin);
};

// Simplified Unix domain socket. A single client and a single server process
// can send a file descriptor over. Not a part of the vnode family.
struct uds {
    char key_[MAX_FILENAME_LEN] = "\0"; // Index of descriptor
    int fd_ = -1;
    bool listening_ = false, accepting_ = false;
    bool received_ = true;
    proc *server_ = nullptr, *client_ = nullptr;
    spinlock client_server_lock_;       // Any information relevant to client AND server locks
    wait_queue servq_, clq_;            // Server and client blocking queues

    uds(const char* key);
    ~uds();

    // The below require locks.
    int bind(proc* server);             // Binds a server process with the socket
    int listen();                       // Marks socket as open for listening
    int accept();                       // Sleep-waits for a client connection
    int connect(proc* client);          // Sleep-waits for server accept
    inline bool connected() {           // * Assumes locked
        return (client_ != nullptr);
    }
    int close(proc* closer);
    int write(proc* p, int fd);                  // Client updates the socket fd
    int read(proc* p);                   // Server grabs socket fd
    void wake_all();
};

#endif
