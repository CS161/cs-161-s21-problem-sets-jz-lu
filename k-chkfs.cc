#include "k-chkfs.hh"
#include "k-ahci.hh"
#include "k-chkfsiter.hh"

bufcache bufcache::bc;

// Debugging helper function: visualize dirty list block numbers.
[[maybe_unused]] static void print_dirty_list() {
    bufcache& bc = bufcache::get();
    log_printf("Dirty --> [ ");
    for (auto it = bc.dirty_list_.front(); it; it = bc.dirty_list_.next(it)) {
        log_printf("%d ", it->bn_);
    }
    log_printf("]\n");
}

// Debugging helper function: visualize evict q list block numbers.
// Use for light single-process debugging only--violates state read invariants for multiple cores.
[[maybe_unused]] static void print_evict_queue() {
    bufcache& bc = bufcache::get();
    log_printf("Evictq (es@bn) --> [ ");
    for (auto it = bc.evictq_.front(); it; it = bc.evictq_.next(it)) {
        int localstate = it->estate_;
        log_printf("%d@%d ", localstate, it->bn_);
    }
    log_printf("]\n");
}

bufcache::bufcache() {
}

// bufcache::evict()
//    Evict a block from the bufcache, if possible, and
//    return the index into the bufcache entry array of the free block.
//    Assumes the bufcache is locked, but the entry is not (no changes made to it).
size_t bufcache::evict() {
    bcentry* blk = evictq_.pop_front();
    if (!blk) { // Nothing to pop
        // Attempt to pop a prefetch if it's done.
        // (pfq_ is protected by bufcache lock, which is held by caller.)
        // NOTE: there is an inherent race where a prefetch completes after the loop
        // has already walked past it. The method below guarantees that an eviction is
        // possible but not guaranteed from the prefetch queue. We leave it up to the caller
        // if sufficiently desparate for eviction to keep calling it, while the pfq_ is
        // nonempty and evict() does not return -1.
        for (auto it = pfq_.front(); it; it = pfq_.next(it)) {
            if (it->pfstatus_ != E_AGAIN) {
                size_t idx = it->index();
                pfq_.erase(it);
                it->lock_.lock_noirq();
                it->clear();
                assert(it->estate_ == bcentry::es_empty);
                it->lock_.unlock_noirq();
                return idx;
            }
        }
        return -1;
    } else { // Compute the index into the bufcache and return it
        size_t blk_index = blk->index();
        assert(blk_index < ne);
        blk->lock_.lock_noirq();
        blk->clear();
        assert(blk->estate_ == bcentry::es_empty);
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
            // If the block is prefetching here, set it to clean
            // and proceed as usual. (And pop off prefetching queue).
            if (e_[i].estate_ == bcentry::es_prefetching) {
                 waiter().block_until(read_wq_, [&] () {
                    return e_[i].pfstatus_ != E_AGAIN;
                }, lock_, irqs);
            }

            e_[i].lock_.lock_noirq();
            if (e_[i].estate_ != bcentry::es_prefetching) {
                e_[i].lock_.unlock_noirq();
                break;
            }

            ++e_[i].ref_;
            assert(!e_[i].qlink_.is_linked());
            e_[i].estate_ = bcentry::es_clean;
            if (cleaner) {
                cleaner(&e_[i]);
            }
            pfq_.erase(&e_[i]);
            if (SLOT_AND_PREFETCH_EXAMINE) {
                log_printf("Desired block was already prefetched!\n");
            }

            lock_.unlock_noirq();
            e_[i].lock_.unlock(irqs);
            return &e_[i];
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


// bcentry::prefetch()
//    Prefetches (nonblocking) the next 2 blocks of a I/O, or first 2 blocks
//    if a file is just being opened.
int bufcache::prefetch(chkfs::inode* ino, off_t off, bool inclusive, int nfetch) {
    if (!inclusive) {
        off += chkfs::blocksize; // Switch to next block if not grabbing first block
    }

    chkfs_fileiter it(ino);
    for (int nremaining = nfetch; it.find(off).active() && nremaining > 0;
         --nremaining, off+=chkfs::blocksize) {
        auto irqs = lock_.lock();

        blocknum_t bn = it.blocknum();
        bool already_fetched = false;
        size_t i, empty_slot = -1;

        for (i = 0; i != ne; ++i) {
            if (e_[i].empty()) {
                assert(!e_[empty_slot].qlink_.is_linked());
                if (empty_slot == size_t(-1)) {
                    empty_slot = i;
                }
            } else if (e_[i].bn_ == bn) {
                already_fetched = true;
                break;
            }
        }

        if (already_fetched) {
            lock_.unlock(irqs);
            continue;
        }

        if (empty_slot == size_t(-1)) {
            empty_slot = evict();
            if (empty_slot == size_t(-1)) {
                lock_.unlock(irqs);
                return -1;
            }
        }

        e_[empty_slot].lock_.lock_noirq();
        e_[empty_slot].bn_ = bn;
        e_[empty_slot].prefetch_block();
        e_[empty_slot].lock_.unlock_noirq();
        assert(!e_[empty_slot].qlink_.is_linked());
        lock_.unlock(irqs);
    }

    return 0;
}


// bcentry::prefetch_block()
//    Prefetches a block from disk without blocking.
bool bcentry::prefetch_block() {
    assert(estate_ == es_empty);
    if (!buf_) {
        buf_ = reinterpret_cast<unsigned char*>
            (kalloc(chkfs::blocksize));
        if (!buf_) {
            return false;
        }
    }
    pfstatus_ = E_AGAIN; // Initialize
    estate_ = es_prefetching;
    bufcache::get().pfq_.push_back(this);
    sata_disk->read(buf_, chkfs::blocksize,
                    bn_ * chkfs::blocksize, pfstatus_);  // this is the non-blocking version
    return true;    
}


// bcentry::put()
//    Releases a reference to this buffer cache entry. The caller must
//    not use the entry after this call.

void bcentry::put() {
    bufcache& bc = bufcache::get();
    spinlock_guard bcguard(bc.lock_);
    spinlock_guard guard(lock_);
    assert(ref_ != 0);
    if (--ref_ == 0 && bn_ != 0 && estate_ != es_dirty && estate_ != es_prefetching) {
        bc.evictq_.push_back(this);
    }
}


// bcentry::get_write()
//    Obtains a write reference for this entry.

void bcentry::get_write(bool push) {
    {
    spinlock_guard guard(lock_);
    assert(wref_ == 0 || wref_ == 1);
    if (wref_ == 1) {
        waiter().block_until(wq_, [&] () {
            return (wref_ == 0);
        }, guard);
    }
    ++wref_;
    }
    if (push) {
        bufcache& bc = bufcache::get();
        spinlock_guard bcguard(bc.lock_);
        spinlock_guard guard(lock_);
        
        if (!dlink_.is_linked()) {
            estate_ = bcentry::es_dirty;
            bc.dirty_list_.push_back(this);
            assert(bc.dirty_list_.front());
            assert(dlink_.is_linked());
        }
    }
}


// bcentry::put_write()
//    Releases a write reference for this entry.
void bcentry::put_write() {
    spinlock_guard guard(lock_);
    assert(wref_ == 1);
    --wref_;
    wq_.wake_all();
}

// bufcache::sync(drop)
//    Writes all dirty buffers to disk, blocking until complete.
//    If `drop > 0`, then additionally free all buffer cache contents,
//    except referenced blocks. If `drop > 1`, then assert that all inode
//    and data blocks are unreferenced.

int bufcache::sync(int drop) {
    // write dirty buffers to disk
    list<bcentry, &bcentry::dlink_> local_dirty;
    auto irqs = lock_.lock();
    local_dirty.swap(dirty_list_);
    lock_.unlock(irqs);

    while (bcentry* e = local_dirty.pop_front()) {
        e->get_write(false); // Don't mark the block as dirty!
        sata_disk->write(e->buf_, chkfs::blocksize, e->bn_ * chkfs::blocksize);
        {
        spinlock_guard eguard(e->lock_);
        e->estate_ = bcentry::es_clean;
        if (e->ref_ == 0 && e->bn_ != 0 && drop <= 0) {
            assert(e->estate_ != bcentry::es_empty);
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
                if (e_[i].qlink_.is_linked()) {
                    bufcache::get().evictq_.erase(&e_[i]);
                } else if (e_[i].pflink_.is_linked()) {
                    assert(!e_[i].qlink_.is_linked());
                    bufcache::get().pfq_.erase(&e_[i]);
                }
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

// chkfsstate::ino_to_inum
//    Compute the inum of an inode ptr.
//    Assumes the inode Does not put the inode back.
//    inums are static to an inode--does not require any locks.
chkfs::inum_t chkfsstate::ino_to_inum(chkfs::inode* ino) {
    // Compute the inum from the inode ptr, using an inverse function of the 
    // inum -> inode* function used in get_inode().
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();

    bcentry* ie = ino->entry();
    inum_t mod_inum = ino - reinterpret_cast<inode*>(ie->buf_); // inum % inodesperblock
    return (ie->bn_ - sb.inode_bn)*chkfs::inodesperblock + mod_inum;
}



// chkfsstate::get_inode(inum)
//    Returns inode number `inum`, or `nullptr` if there's no such inode.
//    Obtains a reference on the buffer cache block containing the inode;
//    you should eventually release this reference by calling `ino->put()`.
//    An inode can be grabbed to cache without r/w locks.
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
void inode::put(bool user_file) {
    chkfsstate& fs = chkfsstate::get();
    bcentry* ie = entry();
    lock_write(); // Prevents multiple threads from freeing inode at once
    if (user_file) {
        log_printf("[put] putting inum=%d, ref=%d, link=%d\n", 
        fs.ino_to_inum(this), (flags >> 1), (flags & 1));
        flags -= (1 << 1); // Decrement per-inode refcount if it is a file
    }
    if (!(flags >> 1) && ((flags & 1) == chkfs::unlinked)) {
        unlock_write();
        log_printf("freeing inode\n");
        int r = fs.free_inode(this);
        assert(!r);
    } else {
        unlock_write();
    }
    ie->put();
}
}


// chkfsstate::lookup_directory(pathname, access_last)
//    Walks directory and returns the dirino of the last directory,
//    which is either the end of the string, or the second last if the end is a filename.
//    Assumes that the root directory is locked.
chkfs::inode* chkfsstate::lookup_directory(const char* pathname, bool access_last) {
    char* s = (char*) pathname;
    char dlm = '/';
    int ndelims = 0;
    bool slash_end = false;
    bool from_root = false; // If true, start at root dir, else start at pwd
    chkfs::inode* curdir = nullptr; // current directory

    if (s[0] == dlm) {
        ++s;
        from_root = true;
    }
    if (from_root) {
        curdir = get_inode(1);
    } else {
        // Start at pwd.
        curdir = get_inode(1); // TODO change to cwd
    }
    if (!curdir) {
        return nullptr;
    }
    if (s[strlen(s)-1] == dlm) {
        s[strlen(s)-1] = '\0';
        slash_end = true;
    }

    // Parse the string, and replace delimiters with nulls terminators.
    for (int i = 0; s[i]; ++i) {
        if (s[i] == dlm) {
            s[i] = '\0';
            ++ndelims;
        }
    }
    if (!ndelims) {
        if (!access_last) {
            return curdir;
        } else { // TODO not the case if we have a pwd, add else if in that case
            curdir->put();
            return nullptr;
        }
    }

    for (; ndelims >= 0; --ndelims) {
        // Search for the next direntry at each step.
        chkfs_fileiter it(curdir);
        chkfs::inum_t in = 0;
        for (size_t diroff = 0; !in; diroff += blocksize) {
            if (bcentry* e = it.find(diroff).get_disk_entry()) {
                size_t bsz = min(curdir->size - diroff, blocksize);
                auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
                for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                    if (dirent->inum && strcmp(dirent->name, s) == 0) {
                        in = dirent->inum;
                        break;
                    }
                }
                e->put();
            } else {
                goto lookup_unsuccessful;
            }
        }
        curdir->put();
        curdir = get_inode(in);
        if (!curdir) {
            goto get_unsuccessful;
        }
        if (curdir->type != chkfs::type_directory) { // Traversal must be directory
            goto lookup_unsuccessful;
        }

        s += strlen(s) + 1;
        if (!ndelims && access_last) { // Don't grab a file--we want the directory
            break;
        }
    }

    return curdir;

    lookup_unsuccessful:
        log_printf("Lookup of '%s' unsuccessful\n", s);
        curdir->unlock_read();
        curdir->put();
    get_unsuccessful:
        log_printf("inode get of '%s' unsuccessful\n", s);
        return nullptr;
}


bool chkfsstate::contains(chkfs::inode* dirino, chkfs::inum_t inum) {
    // Explore a given directory and return true 
    // if directory contains inode with given inum.
    chkfs_fileiter it(dirino);
    bool contains = false;
    bcentry* de = nullptr;
    for (size_t diroff = 0; ; diroff += blocksize) {
        if ((de = it.find(diroff).get_disk_entry())) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(de->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (dirent->inum == inum) { // Found inode
                    contains = true;
                    break;
                }
            }
            de->put();
            if (contains) {
                break;
            }
        } else {
            break;
        }
    }
    return contains;
}


chkfs::inode* chkfsstate::lookup_directory(chkfs::inode* start_dirino, chkfs::inode* ino) {
    chkfs::inum_t inum = ino_to_inum(ino);

    // DFS through the directory tree, stopping when the inode has been found.
    if (contains(start_dirino, inum)) { // Base case 1: found inode
        return start_dirino;
    } else {
        // Walk through the direntries of the start directory, and compute the inode,
        // if the inode is a directory, recurse on it.
        chkfs_fileiter it(start_dirino);
        bcentry* de = nullptr;
        chkfs::inode* returned_dirino = nullptr;
        for (size_t diroff = 0; ; diroff += blocksize) {
            if ((de = it.find(diroff).get_disk_entry())) {
                size_t bsz = min(start_dirino->size - diroff, blocksize);
                auto dirent = reinterpret_cast<chkfs::dirent*>(de->buf_);
                for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                    chkfs::inode* found_ino = get_inode(dirent->inum);
                    if (found_ino->type == chkfs::type_directory) {
                        if (chkfs::inode* lookup_ino = lookup_directory(found_ino, ino)) {
                            returned_dirino = lookup_ino;
                            break;
                        }
                    }
                    found_ino->put();
                }
                de->put();
                if (returned_dirino) {
                    return returned_dirino;
                }
            } else { // Base case 2: nothing left to search
                return nullptr;
            }
        }
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
    auto root = get_inode(1);
    if (!root) {
        return nullptr;
    }
    root->lock_read();
    auto dirino = lookup_directory(filename);
    if (dirino) {
        auto ino = fs.lookup_inode(dirino, filename);
        root->unlock_read();
        dirino->put();
        root->put();
        return ino;
    } else {
        root->unlock_read();
        root->put();
        return nullptr;
    }
}


char* chkfsstate::path_find_last(char* s) {
    int ndelims = 0;
    if (s[0] == '/') {
        ++s;
    }
    if (s[strlen(s)-1] == '/') {
        s[strlen(s)-1] = '\0';
    }
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '/') {
            s[i] = '\0';
            ++ndelims;
        }
    }
    for (; ndelims > 0; --ndelims) {
        s += strlen(s) + 1;
    }
    return s;
}

// Assumes `dirino` is write-locked.
chkfs::dirent* chkfsstate::allocate_direntry(chkfs::inode* dirino, bcentry*& de) {
    chkfs_fileiter it(dirino);
    de = nullptr;
    chkfs::dirent* open_entry = nullptr;
    // Traverse the directory until a free entry is found.
    for (size_t diroff = 0; !open_entry; diroff += chkfs::blocksize) { // Block walk
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            size_t bsz = min(dirino->size - diroff, chkfs::blocksize);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) { // direntry walk in block
                if (dirent->inum == 0) {
                    open_entry = dirent;
                    assert(open_entry);
                    de = e;
                    break;
                }
            }
            if (!de) {
                e->put();
            }
        } else {
            // Allocate a new directory block and add to extent. No need to walk the individual
            // direntries this time--since it is a new allocation the first direntry is free.
            chkfs::blocknum_t first = fs.allocate_extent(1);
            if (first >= chkfs::blocknum_t(E_MINERROR)) {
                log_printf("Error in allocate_extent: unable to allocate new directory block\n");
            } else {
                int r = it.insert(first, 1);
                if (r < 0) {
                    log_printf("Error in insert: unable to insert new directory block\n");
                    fs.free_extent(first, 1);
                }
            }
            de = it.find(diroff).get_disk_entry();
            if (!de) {
                return nullptr;
            }
            open_entry = reinterpret_cast<chkfs::dirent*>(de->buf_);
        }
    }
    return open_entry;
}


// chkfsstate::rename_direntry(dirino, filename)
//    Same function as chkfsstate:lookup_inode, but renames the direntry in the walk
//    instead of fetching and returning the inode. Assumes `dirino` is write-locked.
int chkfsstate::rename_direntry(inode* dirino,
                                       const char* oldname, const char* newname) {
    chkfs_fileiter it(dirino);

    // Walk entire directory to check for duplicate names, then adjust 
    // if no duplicates at the end.
    chkfs::dirent* de = nullptr;
    bcentry* de_be = nullptr; // bufcache entry for directory
    bool name_already_exists = false; // another file has the same name
    for (size_t diroff = 0; !de && !name_already_exists; diroff += blocksize) {
        bool put = true;
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (strcmp(dirent->name, newname) == 0) {
                    name_already_exists = true;
                    break;
                } else if (dirent->inum && strcmp(dirent->name, oldname) == 0) {
                    de = dirent;
                    de_be = e;
                    put = false; // Don't put back the entry yet since we need it
                }
            }
            if (put) {
                e->put();
            }
        } else {
            break;
        }
    }

    if (name_already_exists) { // Duplicate name found
        if (de_be) {
            de_be->put();
        }
        return E_SAMENAME;
    } else if (!de) { // Could not find the current file
        assert(!de_be);
        return E_NOENT;
    }

    de_be->get_write();
    strcpy(de->name, newname);
    de_be->put_write();
    de_be->put();
    return 0;
}


// chkfsstate::rename_direntry(filename)
//    Rename direntry of `oldname` to `newname` in the root directory.
int chkfsstate::rename_direntry(const char* oldname, const char* newname) {
    auto root = get_inode(1);
    if (!root) {
        return E_NOENT;
    }
    root->lock_write();
    auto dirino = lookup_directory(oldname);
    if (dirino) {
        int r = fs.rename_direntry(dirino, oldname, newname);
        root->unlock_write();
        dirino->put();
        root->put();
        return r;
    } else {
        root->unlock_write();
        root->put();
        return E_NOENT;
    }
}


// chkfsstate::allocate_inode(type)
//    Returns inode number `inum` for newly allocate inode,
//    or returns 0 if none available. (0 is a reserved "always free" inum.)
//    Does NOT add refcounts to the inode. This is done by calling get_inode() on the inum returned.
chkfs::inum_t chkfsstate::allocate_inode(int type) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();

    chkfs::inode* ino = nullptr;
    inum_t free_in = 0;
    for (inum_t in = 2; in < sb.ninodes && !free_in; ++in) { // Start at inum 2, 0 is free, 1 is root dir
        auto bn = sb.inode_bn + in / chkfs::inodesperblock;
        if (auto inode_entry = bc.get_disk_entry(bn, clean_inode_block)) {
            ino = reinterpret_cast<inode*>(inode_entry->buf_);
            ino += in % chkfs::inodesperblock;
            ino->lock_write();
            if (ino->type == 0) {
                ino->entry()->get_write();
                ino->type = type;
                ino->flags = chkfs::linked;
                ino->size = 0;
                ino->nlink = 1; // One file referring to this upon allocation
                ino->entry()->put_write();
                free_in = in;
            }
            ino->unlock_write();
            ino->put();
        }
    }
    return free_in;
}


// chkfsstate::free_inode(dirino, ino)
//    Frees all extents of an inode, and marks inode as free.
//    Walks directory until direntry of inode is found, and frees it.
int chkfsstate::free_inode(inode* dirino, inode* ino) {
    assert(ino);
    assert(dirino != ino);

    bufcache& bc = bufcache::get();
    bcentry* ie = ino->entry();
    ino->lock_write();
    ie->get_write();
    // Free direct extents first.
    size_t eidx = 0;
    for (chkfs::extent* ex = ino->direct; ex->count && eidx < chkfs::ndirect; ++ex, ++eidx) {
        free_extent(ex->first, ex->count);
        ex->first = ex->count = 0;
    }

    // Then free all indirect extents.
    if (eidx == chkfs::ndirect && ino->indirect.count) {
        assert(ino->indirect.count == 1);
        bcentry* iee = bc.get_disk_entry(ino->indirect.first);
        assert(iee);
        chkfs::extent* iex = reinterpret_cast<chkfs::extent*>(iee);
        for (size_t i = 0; iex->count && i < chkfs::extentsperblock;  ++iex, ++i) {
            free_extent(iex->first, iex->count);
            iex->first = iex->count = 0;
        }

        // Free the indirect extent block itself.
        free_extent(ino->indirect.first, ino->indirect.count);
        iee->put();
    }

    // Mark the inode as free.
    ino->type = ino->size = ino->nlink = 0;
    ie->put_write();
    ino->unlock_write();
    return 0;
}


// chkfsstate::free_inode(ino)
//    Finds the directory of the inode and calls 
//    the free function above on that directory.
int chkfsstate::free_inode(inode* ino) {
    if (ino_to_inum(ino) <= 1) {
        return E_INVAL;
    }

    // Find the right directory of the inode.
    auto root_dirino = get_inode(1);
    chkfs::inode* dirino = root_dirino;
    if (!root_dirino) {
        return E_NOENT;
    }
    root_dirino->lock_read(); // Just to protect directory lookup
    ino->lock_write();
    ino->entry()->get_write();
    if ((ino->flags & 1) == chkfs::linked) {
        log_printf("[free_inode] Using lookup directory\n");
        dirino = lookup_directory(root_dirino, ino);
        root_dirino->unlock_read();
    } else {
        root_dirino->unlock_read();
        // If unlinked, we are about to free, so set it back to linked.
        int ref = (ino->flags >> 1);
        ino->flags = (ref << 1) + chkfs::linked;
    }
    ino->entry()->put_write();
    ino->unlock_write();
    if (ino_to_inum(dirino) != 1) {
        root_dirino->put(); // Avoid double put
    }

    // Free the inode.
    if (dirino) {
        dirino->lock_write();
        int r = fs.free_inode(dirino, ino);
        dirino->unlock_write();
        dirino->put();
        return r;
    } else {
        // Will never be reached if dirino is root.
        return E_NOENT;
    }
}


int chkfsstate::free_direntry(inode* dirino, inode* ino) {
    inum_t inum = ino_to_inum(ino);

    // Walk the directory until the direntry with the right inum is found; free that direntry.
    chkfs_fileiter it(dirino);
    bool found = false;
    bcentry* de = nullptr;
    for (size_t diroff = 0; !found; diroff += blocksize) {
        if ((de = it.find(diroff).get_disk_entry())) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(de->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (dirent->inum == inum) { // Found inode
                    de->get_write();
                    dirent->inum = 0;
                    dirent->name[0] = '\0';
                    found = true;
                    de->put_write();
                    break;
                }
            }
            de->put();
        } else {
            log_printf("[free_direntry] Directory entry of inode %d not found\n", inum);
            return E_NOENT;
        }
    }
    return 0;
}


int chkfsstate::free_direntry(inode* ino) {
    auto root_dirino = get_inode(1);
    if (!root_dirino) {
        return E_NOENT;
    }
    root_dirino->lock_write();
    auto dirino = lookup_directory(root_dirino, ino);
    if (dirino) {
        int r = fs.free_direntry(dirino, ino);
        root_dirino->unlock_write();
        if (ino_to_inum(dirino) != 1) {
            dirino->put();
        }
        log_printf("put on inum=%d, ref = %d\n", ino_to_inum(dirino), dirino->entry()->ref_);
        log_printf("same? %s\n", ino_to_inum(dirino) == ino_to_inum(root_dirino) ? "Yes" : "No");
        root_dirino->put();
        log_printf("put on inum=%d, ref = %d\n", ino_to_inum(root_dirino), root_dirino->entry()->ref_);
        log_printf("free_direntry returning %d\n", r);
        return r;
    } else {
        root_dirino->unlock_write();
        root_dirino->put();
        log_printf("[free_direntry] dirino not found\n");
        return E_NOENT;
    }
}


int chkfsstate::mkdir(char* path) {
    // First make sure that the directory doesn't already exist.
    if (lookup_directory(path, true)) {
        return E_SAMENAME;
    }

    // 1. Find the parent directory of the new child.
    chkfs::inode* dirino = lookup_directory(path);
    if (!dirino) {
        return E_NOENT;
    }
    dirino->lock_write();

    // 2. Allocate an inode for the new subdirectory.
    bcentry* de = nullptr;
    chkfs::dirent* open_entry = nullptr; // Declare above gotos
    chkfs::inum_t in = allocate_inode(chkfs::type_directory);
    if (in == 0) {
        goto failed_alloc;
    }

    // 3. Allocate a new direntry and set the name and inum.
    open_entry = allocate_direntry(dirino, de);
    if (!open_entry || !de) {
        goto failed_alloc;
    }

    // 4. Store the new data into the direntry.
    de->get_write();
    open_entry->inum = in;
    strcpy(open_entry->name, path_find_last(path));
    de->put_write();
    de->put();
    dirino->unlock_write();
    dirino->put();
    return 0;

    failed_alloc:
        dirino->entry()->put_write();
        dirino->unlock_write();
        dirino->put();
        return E_NOSPC;
}

int chkfsstate::rm(char* path) {
    // First make sure that the directory exists.
    if (!lookup_directory(path, true)) {
        return E_NOENT;
    }

    // Lookup the parent of the directory-to-remove.
    chkfs::inode* parent_dirino = lookup_directory(path);
    if (!parent_dirino) {
        return E_IO;
    }

    // Free the direntry and the inode.
    parent_dirino->lock_write();
    chkfs::inode* dirino = lookup_inode(parent_dirino, path_find_last(path));
    if (!dirino) {
        parent_dirino->put();
        return E_IO;
    }

    if (int r = free_direntry(parent_dirino, dirino)) {
        parent_dirino->unlock_write();
        return r;
    }
    
    parent_dirino->unlock_write();
    if (int r = free_inode(dirino) < 0) { 
        // No races here: another process trying to rm at the same time will fail to free
        // the direntry and will not reach here.
        return r;
    }

    return 0;
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
        assert(last_searched > next_available_block);
        if (last_searched - next_available_block == count) {
            return next_available_block;
        }
    }
}


// chkfsstate::free_extent(first, count)
//    Free allocations made by chkfsstate::allocate_extent() in FBB.
void chkfsstate::free_extent(unsigned first, unsigned count) {
    auto& bc = bufcache::get();
    auto superblock_entry = bc.get_disk_entry(0);
    assert(superblock_entry);
    auto& sb = *reinterpret_cast<chkfs::superblock*>
        (&superblock_entry->buf_[chkfs::superblock_offset]);
    superblock_entry->put();
    auto fbb = bc.get_disk_entry(sb.fbb_bn);
    fbb->get_write();
    unsigned i = 0;
    for (blocknum_t bn = first; i < count; ++bn, ++i) {
        mark_block_free(reinterpret_cast<void*>(fbb->buf_), bn);
        assert(block_is_free(reinterpret_cast<void*>(fbb->buf_), bn));
    }
    fbb->put_write();
    fbb->put();
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
    fbb->get_write();
    blocknum_t first = find_free_range(reinterpret_cast<void*>(fbb->buf_), count, 0);
    if (first == (blocknum_t) -1) {
        return E_NOSPC;
    }
    // mark_blocks_taken(reinterpret_cast<void*>(fbb), first, count);
    unsigned i = 0;
    for (blocknum_t bn = first; i < count; ++bn, ++i) {
        mark_block_taken(reinterpret_cast<void*>(fbb->buf_), bn);
        assert(!block_is_free(reinterpret_cast<void*>(fbb->buf_), bn));
    }
    fbb->put_write();
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

