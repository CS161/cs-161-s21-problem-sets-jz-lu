#ifndef CHICKADEE_K_CHKFS_HH
#define CHICKADEE_K_CHKFS_HH
#include "kernel.hh"
#include "chickadeefs.hh"
#include "k-lock.hh"
#include "k-wait.hh"

// buffer cache

using bcentry_clean_function = void (*)(bcentry*);

struct bcentry {
    using blocknum_t = chkfs::blocknum_t;

    enum estate_t {
        es_empty, es_allocated, es_loading, es_clean, es_dirty, es_prefetching
    };

    std::atomic<int> estate_ = es_empty;

    spinlock lock_;                             // protects most `estate_` changes
    blocknum_t bn_;                             // disk block number (unless empty)
    unsigned ref_ = 0;                          // reference count
    std::atomic<unsigned int> wref_ = 0;        // Write reference "lock"
    unsigned char* buf_ = nullptr;              // memory buffer used for entry
    wait_queue wq_;                             // Write reference wait queue
    list_links qlink_, dlink_, pflink_;         // Eviction, dirty, prefetch list links
    std::atomic<int> pfstatus_ = E_AGAIN;       // Prefetching status, used only when prefetching


    // return the index of this entry in the buffer cache
    inline size_t index() const;

    // test if this entry is empty (`estate_ == es_empty`)
    inline bool empty() const;

    // test if this entry's memory buffer contains a pointer
    inline bool contains(const void* ptr) const;

    // release the caller's reference
    void put();

    // obtain/release a write reference to this entry
    void get_write(bool push=true);
    void put_write();


    // internal functions
    void clear();
    bool load(irqstate& irqs, bcentry_clean_function cleaner);
    bool prefetch_block();
};

struct bufcache {
    using blocknum_t = bcentry::blocknum_t;

    static constexpr size_t ne = 32;                // Number of entries in the cache

    spinlock lock_;                                 // protects all entries' bn_ and ref_
    wait_queue read_wq_;                            // Processes waiting on a prefetch
    bcentry e_[ne];                                 // Entries
    list<bcentry, &bcentry::qlink_> evictq_;        // Eviction queue (refcount 0 blocks only!)
    list<bcentry, &bcentry::dlink_> dirty_list_;    // List of dirty blocks
    list<bcentry, &bcentry::pflink_> pfq_;          // Prefetching blocks queue

    static inline bufcache& get();
    bool full();

    bcentry* get_disk_entry(blocknum_t bn,
                            bcentry_clean_function cleaner = nullptr);
    int prefetch(chkfs::inode* ino, off_t off, bool inclusive=false, int nfetch=8);

    int sync(int drop);
    
 private:
    static bufcache bc;

    bufcache();
    size_t evict();                             // Evict a block and return newly freed block index
    bool has_uref_dirty_blocks();               // Checks if any dirty blocks are unref'd
    NO_COPY_OR_ASSIGN(bufcache);
};


// diskfile::loader: loads a `proc` from a `memfile`
struct diskfile_loader : public proc_loader {
    chkfs::inode* ino_;
    bcentry* curr_pg_ = nullptr;
    inline diskfile_loader(chkfs::inode* ino, x86_64_pagetable* pt)
        : proc_loader(pt), ino_(ino) {
    }

    ssize_t get_page(uint8_t** pg, size_t off) override;
    void put_page() override;
};


// chickadeefs state: a Chickadee file system on a specific disk
// (Our implementation only speaks to `sata_disk`.)

struct chkfsstate {
    using blocknum_t = chkfs::blocknum_t;
    using inum_t = chkfs::inum_t;
    using inode = chkfs::inode;
    static constexpr size_t blocksize = chkfs::blocksize;

    static inline chkfsstate& get();
    // Find the end of a path string, e.g. for '/users/bigka$h/hi.txt' returns ptr to 'hi.txt'
    char* path_find_last(char* s);

    // obtain an inode by number
    inode* get_inode(inum_t inum);

    // directory inode lookup, starting from user path specification, requires fstlock
    inode* lookup_directory(const char* pathname, bool access_last=false, bool is_file=true);
    // directory inode lookup, starting from an inode ptr, requires fstlock
    inode* lookup_directory(inode* start_dirino, inode* ino);
    // inode lookup in directory `dirino`, requires fstlock
    inode* lookup_inode(inode* dirino, const char* name);
    // inode lookup starting at root directory, requires fstlock
    inode* lookup_inode(const char* name);
    // allocate a new direntry in a specified directory, return direntry and set de
    // requires fstlock. Also needs dirino lock purely for legacy compatibility with chkfsiter.
    chkfs::dirent* allocate_direntry(inode* dirino, bcentry*& de);
    // direntry rename in directory `dirino`, requires fstlock
    // Also needs dirino lock purely for legacy compatibility with chkfsiter.
    int rename_direntry(inode* dirino, const char* oldname, const char* newname);
    // direntry rename starting at root directory, requires fstlock
    int rename_direntry(const char* oldname, const char* newname);
    // direntry free in directory `dirino`, requires fstlock
    // Also needs dirino lock purely for legacy compatibility with chkfsiter.
    int free_direntry(inode* dirino, inode* ino);
    // free direntry corresponding to the given inode, requires fstlock
    int free_direntry(inode* ino);
    // checks if a directory_is empty, requires fstlock
    bool directory_empty(inode* dirino);
    // allocate new inode of a specific type, requires fstlock
    inum_t allocate_inode(int type);
    // free an inode living in directory dirino, requires fstlock
    int free_inode(inode* dirino, inode* ino);
    // free an inode, requires fstlock
    int free_inode(inode* ino);
    // make a new subdirectory, requires fstlock
    int mkdir(char* path);
    // remove a blank subdirectory, requires fstlock
    int rm(char* path);
    // list contents of a directory, requires fstlock
    int ls(const char* pathname, char* buf, size_t bufsz);
    // print out directory tree
    int tree_dfs(inode* dirino, char* buf, size_t bufsz, off_t& off, int depth=0);
    int tree(const char* pathname, char* buf, size_t bufsz);

    bool block_is_free(void* fbb, blocknum_t bn);
    void mark_block_free(void* fbb, blocknum_t bn);
    void mark_block_taken(void* fbb, blocknum_t bn);
    void free_extent(unsigned first, unsigned count);
    blocknum_t find_free_range(void* fbb, unsigned count, size_t start);
    inum_t ino_to_inum(inode* ino);
    blocknum_t allocate_extent(unsigned count = 1);

  private:
    static chkfsstate fs;

    chkfsstate();
    // Does the dirino contain a direntry with given name/inum?
    bool contains(inode* dirino, const char* name);
    bool contains(inode* dirino, inum_t inum);
    NO_COPY_OR_ASSIGN(chkfsstate);
};


inline bufcache& bufcache::get() {
    return bc;
}

inline chkfsstate& chkfsstate::get() {
    return fs;
}

inline size_t bcentry::index() const {
    auto& bc = bufcache::get();
    assert(this >= bc.e_ && this < bc.e_ + bc.ne);
    return this - bc.e_;
}

inline bool bcentry::empty() const {
    return estate_.load(std::memory_order_relaxed) == es_empty;
}

inline bool bcentry::contains(const void* ptr) const {
    return estate_.load(std::memory_order_relaxed) >= es_clean
        && reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(buf_)
               < chkfs::blocksize;
}

inline void bcentry::clear() {
    assert(ref_ == 0);
    estate_ = es_empty;
    if (buf_) {
        kfree(buf_);
        buf_ = nullptr;
    }
}

#endif
