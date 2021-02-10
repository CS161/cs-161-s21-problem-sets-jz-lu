#ifndef CHICKADEE_K_MEMRANGE_HH
#define CHICKADEE_K_MEMRANGE_HH
#include "types.h"
template <unsigned maxsize> class memrangeset;

// Slab allocator constants
#define SLAB_FREE 0
#define SLAB_PARTIAL 1
#define SLAB_FULL 2
#define SMALL_CHUNKSIZE 128
#define BIG_CHUNKSIZE 512
#define NUM_SMALL_SLABS PAGESIZE/SMALL_CHUNKSIZE - 1
#define NUM_BIG_SLABS 2*PAGESIZE/BIG_CHUNKSIZE - 1
#define SLAB_CANARY 984257
#define SLAB_THRESHOLD BIG_CHUNKSIZE - 8

// Buddy allocator struct encoding metadata for each page under the buddy allocator system.
struct bapg {
    uint64_t free = 0; // whether the bapg is free or not
    uint64_t returned = 0; // if the address of this page has been returned in kalloc().
    uint64_t ord; // order
    uint64_t r_ord; // order of the root block associated with current block
    uintptr_t r_addr; // address of the root block associated with the current block
    list_links pglink; // To make struct available for link lists
};

// Units of memory in the slab, called "chunks." The first 8 bytes of the chunks are reserved for
// metadata; be careful to always return chunk_ptr + 8 bytes to user, and subtract by 8 bytes 
// when freeing the user pointer to get the right dereferencing, or you will be doing some WILD frees.
struct small_chunk {
    unsigned short sz = SMALL_CHUNKSIZE;
    unsigned short ind;
    const unsigned int canary = SLAB_CANARY;
    unsigned char data[SMALL_CHUNKSIZE - sizeof(int) - 2*sizeof(short)];
};

struct big_chunk {
    unsigned short sz = BIG_CHUNKSIZE;
    unsigned short ind;
    const unsigned int canary = SLAB_CANARY;
    unsigned char data[BIG_CHUNKSIZE - sizeof(int) - 2*sizeof(short)];
};

// Small slab allocator
struct smallslab {
    private:
        int64_t nallocs_ = 0; // Number of allocations
    public:
        uint64_t state = SLAB_FREE;
        list_links pglink; // Linked list element
        int64_t ind; // Index slab within linked list, so we know where to return mem to
        small_chunk chunks[NUM_SMALL_SLABS]; // Chunks of SMALL_CHUNKSIZE for allocs
        unsigned char free_chunks[NUM_SMALL_SLABS]; // Indexes whether a given chunk is free or not

    smallslab(uint64_t index) {
        // Set the index and the size.
        ind = index;
        for (uint64_t i = 0; i < NUM_SMALL_SLABS; ++i) {
            free_chunks[i] = 1; // Initialize every chunk to be free
            chunks[i].sz = SMALL_CHUNKSIZE;
            chunks[i].ind = index;
        }
    }

    // Allocate a chunk
    void* give() {
        assert(state != SLAB_FULL, "Slab is full, clearly you did not implement the allocator correctly\n");
        uint64_t free_ind = 0;
        while (free_ind < NUM_SMALL_SLABS) {
            if (free_chunks[free_ind]) {
                free_chunks[free_ind] = 0;

                // Update the state if necessary.
                if (++nallocs_ == NUM_SMALL_SLABS) {
                    state = SLAB_FULL;
                }
                else if (nallocs_ > 0) {
                    state = SLAB_PARTIAL;
                }

                return this->chunks + free_ind;
            }
            ++free_ind;
        }
        assert(false);
        return nullptr; // Should never get here!
    }

    // Free a chunk.
    void receive(void *ptr) {
        small_chunk *sc = reinterpret_cast<small_chunk*>(ptr);
        assert(sc->ind == ind);
        assert(sc->sz == SMALL_CHUNKSIZE);
        assert(sc->canary == SLAB_CANARY, "Boundary overwrite detected during slab free\n");
        assert(free_chunks[sc-this->chunks] == 0, "Error: attempted free of allocated chunk in slab\n");
        free_chunks[sc-this->chunks] = 1; // Mark chunk as free in slab

        // Update the state if necessary.
        if (--nallocs_ > 0) {
            state = SLAB_PARTIAL;
        }
        else {
            state = SLAB_FREE;
        }
        return;
    }
};

// Big slab allocator
struct bigslab {
    private:
        int64_t nallocs_ = 0; // Number of allocations
    public:
        uint64_t state = SLAB_FREE;
        list_links pglink; // Linked list element
        int64_t ind; // Index slab within linked list, so we know where to return mem to
        big_chunk chunks[NUM_BIG_SLABS]; // Chunks of SMALL_CHUNKSIZE for allocs
        unsigned char free_chunks[NUM_BIG_SLABS]; // Indexes whether a given chunk is free or not

    bigslab(uint64_t index) {
        // Set the index and the size.
        ind = index;
        for (uint64_t i = 0; i < NUM_BIG_SLABS; ++i) {
            free_chunks[i] = 1; // Initialize every chunk to be free
            chunks[i].sz = BIG_CHUNKSIZE;
            chunks[i].ind = index;
        }
    }

    // Allocate a chunk
    void* give() {
        assert(state != SLAB_FULL, "Slab is full, clearly you did not implement the allocator correctly\n");
        uint64_t free_ind = 0;
        while (free_ind < NUM_BIG_SLABS) {
            if (free_chunks[free_ind]) {
                free_chunks[free_ind] = 0;

                // Update the state if necessary.
                if (++nallocs_ == NUM_BIG_SLABS) {
                    state = SLAB_FULL;
                }
                else if (nallocs_ > 0) {
                    state = SLAB_PARTIAL;
                }

                return this->chunks + free_ind;
            }
            ++free_ind;
        }

        assert(false);
        return nullptr; // Should never get here!
    }

    // Free a chunk.
    void receive(void *ptr) {
        big_chunk *bc = reinterpret_cast<big_chunk*>(ptr);
        assert(bc->ind == ind);
        assert(bc->sz == BIG_CHUNKSIZE);
        assert(bc->canary == SLAB_CANARY, "Boundary overwrite detected during slab free\n");
        assert(free_chunks[bc-this->chunks] == 0, "Error: attempted free of allocated chunk in slab\n");
        free_chunks[bc-this->chunks] = 1; // Mark chunk as free in slab

        // Update the state if necessary.
        if (--nallocs_ > 0) {
            state = SLAB_PARTIAL;
        }
        else {
            state = SLAB_FREE;
        }
        return;
    }
};

// `memrangeset` stores type information for a range of memory addresses.
// Chickadee uses it to remember which physical addresses are reserved or
// occupied by the kernel.

class memrange {
  public:
    inline int type() const;                // type of range
    inline uintptr_t first() const;         // first address in range
    inline uintptr_t last() const;          // one past last address in range

  private:
    uintptr_t addr_;
    int type_;

    template <unsigned maxsize> friend class memrangeset;
};


template <unsigned maxsize>
class memrangeset {
  public:
    // initialize range mapping [0, `limit`) to type 0
    inline memrangeset(uintptr_t limit);

    // return `limit`, i.e. address of the end
    inline uintptr_t limit() const;
    // return number of ranges
    inline unsigned size() const;
    // return pointer to first range
    inline const memrange* begin() const;
    // return pointer to range bound
    inline const memrange* end() const;

    // return range containing `addr`, or `end()`
    const memrange* find(uintptr_t addr) const;
    // return type for `addr`. Requires `0 <= addr < limit()`
    int type(uintptr_t addr) const;

    // set type of address range [`first`, `last`) to `type`.
    // Requires `0 <= first <= last <= limit()`. Returns true iff
    // assignment succeeded.
    bool set(uintptr_t first, uintptr_t last, int type);

    // print contents to log
    void log_print(const char* prefix = "") const;
    // validate correctness of data structure
    void validate() const;

  private:
    unsigned n_;
    memrange r_[maxsize + 1];

    void split(unsigned i, uintptr_t addr);
};


inline int memrange::type() const {
    return type_;
}
inline uintptr_t memrange::first() const {
    return addr_;
}
inline uintptr_t memrange::last() const {
    return this[1].addr_;
}

template <unsigned maxsize>
inline memrangeset<maxsize>::memrangeset(uintptr_t limit)
    : n_(1) {
    r_[0].addr_ = 0;
    r_[0].type_ = 0;
    r_[1].addr_ = limit;
}
template <unsigned maxsize>
inline uintptr_t memrangeset<maxsize>::limit() const {
    return r_[n_].addr_;
}
template <unsigned maxsize>
inline unsigned memrangeset<maxsize>::size() const {
    return n_;
}
template <unsigned maxsize>
inline const memrange* memrangeset<maxsize>::begin() const {
    return &r_[0];
}
template <unsigned maxsize>
inline const memrange* memrangeset<maxsize>::end() const {
    return &r_[n_];
}
template <unsigned maxsize>
inline const memrange* memrangeset<maxsize>::find(uintptr_t addr) const {
    unsigned i = 0;
    while (i < n_ && addr >= r_[i + 1].addr_) {
        ++i;
    }
    return &r_[i];
}
template <unsigned maxsize>
inline int memrangeset<maxsize>::type(uintptr_t addr) const {
    auto r = find(addr);
    assert(r != end());
    return r->type();
}
template <unsigned maxsize>
void memrangeset<maxsize>::split(unsigned i, uintptr_t addr) {
    assert(i < n_ && n_ + 1 <= maxsize);
    assert(r_[i].addr_ < addr && r_[i + 1].addr_ > addr);
    memmove(&r_[i + 2], &r_[i + 1], (n_ - i) * sizeof(memrange));
    r_[i + 1].addr_ = addr;
    r_[i + 1].type_ = r_[i].type_;
    ++n_;
}
template <unsigned maxsize>
bool memrangeset<maxsize>::set(uintptr_t first, uintptr_t last, int type) {
    assert(first <= last && last <= r_[n_].addr_);
    if (first == last) {
        return true;
    }

    // find lower bound of insertion position for range
    unsigned i = 0;
    while (first >= r_[i + 1].addr_) {
        ++i;
    }
    assert(i < n_ && first >= r_[i].addr_ && first < r_[i + 1].addr_);
    // try consolidating range
    if (first == r_[i].addr_ && i > 0 && r_[i - 1].type_ == type) {
        --i;
    }
    if (r_[i].type_ == type) {
        first = r_[i].addr_;
    }

    // find upper bound of insertion position for range
    unsigned j = i;
    while (j < n_ && last >= r_[j + 1].addr_) {
        ++j;
    }
    assert(j <= n_ && last >= r_[j].addr_
           && (j == n_ || last < r_[j + 1].addr_));
    if (j < n_ && r_[j].type_ == type) {
        ++j;
        last = r_[j].addr_;
    } else if (j > i && first == r_[i].addr_) {
        r_[j].addr_ = last;
    }

    // split range at left and right
    if (n_ + (first != r_[i].addr_) + (last != r_[j].addr_) > maxsize) {
        return false;
    }
    if (first != r_[i].addr_) {
        split(i, first);
        ++i;
        ++j;
    }
    if (last != r_[j].addr_) {
        split(j, last);
        ++j;
    }

    // assign new range
    assert(r_[i].addr_ == first && r_[j].addr_ == last);
    r_[i].type_ = type;
    if (i + 1 < j) {
        memmove(&r_[i + 1], &r_[j], (n_ + 1 - j) * sizeof(memrange));
        n_ -= j - (i + 1);
    }
    return true;
}
template <unsigned maxsize>
void memrangeset<maxsize>::validate() const {
    assert(n_ > 0 && n_ <= maxsize);
    assert(r_[0].addr_ == 0);
    for (unsigned i = 0; i < n_; ++i) {
        assert(r_[i].addr_ < r_[i + 1].addr_);
        assert(i == 0 || r_[i].type_ != r_[i - 1].type_);
    }
}
template <unsigned maxsize>
void memrangeset<maxsize>::log_print(const char* prefix) const {
    void log_printf(const char* format, ...) __attribute__((noinline));
    for (unsigned i = 0; i < n_; ++i) {
        log_printf("%s[%u]: [%p,%p)=%d\n",
                   prefix, i, r_[i].first(), r_[i].last(), r_[i].type());
    }
}

#endif
