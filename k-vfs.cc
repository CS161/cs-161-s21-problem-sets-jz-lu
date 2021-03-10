#include "k-vfs.hh"

template <typename T>
static T get_min(T a, T b) {
    return a < b ? a : b;
}

vnode::vnode(int mode) {
    // assert((mode >= OF_READ) && (mode <= OF_RDWR));
    mode_ = mode;
}

vnode::~vnode() {
    assert(refcount_ == 0);
}

bool vnode::readable() {
    return mode_ & OF_READ;
}

bool vnode::writeable() {
    return mode_ & OF_WRITE;
}

void vnode::close() {
    assert(refcount_ > 0);
    if(--refcount_ == 0) {
        if (VFS_KBC_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[Vfs-vnode] Freeing vnode at %p... *Closing time...one last call for alcohol*\n",
                this);
        }
        kfree(reinterpret_cast<void*>(this));
    }
}

size_t io_sz(size_t start, size_t cap, size_t sz) {
    assert(cap >= start);
    if (cap - start > sz) {
        return sz;
    } else {
        return cap - start;
    }
}

kb_c_vnode::kb_c_vnode() : vnode(OF_RDWR) {
    assert(offset_ == 0);
    assert(refcount_ == 0);
    if (VFS_KBC_PARANOIA >= 1) {
        log_printf("[kb_c_vnode] Constructor of Stdio vnode called\n");
    }
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

memfile_vnode::memfile_vnode(int mode, memfile* mf) : vnode(mode) {
    mf_ = mf;
    assert(mf);
    assert(offset_ == 0);
    assert(refcount_ == 0);
    assert(!mf_->empty());
}

uintptr_t memfile_vnode::write(uintptr_t addr, size_t sz) {
    if (!writeable()) {
        return E_BADF;
    }
    spinlock_guard guard(mf_->lock_);

    // Compute the best size that can be written; attempt once to 
    // increase the file size if the best size is not enough.
    size_t best_sz = io_sz(offset_, mf_->capacity_, sz);
    if (best_sz < sz) {
        if (mf_->set_length(offset_ + sz) == E_NOSPC) {
            return E_NOSPC;
        }
    }
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + offset_);
    memcpy(mf_ptr, reinterpret_cast<void*>(addr), sz);
    
    // Update the offset, and if needed, the length of the memfile
    offset_ += sz;
    return sz;
}

uintptr_t memfile_vnode::read(uintptr_t addr, size_t sz) {
    if (!readable()) {
        return E_BADF;
    }
    spinlock_guard guard(mf_->lock_);
    size_t rd_sz = io_sz(offset_, mf_->len_, sz);
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + offset_);
    memcpy(reinterpret_cast<void*>(addr), mf_ptr, rd_sz);

    // Update offset of node.
    offset_ += rd_sz;
    return rd_sz;
}

// Assumes lock held.
bool pipe_bbuf::pipe_empty() {
    assert(len_ >= 0);
    return len_;
}

// Assumes lock held.
bool pipe_bbuf::pipe_full() {
    assert(len_ <= BBUF_CAP);
    return (len_ == BBUF_CAP);
}

// Assumes lock held.
void pipe_bbuf::close_write() {
    write_closed_ = true;
}

// Assumes lock held.
void pipe_bbuf::close_read() {
    read_closed_ = true;
}

uintptr_t pipe_bbuf::write(uintptr_t addr, size_t sz) {
    spinlock_guard guard(lock_);

    // Block if pipe full.
    if (pipe_full()) {
        waiter().block_until(wrq_, [&] () {
            return !pipe_full();
        }, guard);
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

    return pos;
}

uintptr_t pipe_bbuf::read(uintptr_t addr, size_t sz) {
    spinlock_guard guard(lock_);

    // Block if pipe empty.
    if (pipe_empty()) {
        waiter().block_until(wrq_, [&] () {
            return !pipe_empty();
        }, guard);
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

    return pos;
}

pipe_vnode::pipe_vnode(int mode, pipe_bbuf* bbuf)
    : vnode(mode) {
    bool mode_valid = (mode == OF_READ || mode == OF_WRITE);
    assert(mode_valid);
    if (!bbuf) {
        bbuf_ = knew<pipe_bbuf>();
    } else {
        bbuf_ = bbuf; // read end should use same buf as write end
    }
}

pipe_vnode::~pipe_vnode() {
    // Free the bounded buffer, if the node is a write.
    spinlock_guard guard(bbuf_->lock_);
    if (mode_ == OF_READ) { // Read node
        delete bbuf_;
    } else {
        bbuf_->close_write();
    }
    
    bbuf_ = nullptr;
}

pipe_bbuf* pipe_vnode::get_bbuf() {
    return bbuf_;
}

int pipe_vnode::set_partner(pipe_vnode* p) {
    if (p) {
        partner_ = p;
        return 0;
    }
    return -1;
}

uintptr_t pipe_vnode::write(uintptr_t addr, size_t sz) {
    if (!writeable()) {
        return E_BADF;
    }
    return bbuf_->write(addr, sz);
}

uintptr_t pipe_vnode::read(uintptr_t addr, size_t sz) {
    if (!readable()) {
        return E_BADF;
    }
    return bbuf_->read(addr, sz);
}


