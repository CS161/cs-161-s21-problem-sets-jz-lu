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
    
    if (sata_disk->read(buf_, chkfs::blocksize,
                    bn_ * chkfs::blocksize, pfstatus_)) {
            return false;
    }
    bufcache::get().pfq_.push_back(this);
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
    if (type == chkfs::type_regular) {
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
    }
    ie->put();
}
}


// chkfsstate::lookup_directory(pathname, access_last)
//    Walks directory and returns the dirino of the last directory,
//    which is either the end of the string, or the second last if the end is a filename.
//    Assumes that the root directory is locked.
chkfs::inode* chkfsstate::lookup_directory(const char* pathname, bool access_last, bool is_file) {
    log_printf("[lookup directory] called on '%s'\n", pathname);
    char str[strlen(pathname)+1];
    strcpy(str, pathname);
    char* s = str;
    char dlm = '/';
    int ndelims = 0;
    chkfs::inode* curdir = nullptr; // current directory

    if (s[0] == dlm) {
        ++s;
    }
    curdir = get_inode(1);
    if (!curdir) {
        log_printf("[lookup directory] Unable to fetch root directory, check bufcache is not overloaded\n");
        return nullptr;
    }
    if (s[strlen(s)-1] == dlm) {
        s[strlen(s)-1] = '\0';
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
            log_printf("[lookup directory] returning root\n");
            return curdir;
        } else if (is_file) { // TODO if we have a pwd, add another else if in that case
            log_printf("[lookup directory] returning null\n");
            curdir->put();
            return nullptr;
        }
    }

    for (; ndelims >= 0; --ndelims) {
        if (!ndelims && (!access_last || is_file)) { // Don't grab a file--we want the directory
            log_printf("[lookup directory] Breaking early, before searching '%s'\n", s);
            break;
        }
        // Search for the next direntry at each step.
        log_printf("[lookup directory] Searching directory inum=%d for '%s'\n", 
            ino_to_inum(curdir), s);
        chkfs_fileiter it(curdir);
        chkfs::inum_t in = 0;
        for (size_t diroff = 0; !in; diroff += blocksize) {
            if (bcentry* e = it.find(diroff).get_disk_entry()) {
                size_t bsz = min(curdir->size - diroff, blocksize);
                auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
                for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                    if (dirent->inum) {
                        log_printf("[lookup directory] Walked to '%s'\n", dirent->name);
                    }
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
            log_printf("[lookup directory] Error: inode looked up is not of type directory\n");
            goto lookup_unsuccessful;
        }

        s += strlen(s) + 1;
    }
    log_printf("[lookup directory] returning inum=%d\n", ino_to_inum(curdir));
    return curdir;

    lookup_unsuccessful:
        log_printf("[lookup directory] Error: Lookup of '%s' unsuccessful\n", s);
        curdir->put();
    get_unsuccessful:
        log_printf("[lookup directory] Error: inode get of '%s' unsuccessful\n", s);
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
        log_printf("made it here 1\n");
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
                        log_printf("made it here 2\n");
                        if (chkfs::inode* lookup_ino = lookup_directory(found_ino, ino)) {
                            log_printf("made it here 3\n");
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
                if (dirent->inum) {
                    log_printf("[post] [lookup inode] Walked to '%s'\n", dirent->name);
                }
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
        log_printf("[pre] [lookup inode] Parent dirino found, searching it for '%s' (full name '%s')\n", 
            path_find_last((char*) filename), filename);
        auto ino = fs.lookup_inode(dirino, path_find_last((char*) filename));
        root->unlock_read();
        dirino->put();
        root->put();
        log_printf("[pre] [lookup inode] inode lookup complete, returning %p\n", ino);
        return ino;
    } else {
        log_printf("[pre] [lookup inode] Error: directory of '%s' not found\n", filename);
        root->unlock_read();
        root->put();
        return nullptr;
    }
}


char* chkfsstate::path_find_last(char* s) {
    if (!strchr(s, '/')) {
        return s;
    }
    if (s[strlen(s)-1] == '/') {
        s[strlen(s)-1] = '\0';
    }
    int last_delim_index = 0;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '/') {
            last_delim_index = i;
        }
    }
    return s + last_delim_index + 1;
}


// Assumes `dirino` is write-locked.
chkfs::dirent* chkfsstate::allocate_direntry(chkfs::inode* dirino, bcentry*& de) {
    log_printf("[allocate direntry] Allocating a new direntry in inum=%d\n", ino_to_inum(dirino));
    chkfs_fileiter it(dirino);
    de = nullptr;
    chkfs::dirent* open_entry = nullptr;
    // Traverse the directory until a free entry is found.
    for (size_t diroff = 0; !open_entry; diroff += chkfs::blocksize) { // Block walk
        log_printf("[allocate direntry] looping: diroff at %lu\n", diroff);
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            log_printf("[allocate direntry] Searching the block for direntries\n");
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            size_t bsz = min(dirino->size - diroff, chkfs::blocksize);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) { // direntry walk in block
                log_printf("[allocate direntry] Walked to entry inum=%d, name=%s\n", 
                    dirent->inum, dirent->name);
                if (dirent->inum == 0) {
                    open_entry = dirent;
                    log_printf("[allocate direntry] Open entry found in bn=%d\n", e->bn_);
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
            log_printf("[allocate direntry] No room remaining in directory, allocating a new extent\n");
            chkfs::blocknum_t first = fs.allocate_extent(1);
            log_printf("[allocate direntry] Allocated new extent at first=%d\n", first);
            if (first >= chkfs::blocknum_t(E_MINERROR)) {
                log_printf("Error in allocate_extent: unable to allocate new directory block\n");
            } else {
                int r = it.insert(first, 1);
                if (r < 0) {
                    log_printf("Error in insert: unable to insert new directory block\n");
                    fs.free_extent(first, 1);
                }
            }
            diroff -= chkfs::blocksize;
            dirino->size += chkfs::blocksize;
            // de = it.find(diroff).get_disk_entry();
            // log_printf("[allocate direntry] New extent: bn=%d\n", de->bn_);
            // if (!de) {
            //     return nullptr;
            // }
            // open_entry = reinterpret_cast<chkfs::dirent*>(de->buf_);
        }
    }
    log_printf("[allocate entry] Succeeded\n");
    return open_entry;
}


// chkfsstate::rename_direntry(dirino, filename)
//    Same function as chkfsstate:lookup_inode, but renames the direntry in the walk
//    instead of fetching and returning the inode. Assumes `dirino` is write-locked.
int chkfsstate::rename_direntry(inode* dirino,
                                       const char* oldname, const char* newname) {
    log_printf("[post] [rename direntry] Renaming '%s' --> '%s'\n", oldname, newname);
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
    log_printf("[renamed direntry] called '%s' --> '%s'\n", oldname, newname);
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


// chkfsstate::directory_empty(inode* dirino)
bool chkfsstate::directory_empty(inode* dirino) {
    assert(dirino);
    assert(dirino->type == chkfs::type_directory);
    dirino->lock_read();
    chkfs_fileiter it(dirino);
    for (size_t diroff = 0; ; diroff += blocksize) {
        if (bcentry* e = it.find(diroff).get_disk_entry()) {
            size_t bsz = min(dirino->size - diroff, blocksize);
            auto dirent = reinterpret_cast<chkfs::dirent*>(e->buf_);
            for (unsigned i = 0; i * sizeof(*dirent) < bsz; ++i, ++dirent) {
                if (dirent->inum) {
                    e->put();
                    dirino->unlock_read();
                    log_printf("[directory empty] Error: directory inum=%d contains inode inum=%d\n",   
                        ino_to_inum(dirino), dirent->inum);
                    return false;
                }
            }
            e->put();
        } else {
            break;
        }
    }
    dirino->unlock_read();
    return true;
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
            if (ino->type != 0) {
                // Don't grab a lock if it is not free. Otherwise we lock before checking since
                // if locking after checking 2 processes can race on an allocation.
                continue;
            }
            ino->lock_write();
            if (ino->type == 0) {
                ino->entry()->get_write();
                ino->type = type;
                ino->flags = chkfs::linked;
                ino->size = 0;
                ino->nlink = 1; // One file referring to this upon allocation
                ino->entry()->put_write();
                free_in = in;
                log_printf("[allocate inode] Allocated inum=%d\n", in);
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
    log_printf("[post] [free inode] freeing inode inum=%d\n", ino_to_inum(ino));

    bufcache& bc = bufcache::get();
    bcentry* ie = ino->entry();
    ino->lock_write();
    ie->get_write();
    // Free direct extents first.
    size_t eidx = 0;
    for (chkfs::extent* ex = ino->direct; ex->count && eidx < chkfs::ndirect; ++ex, ++eidx) {
        free_extent(ex->first, ex->count);
        log_printf("[free inode] freed %d blocks starting from first=%d\n", ex->count, ex->first);
        ex->first = ex->count = 0;
    }

    // Then free all indirect extents.
    if (eidx == chkfs::ndirect && ino->indirect.count) {
        assert(ino->indirect.count == 1);
        bcentry* iee = bc.get_disk_entry(ino->indirect.first);
        assert(iee);
        chkfs::extent* iex = reinterpret_cast<chkfs::extent*>(iee);
        for (size_t i = 0; iex->count && i < chkfs::extentsperblock; ++iex, ++i) {
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
    log_printf("[pre] [free inode] called\n");
    if (ino_to_inum(ino) <= 1) {
        return E_INVAL;
    }

    // Find the right directory of the inode.
    auto root_dirino = get_inode(1);
    chkfs::inode* dirino = root_dirino;
    if (!root_dirino) {
        return E_NOENT;
    }
    assert(ino != root_dirino); // No freeing the root!
    root_dirino->lock_read(); // Just to protect directory lookup
    assert(ino->type != 0); // No double frees
    if (ino->type == chkfs::type_regular) { // Free a file
        ino->lock_write();
        ino->entry()->get_write();
        if ((ino->flags & 1) == chkfs::linked) {
            dirino = lookup_directory(root_dirino, ino);
            log_printf("[pre] [free_inode] lookup directory returned dirino inum=%d\n", ino_to_inum(dirino));
        } else {
            // If unlinked, we are about to free, so set it back to linked.
            int ref = (ino->flags >> 1);
            ino->flags = (ref << 1) + chkfs::linked;
        }
        ino->entry()->put_write();
        ino->unlock_write();
    }
    root_dirino->unlock_read();
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
                    log_printf("[post] [free direntry] Found dirent with inum=%d, name=%s, freeing\n",
                        dirent->inum, dirent->name);
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
        root_dirino->put();
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
    log_printf("[mkdir] About to make directory for '%s'\n", path);
    auto root = get_inode(1);
    if (!root) {
        log_printf("[mkdir] Error: Failed to find root directory\n");
        return E_NOENT;
    }
    root->lock_read();
    if (lookup_directory(path, true)) {
        root->unlock_read();
        root->put();
        log_printf("[mkdir] Error: there is already a directory path '%s'\n", path);
        return E_SAMENAME;
    }

    // 1. Find the parent directory of the new child.
    log_printf("[mkdir] Attempting to find the parent directory of the new subdirectory...\n");
    chkfs::inode* dirino = lookup_directory(path);
    if (!dirino) {
        root->unlock_read();
        root->put();
        log_printf("[mkdir] Error: parent directory of new subdirectory not found\n");
        return E_NOENT;
    }
    root->unlock_read();
    root->put();
    dirino->lock_write();
    log_printf("[mkdir] Found parent directory of the new subdirectory...\n");

    // 2. Allocate an inode for the new subdirectory.
    bcentry* de = nullptr;
    chkfs::dirent* open_entry = nullptr; // Declare above gotos
    log_printf("[mkdir] Allocating a new dirino for the new subdirectory...\n");
    chkfs::inum_t in = allocate_inode(chkfs::type_directory);
    if (in == 0) {
        goto failed_alloc;
    }
    log_printf("[mkdir] inode allocated successfully\n");

    // 3. Allocate a new direntry and set the name and inum.
    log_printf("[mkdir] Allocating a new direntry in the parent for the new subdirectory...\n");
    open_entry = allocate_direntry(dirino, de);
    if (!open_entry || !de) {
        goto failed_alloc;
    }
    log_printf("[mkdir] direntry allocated successfully\n");

    // 4. Store the new data into the direntry.
    de->get_write();
    open_entry->inum = in;
    strcpy(open_entry->name, path_find_last(path));
    log_printf("[mkdir] Stored new name '%s' in new direntry\n", path_find_last(path));
    de->put_write();
    de->put();
    dirino->unlock_write();
    dirino->put();
    log_printf("[mkdir] mkdir done\n");
    return 0;

    failed_alloc:
        dirino->entry()->put_write();
        dirino->unlock_write();
        dirino->put();
        return E_NOSPC;
}

int chkfsstate::rm(char* path) {
    // First make sure that the directory exists.
    log_printf("[rm] rm called, confirming directory '%s' exists...\n", path);
    chkfs::inode* dir = lookup_directory(path, true, false);
    if (!dir) {
        log_printf("Directory does not exist\n");
        return E_NOENT;
    }
    // Traverse the parent directory and ensure that it is empty.
    log_printf("[rm] Directory exists, confirming directory is empty...\n");
    if (!directory_empty(dir)) {
        log_printf("[rm] Error: Attempted removal of nonempty directory '%s'\n", path);
        dir->put();
        return E_NONEMPTY;
    }
    dir->put(); // Put back the directory--we don't need it.
    log_printf("[rm] Validated directory\n");

    // Lookup the parent of the directory-to-remove.
    log_printf("[rm] Looking up parent of directory...\n");
    chkfs::inode* parent_dirino = lookup_directory(path);
    if (!parent_dirino) {
        log_printf("[rm] Error: failed to find parent directory of path '%s'\n", path);
        return E_IO;
    }

    log_printf("[rm] All validataions passed, removing directory...\n");

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

// ===== CWD Functions ===== //

void cwd::lock_read() {
    chkfs::mlock_t v = mlock.load(std::memory_order_relaxed);
    while (true) {
        if (v >= chkfs::mlock_t(-2)) {
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

void cwd::unlock_read() {
    chkfs::mlock_t v = mlock.load(std::memory_order_relaxed);
    assert(v != 0 && v != chkfs::mlock_t(-1));
    while (!mlock.compare_exchange_weak(v, v - 1,
                                        std::memory_order_release)) {
        pause();
    }
}

void cwd::lock_write() {
    assert(!has_write_lock());
    chkfs::mlock_t v = 0;
    while (!mlock.compare_exchange_weak(v, chkfs::mlock_t(-1),
                                        std::memory_order_acquire)) {
        current()->yield();
        v = 0;
    }
}

void cwd::unlock_write() {
    assert(has_write_lock());
    mlock.store(0, std::memory_order_release);
}

bool cwd::has_write_lock() const {
    return mlock.load(std::memory_order_relaxed) == chkfs::mlock_t(-1);
}


char* cwd::write(char* s) {
    char buf[strlen(s)];
    strcpy(buf, s);
    int blen = strlen(buf);
    if (buf[blen-1] != '/') {
        buf[blen+1] = '\0';
        buf[blen] = '/';
    }

    // First make sure that the directory exists.
    log_printf("[cwd::write] cd called, confirming directory '%s' exists...\n", buf);
    chkfs::inode* dir = chkfsstate::get().lookup_directory(buf, true, false);
    if (!dir) {
        log_printf("[cwd::write] Directory does not exist\n");
        return nullptr;
    }

    lock_write();
    char* ret = strcpy(this->name, buf);
    unlock_write();
    return ret;
}

int cwd::len() {
    return strlen(name);
}

char* cwd::cat(char* s, char* buf, bool dir) {
    if (strlen(s) + strlen(name) > chkfs::maxnamelen-2) {
        return nullptr;
    }
    strcpy(buf, name);
    if (s[0] == '/') {
        ++s;
    }
    memcpy((void*) (buf+strlen(name)), (void*) s, strlen(s)+1);
    int blen = strlen(buf);
    if (dir && buf[blen-1] != '/') {
        buf[blen+1] = '\0';
        buf[blen] = '/';
    }
    return buf;
}

void cwd::reset() {
    // Reset the CWD to root directory.
    lock_write();
    char rt[2] = "/";
    strcpy(name, rt);
    unlock_write();
}

char* cwd::read(char* buf) {
    buf[0] = '~';

    lock_read();
    char* ret = strcpy(buf + 1, this->name);
    unlock_read();
    
    return ret;
}

