#include "k-vfs.hh"

// Helper functions.
template <typename T>
static T get_min(T a, T b) {
    return a < b ? a : b;
}

static size_t io_sz(size_t start, size_t cap, size_t sz) {
    assert(cap >= start);
    if (cap - start > sz) {
        return sz;
    } else {
        return cap - start;
    }
}

// Direct functions.
vnode::vnode(int mode) : mode_(mode) {
    if (VFS_KBC_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
        log_printf("[vnode] Generic vnode constructor called\n");
    }
    assert((mode >= OF_READ) && (mode <= (OF_RDWR)));
}

// * By C++ decree, a struct that is delete'd with the parent class pointer type
// * must have virtual destructor. A virtual structor must be defined. If a 
// * single virtual function in a struct is defined, the struct is no longer pure virtual 
// * and as such every virtual function must be defined. Hence the absurd 3 lines below.
vnode::~vnode() {}
uintptr_t vnode::write(uintptr_t addr, size_t sz) { return 0; }
uintptr_t vnode::read(uintptr_t addr, size_t sz) { return 0; }

bool vnode::readable() {
    return mode_ & OF_READ;
}

bool vnode::writeable() {
    return mode_ & OF_WRITE;
}

int vnode::close() {
    if (VFS_PARANOIA >= 2) {
        log_printf("[vnode-close] Generic close called, decrementing refcount to %d\n", 
            refcount_-1);
    }
    assert(refcount_ > 0);
    return --refcount_;
}

kb_c_vnode::kb_c_vnode() : vnode(OF_RDWR) {
    assert(offset_ == 0);
    assert(refcount_ == 0);
    if (VFS_KBC_PARANOIA >= 1) {
        log_printf("[kb_c_vnode] Stdio vnode constructor called\n");
    }
}

kb_c_vnode::~kb_c_vnode() {
    spinlock_guard guard(open_close_lock_);
    if (VFS_KBC_PARANOIA >= 1) {
        log_printf("[kb_c_vnode-destructor] *Closing time...one last call for alcohol*\n");
    }
    assert(refcount_ == 0);
}

int kb_c_vnode::close() {
    spinlock_guard guard(open_close_lock_); // May be contended for
    if (VFS_KBC_PARANOIA >= 1) {
        log_printf("[kb_c_vnode-close] KBC close called\n");
    }
    assert(refcount_ > 0);
    return --refcount_;
}

uintptr_t kb_c_vnode::write(uintptr_t addr, size_t sz) {
    auto& csl = consolestate::get();
    spinlock_guard guard(csl.lock_);
    size_t n = 0;
    while (n < sz) {
        int ch = *reinterpret_cast<const char*>(addr);
        ++addr;
        ++n;
        console_printf(0x0F00, "%c", ch);
    }
    return n;
}

uintptr_t kb_c_vnode::read(uintptr_t addr, size_t sz) {
    auto& kbd = keyboardstate::get();
    auto irqs = kbd.lock_.lock();

    // mark that we are now reading from the keyboard
    // (so `q` should not power off)
    if (kbd.state_ == kbd.boot) {
        kbd.state_ = kbd.input;
    }

    // yield until a line is available
    // (special case: do not block if the user wants to read 0 bytes)
    if (sz) {
        waiter().block_until(kbd.wq_, [&] () {
            return (kbd.eol_);
        }, kbd.lock_, irqs);
    }

    // read that line or lines
    size_t n = 0;
    while (kbd.eol_ != 0 && n < sz) {
        if (kbd.buf_[kbd.pos_] == 0x04) {
            // Ctrl-D means EOF
            if (n == 0) {
                kbd.consume(1);
            }
            break;
        } else {
            *reinterpret_cast<char*>(addr) = kbd.buf_[kbd.pos_];
            ++addr;
            ++n;
            kbd.consume(1);
        }
    }

    kbd.lock_.unlock(irqs);
    return n;
}

// mf can be a nullptr, but then set_mf(mf) must be called before any I/O.
// syscall_open() uses this feature to prevent memory leaks.
memfile_vnode::memfile_vnode(int mode, memfile* mf) : vnode(mode) {
    if (VFS_MF_PARANOIA >= 1) {
        log_printf("[memfile-vnode-constructor] memfile vnode constructor called with mode %d\n",
            mode);
    }
    mf_ = mf;
    assert(offset_ == 0);
    assert(refcount_ == 0);
}

memfile_vnode::~memfile_vnode() {
    spinlock_guard guard(open_close_lock_);
    if (VFS_MF_PARANOIA >= 1) {
        log_printf("[memfile_vnode-destructor] *Closing time...one last call for alcohol*\n");
    }
    assert(refcount_ == 0);
}

void memfile_vnode::set_mf(memfile* mf) {
    assert(mf);
    assert(!mf_); // Can only set this once
    mf_ = mf;
}

int memfile_vnode::close() {
    spinlock_guard guard(open_close_lock_);
    if (VFS_MF_PARANOIA >= 2) {
        log_printf("[memfile_vnode-close] memfile close called\n");
    }
    if (!refcount_) {
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[memfile_vnode-close] Failed to close: file not open\n");
        }
        return E_BADF;
    }
    return --refcount_;
}

uintptr_t memfile_vnode::write(uintptr_t addr, size_t sz) {
    if (!writeable()) {
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[memfile_vnode-write] Rejected attempted write on read-only node\n");
        }
        return E_BADF;
    }
    spinlock_guard guard(mf_->lock_);

    // Compute the best size that can be written; attempt once to 
    // increase the file size if the best size is not enough.
    size_t best_sz = ::io_sz(offset_, mf_->len_, sz);
    if (best_sz < sz) {
        if (mf_->set_length(offset_ + sz) == E_NOSPC) {
            if (VFS_MF_PARANOIA >= 1) {
                log_printf("[memfile_vnode-write] No space remaining in file\n");
            }
            return E_NOSPC;
        }
    }
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + offset_);
    memcpy(mf_ptr, reinterpret_cast<void*>(addr), sz);
    
    // Update the offset.
    offset_ += sz; // No open close lock--the memfile lock is stronger
    return sz;
}

uintptr_t memfile_vnode::read(uintptr_t addr, size_t sz) {
    if (!readable()) {
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[memfile_vnode-write] Rejected attempted read on write-only node\n");
        }
        return E_BADF;
    }
    spinlock_guard guard(mf_->lock_);
    size_t rd_sz = ::io_sz(offset_, mf_->len_, sz);
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + offset_);
    memcpy(reinterpret_cast<void*>(addr), mf_ptr, rd_sz);

    // Update offset of node.
    offset_ += rd_sz;
    return rd_sz;
}

// Assumes lock held.
bool pipe_bbuf::pipe_empty() {
    assert(len_ >= 0);
    return len_ == 0;
}

// Assumes lock held.
bool pipe_bbuf::pipe_full() {
    assert(len_ <= BBUF_CAP);
    return (len_ == BBUF_CAP);
}

// Assumes lock held.
void pipe_bbuf::close_write() {
    assert(!write_closed_);
    write_closed_ = true;
}

// Assumes lock held.
void pipe_bbuf::close_read() {
    assert(!read_closed_);
    read_closed_ = true;
}

uintptr_t pipe_bbuf::write(uintptr_t addr, size_t sz) {
    spinlock_guard guard(lock_);
    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe_bbuf-write] Pipe bbuf now handling a WRITE of %lu size\n", sz);
    } 
    if (PIPE_PARANOIA >= 3) {
        log_printf("[pipe_bbuf-write] Buf dump (to be written): '%s'\n", reinterpret_cast<char*>(addr));
    }
    assert(!write_closed_);

    // Block if pipe full.
    if (pipe_full() && sz) {
        if (PIPE_PARANOIA >= 2) {
            log_printf("[pipe_bbuf-write] Pipe bbuf full, blocking\n");
        }
        waiter().block_until(wrq_, [&] () {
            return (!pipe_full() || read_closed_);
        }, guard);
    }

    // Writing to a pipe with read end closed returns error.
    if (read_closed_) {
        if (PIPE_PARANOIA >= 2) {
            log_printf("[pipe_bbuf-write] Read end closed, returning error\n");
        }
        return E_PIPE;
    }

    int pos = 0;
    while (pos < (int) sz && len_ < BBUF_CAP) {
        int index = (pos_ + len_) % BBUF_CAP;
        int available_space = get_min(BBUF_CAP - index, BBUF_CAP - len_);
        size_t wr_sz = get_min((int) sz - pos, available_space);
        memcpy(&bbuf_[index], reinterpret_cast<void*>(addr+pos), wr_sz);
        len_ += wr_sz;
        pos += wr_sz;
    }

    // Wake up processes sleeping on a read.
    rdq_.wake_all();

    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe_bbuf-write] Successfully wrote %d chars, new len_=%d\n", pos, len_);
    }

    return pos;
}

uintptr_t pipe_bbuf::read(uintptr_t addr, size_t sz) {
    spinlock_guard guard(lock_);
    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe_bbuf-read] Pipe bbuf now handling a READ of %lu size\n", sz);
    }
    assert(!read_closed_);

    // Block if pipe empty.
    if (pipe_empty() && !write_closed_ && sz) {
        if (PIPE_PARANOIA >= 2) {
            log_printf("[pipe_bbuf-read] Pipe bbuf empty, blocking\n");
        }
        waiter().block_until(rdq_, [&] () {
            return (!pipe_empty() || write_closed_);
        }, guard);
    }

    // A drained pipe with the write end closed should return EOF.
    if (write_closed_ && pipe_empty()) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[pipe_bbuf-read] EOF reached, returning\n");
        }
        return EOF;
    }

    int pos = 0;
    while (pos < (int) sz && len_ > 0) {
        size_t available_space = get_min(len_, BBUF_CAP - pos_);
        size_t n = get_min(sz - pos, available_space);
        memcpy(reinterpret_cast<void*>(addr+pos), &bbuf_[pos_], n);
        pos_ = (pos_ + n) % BBUF_CAP;
        len_ -= n;
        pos += n;
    }

    // Wake up processes sleeping on a write.
    wrq_.wake_all();

    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe_bbuf-read] Successfully read %d chars\n", pos);
    }
    return pos;
}

pipe_vnode::pipe_vnode(int mode, pipe_bbuf* bbuf)
    : vnode(mode) {
    bool mode_valid = (mode == OF_READ || mode == OF_WRITE);
    assert(mode_valid);
    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe] Pipe constructor called with mode %s\n", 
            mode == OF_READ ? "READ" : "WRITE");
    }
    if (!bbuf) { // Only one (r XOR w) node should allocate, the other should pass in ptr
        bbuf_ = knew<pipe_bbuf>();
        if (!bbuf_) {
            if (PIPE_PARANOIA >= 1) {
                log_printf("[pipe-bbuf] Failed to allocate memory for a bbuf, returning to syscall\n");
            }
        }
    } else {
        bbuf_ = bbuf; // read end should use same buf as write end
    }
}

pipe_vnode::~pipe_vnode() {    
    spinlock_guard guard(bbuf_->lock_);
    if (PIPE_PARANOIA >= 2) {
        log_printf("[pipe] pipe %s end destructor called\n",
            mode_ == OF_READ ? "READ" : "WRITE");
    }

    // If one or more initial allocations failed the destructor does nothing.
    if (!bbuf_) return;

    // Tell the bounded buffer to close off the relevant end.
    if (mode_ == OF_READ) { // Read node
        bbuf_->close_read();
        bbuf_->wrq_.wake_all();
    } else {
        bbuf_->close_write();
        bbuf_->rdq_.wake_all();
    }

    // If both ends are closed, free the buffer. Note that
    // the desctructor is under bbuf lock, so there are no
    // races for double frees. The last node to transcend frees bbuf.
    if (bbuf_->write_closed_ && bbuf_->read_closed_) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[pipe] Both ends of pipe closed, deleting buffer\n");
        }
        delete bbuf_;
    }
    return;
}

pipe_bbuf* pipe_vnode::get_bbuf() {
    return bbuf_;
}

int pipe_vnode::close() {
    spinlock_guard guard(open_close_lock_);
    if (PIPE_PARANOIA >= 1) {
        log_printf("[pipe_vnode-close] pipe close called\n");
    }
    assert(refcount_ > 0);
    return --refcount_;
}

uintptr_t pipe_vnode::write(uintptr_t addr, size_t sz) {
    if (!writeable()) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[pipe] Rejected attempted write to read end of pipe\n");
        }
        return E_BADF;
    }
    if (PIPE_PARANOIA >= 3) {
        log_printf("[pipe] Valid pipe write called, transferring to bbuf\n");
    }
    return bbuf_->write(addr, sz);
}

uintptr_t pipe_vnode::read(uintptr_t addr, size_t sz) {
    if (!readable()) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[pipe] Rejected attempted read to write end of pipe\n");
        }
        return E_BADF;
    }
    if (PIPE_PARANOIA >= 3) {
        log_printf("[pipe] Valid pipe read called, transferring to bbuf\n");
    }
    return bbuf_->read(addr, sz);
}

// ===== Unix Domain Sockets ===== //

uds::uds(const char* key) {
    key_ = (char*) key;
}

uds::~uds() {
}

int uds::bind(proc* server) {
    spinlock_guard guard(client_server_lock_);
    if (server_) {
        return E_MFILE; // A server is already using this socket
    } else {
        server_ = server;
        return 0;
    }
}

int uds::listen() {
    spinlock_guard guard(client_server_lock_);
    if (!server_) {
        return E_BADCONN;
    }
    listening = true;
}

int uds::accept() {
    spinlock_guard guard(client_server_lock_);
    if (!listening) {
        return E_BADCONN;
    }
    accepting = true;
    clq_.wake_all(); // Wake up client sleeping on a connect()
    long start_time = ticks;
    long end_time;
    if (!connected()) {
        waiter().block_until(servq_, [&] () {
            end_time = ticks;
            return (connected() || end_time >= start_time + UDS_TIMEOUT);
        }, guard);
    }
    if (end_time >= start_time + UDS_TIMEOUT) {
        return E_SOCKTIMEOUT;
    } else {
        return 0;
    }
}

int uds::connect(proc* client) {
    spinlock_guard guard(client_server_lock_);
    if (!listening) { // Can't connect till server is listening.
        return E_BADCONN;
    }
    client_ = client;
    assert(connected());
    servq_.wake_all(); // Wake up server sleeping on an accept()
    long start_time = ticks;
    long end_time;
    if (!accepting) {
        waiter().block_until(clq_, [&] () {
            end_time = ticks;
            return (accepting || end_time >= start_time + UDS_TIMEOUT);
        }, guard);
    }
    if (end_time >= start_time + UDS_TIMEOUT) {
        return E_SOCKTIMEOUT;
    }
}
// TODO implement timeout feature in proc::exception()

int uds::close() {
    // Reset everything under lock
    spinlock_guard guard(client_server_lock_);
    key_ = nullptr;
    fd_ = -1;
    client_ = server_ = nullptr;
    return 0;
}

int uds::write(int fd) {
    spinlock_guard guard()
}
