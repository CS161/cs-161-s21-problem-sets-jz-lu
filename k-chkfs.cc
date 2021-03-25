#include "k-chkfs.hh"
#include "k-ahci.hh"
#include "k-chkfsiter.hh"

bufcache bufcache::bc;

bufcache::bufcache() {
}

// bufcache::evict()
//    Evict a block from the bufcache, if possible, and
//    return the index into the bufcache entry array of the free block.
//    Assumes the bufcache is locked, but the entry need not be (no changes made to it).
size_t bufcache::evict() {
    bcentry* blk = evictq_.pop_front();
    if (!blk) { // Nothing to pop
        return -1;
    } else { // Compute the index into the bufcache and return it
        size_t blk_index = blk->index();
        assert(blk_index < ne);
        blk->lock_.lock_noirq();
        blk->clear();
        blk->lock_.unlock_noirq();
        return blk_index;
    }
}


// bufcache::full()
//    Returns true if all entries taken.
bool bufcache::full() {
    spinlock_guard guard(lock_);
    for (size_t i = 0; i != ne; ++i) {
        if (e_[i].empty()) {
            return false;
        }
    }
    return true;
}


// bufcache::has_uref_dirty_blocks
//    Examines the dirty list and checks if there are any unreferenced blocks
//    so we can call sync on them.
//    Assumes the bufcache is locked.
bool bufcache::has_uref_dirty_blocks() {
    bool has = false;
    size_t count = 0;
    for (auto it = dirty_list_.front(); it && count < 2*ne; // Emergency stopping condition
            it = dirty_list_.next(it), ++count) {
        auto irqs = it->lock_.lock();
        if (it->ref_ == 0) {
            has = true;
            it->lock_.unlock(irqs);
            break;
        }
        it->lock_.unlock(irqs);
    }
    return has;
}


// bufcache::get_disk_entry(bn, cleaner)
//    Reads disk block `bn` into the buffer cache, obtains a reference to it,
//    and returns a pointer to its bcentry. The returned bcentry has
//    `buf_ != nullptr` and `estate_ >= es_clean`. The function may block.
//
//    If this function reads the disk block from disk, and `cleaner != nullptr`,
//    then `cleaner` is called on the entry to clean the block data.
//
//    Returns `nullptr` if there's no room for the block.

bcentry* bufcache::get_disk_entry(chkfs::blocknum_t bn,
                                  bcentry_clean_function cleaner) {
    assert(chkfs::blocksize == PAGESIZE);
    auto irqs = lock_.lock();

    // look for slot containing `bn`
    size_t i, empty_slot = -1;
    for (i = 0; i != ne; ++i) {
        if (e_[i].empty()) {
            if (empty_slot == size_t(-1)) {
                empty_slot = i;
            }
        } else if (e_[i].bn_ == bn) {
            break;
        }
    }

    // if not found, use free slot
    if (i == ne) {
        if (empty_slot == size_t(-1)) {
            // Cache is full--attempt to evict something.
            empty_slot = evict();
            if (empty_slot == size_t(-1)) {
                log_printf("[bufcache] no room for block %u, attempting to free up buffer\n", bn);
                if (has_uref_dirty_blocks()) {
                    log_printf("[bufcache] Buffer has spare room, freeing and trying again\n");
                    lock_.unlock(irqs);
                    sync(0);
                    return get_disk_entry(bn, cleaner);
                } else {
                    log_printf("[bufcache] Unable to free up buffer\n");
                    lock_.unlock(irqs);
                    return nullptr;
                }
                
            }
        }
        i = empty_slot;
    }

    // obtain entry lock
    e_[i].lock_.lock_noirq();

    // mark allocated if empty
    if (e_[i].empty()) {
        e_[i].estate_ = bcentry::es_allocated;
        e_[i].bn_ = bn;
    }

    // mark reference
    ++e_[i].ref_;
    if (e_[i].qlink_.is_linked()) {
        evictq_.erase(&e_[i]);
    }
    // no longer need cache lock
    lock_.unlock_noirq();

    // load block
    bool ok = e_[i].load(irqs, cleaner);


    // unlock and return entry
    if (!ok) {
        --e_[i].ref_;
        e_[i].clear();
        log_printf("Load failed in get_disk_entry\n");
        assert(false);
    }

    e_[i].lock_.unlock(irqs);

    
    return ok ? &e_[i] : nullptr;
}


// bcentry::load(irqs, cleaner)
//    Completes the loading process for a block. Requires that `lock_` is
//    locked, that `estate_ >= es_allocated`, and that `bn_` is set to the
//    desired block number.

bool bcentry::load(irqstate& irqs, bcentry_clean_function cleaner) {
    bufcache& bc = bufcache::get();

    // load block, or wait for concurrent reader to load it
    while (true) {
        assert(estate_ != es_empty);
        if (estate_ == es_allocated) {
            if (!buf_) {
                buf_ = reinterpret_cast<unsigned char*>
                    (kalloc(chkfs::blocksize));
                if (!buf_) {
                    return false;
                }
            }
            estate_ = es_loading;
            lock_.unlock(irqs);

            sata_disk->read(buf_, chkfs::blocksize,
                            bn_ * chkfs::blocksize);

            irqs = lock_.lock();
            estate_ = es_clean;
            if (cleaner) {
                cleaner(this);
            }
            bc.read_wq_.wake_all();
        } else if (estate_ == es_loading) {
            waiter().block_until(bc.read_wq_, [&] () {
                    return estate_ != es_loading;
                }, lock_, irqs);
        } else {
            return true;
        }
    }
}


// bcentry::put()
//    Releases a reference to this buffer cache entry. The caller must
//    not use the entry after this call.

void bcentry::put() {
    bufcache& bc = bufcache::get();
    spinlock_guard bcguard(bc.lock_);
    spinlock_guard guard(lock_);
    assert(ref_ != 0);
    if (--ref_ == 0 && bn_ != 0 && estate_ != es_dirty) {
        bc.evictq_.push_back(this);
    }
}


// bcentry::get_write()
//    Obtains a write reference for this entry.
void bcentry::get_write() {
    bufcache& bc = bufcache::get();
    spinlock_guard bcguard(bc.lock_);
    spinlock_guard guard(lock_);
    assert(wref_ == 0 || wref_ == 1);
    if (wref_ == 1) {
        waiter().block_until(wq_, [&] () {
            return (wref_ == 0);
        }, guard);
    }
    if (!dlink_.is_linked()) {
        estate_ = bcentry::es_dirty;
        bc.dirty_list_.push_back(this);
        assert(bc.dirty_list_.front());
        assert(dlink_.is_linked());
        log_printf("Pushed bn=%d onto dirty list\n", bn_);
    }
    ++wref_;
}


// bcentry::put_write()
//    Releases a write reference for this entry.
void bcentry::put_write() {
    spinlock_guard guard(lock_);
    assert(wref_ == 1);
    --wref_;
    wq_.wake_all();
}

// Debugging helper function: visualize dirty list block numbers.
[[maybe_unused]] static void print_dirty_list() {
    bufcache& bc = bufcache::get();
    log_printf("Dirty --> [ ");
    for (auto it = bc.dirty_list_.front(); it; it = bc.dirty_list_.next(it)) {
        log_printf("%d ", it->bn_);
    }
    log_printf("]\n");
}

// bufcache::sync(drop)
//    Writes all dirty buffers to disk, blocking until complete.
//    If `drop > 0`, then additionally free all buffer cache contents,
//    except referenced blocks. If `drop > 1`, then assert that all inode
//    and data blocks are unreferenced.

int bufcache::sync(int drop) {
    // Swap the current list under bufcache lock so it doesn't 
    // sync forever (if another thread keeps adding dirty blocks).
    list<bcentry, &bcentry::dlink_> local_dirty;
    auto irqs = lock_.lock();
    local_dirty.swap(dirty_list_);
    lock_.unlock(irqs);

    // Write to the disk and mark the block as clean under bcentry lock.
    // NOTE: the bufcache lock need not be held here since local_dirty is, well, local to the function.
    while (bcentry* e = local_dirty.pop_front()) {
        e->get_write();
        log_printf("sata disk write starting\n");
        sata_disk->write(e->buf_, chkfs::blocksize, e->bn_ * chkfs::blocksize);
        log_printf("sata disk write done\n");
        {
        spinlock_guard eguard(e->lock_);
        e->estate_ = bcentry::es_clean;
        if (e->ref_ == 0 && e->bn_ != 0 && drop <= 0) {
            bc.evictq_.push_back(e);
        }
        }
        e->put_write();
    }

    // drop clean buffers if requested
    if (drop > 0) {
        spinlock_guard guard(lock_);
        for (size_t i = 0; i != ne; ++i) {
            spinlock_guard eguard(e_[i].lock_);

            // validity checks: referenced entries aren't empty; if drop > 1,
            // no data blocks are referenced
            assert(e_[i].ref_ == 0 || e_[i].estate_ != bcentry::es_empty);
            if (e_[i].ref_ > 0 && drop > 1 && e_[i].bn_ >= 2) {
                error_printf(CPOS(22, 0), COLOR_ERROR, "sync(2): block %u has nonzero reference count\n", e_[i].bn_);
                assert_fail(__FILE__, __LINE__, "e_[i].bn_ < 2");
            }

            // actually drop buffer
            if (e_[i].ref_ == 0) {
                e_[i].clear();
            }
        }
    }

    return 0;
}


// inode lock functions
//    The inode lock protects the inode's size and data references.
//    It is a read/write lock; multiple readers can hold the lock
//    simultaneously.
//
//    IMPORTANT INVARIANT: If a kernel task has an inode lock, it
//    must also hold a reference to the disk page containing that
//    inode.

namespace chkfs {

void inode::lock_read() {
    mlock_t v = mlock.load(std::memory_order_relaxed);
    while (true) {
        if (v >= mlock_t(-2)) {
            current()->yield();
            v = mlock.load(std::memory_order_relaxed);
        } else if (mlock.compare_exchange_weak(v, v + 1,
                                               std::memory_order_acquire)) {
            return;
        } else {
            // `compare_exchange_weak` already reloaded `v`
            pause();
        }
    }
}

void inode::unlock_read() {
    mlock_t v = mlock.load(std::memory_order_relaxed);
    assert(v != 0 && v != mlock_t(-1));
    while (!mlock.compare_exchange_weak(v, v - 1,
                                        std::memory_order_release)) {
        pause();
    }
}

void inode::lock_write() {
    assert(!has_write_lock());
    mlock_t v = 0;
    while (!mlock.compare_exchange_weak(v, mlock_t(-1),
                                        std::memory_order_acquire)) {
        current()->yield();
        v = 0;
    }
}

void inode::unlock_write() {
    assert(has_write_lock());
    mlock.store(0, std::memory_order_release);
}

bool inode::has_write_lock() const {
    return mlock.load(std::memory_order_relaxed) == mlock_t(-1);
}

}


// chickadeefs state

chkfsstate chkfsstate::fs;

chkfsstate::chkfsstate() {
}


// clean_inode_block(entry)
//    Called when loading an inode block into the buffer cache. It clears
//    values that are only used in memory.

static void clean_inode_block(bcentry* entry) {
    uint32_t entry_index = entry->index();
    auto is = reinterpret_cast<chkfs::inode*>(entry->buf_);
    for (unsigned i = 0; i != chkfs::inodesperblock; ++i) {
        // inode is initially unlocked
        is[i].mlock = 0;
        // containing entry's buffer cache position is `entry_index`
        is[i].mbcindex = entry_index;
    }
}


// chkfsstate::get_inode(inum)
//    Returns inode number `inum`, or `nullptr` if there's no such inode.
//    Obtains a reference on the buffer cache block containing the inode;
//    you should eventually release this reference by calling `ino->put()`.

chkfs::inode* chkfsstate::get_inode(inum_t inum) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();

    chkfs::inode* ino = nullptr;
    if (inum > 0 && inum < sb.ninodes) {
        auto bn = sb.inode_bn + inum / chkfs::inodesperblock;
        if (auto inode_entry = bc.get_disk_entry(bn, clean_inode_block)) {
            ino = reinterpret_cast<inode*>(inode_entry->buf_);
        }
    }
    if (ino != nullptr) {
        ino += inum % chkfs::inodesperblock;
    }
    return ino;
}


namespace chkfs {
// chkfs::inode::entry()
//    Returns a pointer to the buffer cache entry containing this inode.
//    Requires that this inode is a pointer into buffer cache data.
bcentry* inode::entry() {
    assert(mbcindex < bufcache::ne);
    auto entry = &bufcache::get().e_[mbcindex];
    assert(entry->contains(this));
    return entry;
}

// chkfs::inode::put()
//    Releases the caller’s reference to this inode, which must be located
//    in the buffer cache.
void inode::put() {
    entry()->put();
}
}


// chkfsstate::lookup_inode(dirino, filename)
//    Looks up `filename` in the directory inode `dirino`, returning the
//    corresponding inode (or nullptr if not found). The caller must have
//    a read lock on `dirino`. The returned inode has a reference that
//    the caller should eventually release with `ino->put()`.

chkfs::inode* chkfsstate::lookup_inode(inode* dirino,
                                       const char* filename) {
    chkfs_fileiter it(dirino);

    // read directory to find file inode
    chkfs::inum_t in = 0;
    for (size_t diroff = 0; !in; diroff += blocksize) {
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (dirent->inum && strcmp(dirent->name, filename) == 0) {
                    in = dirent->inum;
                    break;
                }
            }
            e->put();
        } else {
            return nullptr;
        }
    }
    return get_inode(in);
}


// chkfsstate::lookup_inode(filename)
//    Looks up `filename` in the root directory.

chkfs::inode* chkfsstate::lookup_inode(const char* filename) {
    auto dirino = get_inode(1);
    if (dirino) {
        dirino->lock_read();
        auto ino = fs.lookup_inode(dirino, filename);
        dirino->unlock_read();
        dirino->put();
        return ino;
    } else {
        return nullptr;
    }
}

// ===== Bitset helper functions ====== //
// NOTE: all bitset helper functions below assume that the fbb cache entry
// is locked by the caller.

bool chkfsstate::block_is_free(void* fbb, blocknum_t bn) {
    bitset_view fbb_view(reinterpret_cast<uint64_t*>(fbb), chkfs::bitsperblock);
    return fbb_view[bn];
}

void chkfsstate::mark_block_free(void* fbb, blocknum_t bn) {
    bitset_view fbb_view(reinterpret_cast<uint64_t*>(fbb), chkfs::bitsperblock);
    fbb_view[bn] = true;
}

void chkfsstate::mark_block_taken(void* fbb, blocknum_t bn) {
    bitset_view fbb_view(reinterpret_cast<uint64_t*>(fbb), chkfs::bitsperblock);
    fbb_view[bn] = false;
}

auto chkfsstate::find_free_range(void* fbb, unsigned count, size_t start) -> blocknum_t {
    bitset_view fbb_view(reinterpret_cast<uint64_t*>(fbb), chkfs::bitsperblock);
    size_t last_searched = start; // Index of bit most recently searched
    while (true) {
        // a. Find the next available free block.
        size_t next_available_block = fbb_view.find_lsb(last_searched);
        if (next_available_block == (unsigned long) -1 
            || next_available_block == (1 << 15)) {
            log_printf("No data blocks left to allocate in chckfs\n");
            return -1;
        }
        assert(block_is_free(reinterpret_cast<void*>(fbb), next_available_block));

        // b. Check if the entire contiguous array is available. If so, mark and break.
        last_searched = fbb_view.find_lsz(next_available_block, count); // Next un-free block
        log_printf("last_searched (lsz) = %d, next_available (lsb) = %d, count = %d\n",
            last_searched, next_available_block, count);
        assert(last_searched > next_available_block);
        if (last_searched - next_available_block == count) {
            return next_available_block;
        }
    }
}

void chkfsstate::free_extent(unsigned first, unsigned count) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();
    log_printf("Free extent called\n");
    auto fbb = bc.get_disk_entry(sb.fbb_bn);
    auto irqs = fbb->lock_.lock();
    unsigned i = 0;
    for (blocknum_t bn = first; i < count; ++bn, ++i) {
        mark_block_free(reinterpret_cast<void*>(fbb->buf_), bn);
        assert(block_is_free(reinterpret_cast<void*>(fbb->buf_), bn));
    }
    fbb->lock_.unlock(irqs);
    fbb->put();
}

bool chkfsstate::examine_block(blocknum_t bn) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();
    auto fbb = bc.get_disk_entry(sb.fbb_bn);
    auto irqs = fbb->lock_.lock();
    bool isfree = block_is_free(fbb, bn);
    fbb->lock_.unlock(irqs);
    fbb->put();
    return isfree;
}

// chkfsstate::allocate_extent(unsigned count)
//    Allocates and returns the first block number of a fresh extent.
//    The returned extent doesn't need to be initialized (but it should not be
//    in flight to the disk or part of any incomplete journal transaction).
//    Returns the block number of the first block in the extent, or an error
//    code on failure. Errors can be distinguished by
//    `blocknum >= blocknum_t(E_MINERROR)`.

auto chkfsstate::allocate_extent(unsigned count) -> blocknum_t {
    // First just verify that count isn't insane--must be less than 2^15 since
    // that's what our bitmap has.
    if (count > (1 << 15)) {
        log_printf("chkfs unable to handle allocation of %d size\n", count);
        return E_FBIG;
    }

    // 1. Load the free block bitmap into the buffer cache and get a pointer to it.
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();
    auto fbb = bc.get_disk_entry(sb.fbb_bn);

    // 2. Lock that entry and walk through it, attempting to find a contiguous range
    // of `count` blocks. Keep track of the first one. If one is found, walk from the first 
    // one to the last one, use the taken version of `mark_block_free`.
    auto irqs = fbb->lock_.lock();
    blocknum_t first = find_free_range(reinterpret_cast<void*>(fbb->buf_), count, 0);
    if (first == (blocknum_t) -1) {
        return E_NOSPC;
    }
    log_printf("free range from bn=%d of count=%d found\n", first, count);
    // mark_blocks_taken(reinterpret_cast<void*>(fbb), first, count);
    unsigned i = 0;
    for (blocknum_t bn = first; i < count; ++bn, ++i) {
        mark_block_taken(reinterpret_cast<void*>(fbb->buf_), bn);
        assert(!block_is_free(reinterpret_cast<void*>(fbb->buf_), bn));
    }
    fbb->lock_.unlock(irqs);
    fbb->put();
    return first;
}


// diskfile_loader::get_page
//    Load a page from the disk into the cache and point *pg to it.
//    Cannot be called consecutively without calling diskfile_loader::put_page() first.
ssize_t diskfile_loader::get_page(uint8_t** pg, size_t off) {
    if (!ino_) {
        return E_NOENT;
    } 
    // read file inode
    chkfs_fileiter it(ino_);
    // copy data from current block
    if (bcentry* e = it.find(off).get_disk_entry()) {
        unsigned b = it.block_relative_offset();
        *pg = (uint8_t*) e->buf_ + b;
        curr_pg_ = e;
        return chkfs::blocksize - b;              // bytes left in block
    } else {
        return -1;
    }

}

// diskfile_loader::put_page()
//    Decrement refcount of page retrieved by diskfile_loader::get_page().
void diskfile_loader::put_page() {
    curr_pg_->put();
}

