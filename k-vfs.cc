#include "k-vfs.hh"

vnode::vnode(int mode) {
    assert(mode == OF_READ || mode == OF_WRITE || mode == OF_RDWR);
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
}

// TODO UPDATE THE OFFSETS OF THE VNODE! (ALL RD/WR FUNCTIONS!)
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
    size_t wr_sz = io_sz(mf_->len_, mf_->capacity_, sz);
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + mf_->len_);
    memcpy(mf_ptr, reinterpret_cast<void*>(addr), wr_sz);
    assert(mf_->len_ + wr_sz >= mf_->len_); // Detect overflow
    mf_->len_ += wr_sz;
    // TODO Offset update
}

uintptr_t memfile_vnode::read(uintptr_t addr, size_t sz) {
    if (!readable()) {
        return E_BADF;
    }
    spinlock_guard guard(mf_->lock_);
    size_t rd_sz = io_sz(offset_, mf_->len_, sz);
    void* mf_ptr = reinterpret_cast<void*>(mf_->data_ + offset_);
    memcpy(reinterpret_cast<void*>(addr), mf_ptr, rd_sz);
    // TODO check Offset update
    offset_ += rd_sz;
}
