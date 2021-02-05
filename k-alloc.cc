#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

// Buddy allocator data structure.
bapg pgmap[MEMSIZE_PHYSICAL/PAGESIZE];
list<bapg, &bapg::pglink> free_blocks[MAX_ORDER - MIN_ORDER + 1];


// largest_fitting_ord(num, sz)
//  returns largest power of 2 that fits inside of the given number
// TODO change sz to a static
static uint64_t largest_fitting_ord(uint64_t num, int sz=1) {
    if (num == 1) {
        return sz - 1;
    }
    else if (num == 0) {
        return 0;
    }
    else {
        return largest_fitting_ord(num >> 1, ++sz);
    }
}

// order(sz)
//   Equivalently ceiling(log_2(sz))
static uint64_t order(uint64_t sz) {
    uint64_t y = sz - 1;
    uint64_t ord = 0;
    while (sz >>= 1) ord++;

    // If it is a power of 2 don't add 1.
    if (!(y & (y+1))) {
        return ord;
    } else { // Otherwise add 1 for ceiling.
        return ord;
    }
}


// init_kalloc()
//    Initialize stuff needed by kalloc. Called from init_hardware,
//    after physical_ranges is initialized.
void init_kalloc() {
    auto range = physical_ranges.begin();

    while (range != physical_ranges.end()) {
        if (range->type() == mem_available) {
            uintptr_t curr_addr = range->first();
            uint64_t remaining_size = range->last() - curr_addr;
            log_printf("NEW mem init at pa 0x%x with raw size 0x%x\n", curr_addr, remaining_size);

            while (remaining_size != 0) {
                unsigned int ord = largest_fitting_ord(remaining_size);
                assert(ord >= MIN_ORDER && ord <= MAX_ORDER);
                uint64_t current_size = 1 << ord;
                log_printf("Current addr: 0x%x, remaining size to cut: 0x%x\n", curr_addr, remaining_size);
                log_printf("Current largest order = %lu, size: 0x%x\n", ord, current_size);

                for (uint64_t i = curr_addr / PAGESIZE;  
                    i < (curr_addr + current_size) / PAGESIZE;
                    i++) {

                    pgmap[i].free = true;
                    pgmap[i].ord = ord;
                    pgmap[i].r_ord = ord;
                    pgmap[i].r_addr = curr_addr; // change this
                }

                // Push back the free blocks.
                free_blocks[ord - MIN_ORDER].push_back(&pgmap[curr_addr/PAGESIZE]);
                assert(!free_blocks[ord - MIN_ORDER].empty());
                log_printf("PUSHED block of order %lu onto free list\n", ord);

                curr_addr += current_size;
                remaining_size -= current_size;
            }
        }
        ++range;
    }
}


// kalloc(sz)
//    Allocate and return a pointer to at least sz contiguous bytes of
//    memory. Returns nullptr if sz == 0 or on failure.
//
//    The caller should initialize the returned memory before using it.
//    The handout allocator sets returned memory to 0xCC (this corresponds
//    to the x86 int3 instruction and may help you debug).
//
//    If sz is a multiple of PAGESIZE, the returned pointer is guaranteed
//    to be page-aligned.
//
//    The handout code does not free memory and allocates memory in units
//    of pages.
void* kalloc(size_t sz) {
    if (sz == 0 || sz > (1 << MAX_ORDER)) {
        log_printf("Invalid memory request\n");
        return nullptr;
    }

    auto irqs = page_lock.lock();
    void* ptr = nullptr;
    uint64_t ord = order(sz);
    log_printf("[kalloc] NEW request of size 0x%x, ord %lu\n", sz, ord);
    ord = ord >= MIN_ORDER ? ord : MIN_ORDER; // min alloc is 1 page
    assert(ord <= MAX_ORDER, 
        "Error: attempted memory allocation of order larger than maximum\n");
    log_printf("[kalloc] Actual ord given: %lu\n", ord);
    bapg* blk = nullptr; // New block pointer

    // If there are no free blocks of the desired order, then make one via looped block chopping.
    if (!free_blocks[ord - MIN_ORDER].front()) { 
        log_printf("[kalloc] FAILED to find avialable free block of order %lu, proceeding to create one\n", ord);
        uint64_t nb_ord = ord; // next best order
        
        while (nb_ord < MAX_ORDER) { // finding next available higher order block
            if (free_blocks[++nb_ord - MIN_ORDER].front()) {
                bapg* new_blk = free_blocks[nb_ord - MIN_ORDER].pop_front();
                log_printf("[kalloc] FOUND NBB of order %lu, sz 0x%x, addr 0x%x\n", 
                    nb_ord, 1 << nb_ord, (new_blk-pgmap)*PAGESIZE);

                long counter = 1;
                while (nb_ord > ord) { // Iteratively break down block until order matches desired
                    bapg* split_page = new_blk + (1 << (nb_ord - MIN_ORDER - 1));
                    log_printf("[kalloc] SPLITTING at pa 0x%x (raw index %lu)\n", 
                        (split_page-pgmap)*PAGESIZE, (split_page-pgmap));

                    for (bapg* it = split_page; 
                        it - split_page < (1 << (nb_ord - MIN_ORDER - 1)); 
                        it++) { // Update the order for each page in the new child block 

                        it->ord = nb_ord - 1;
                    }

                    // Push back the right child into the list of the given order.
                    // The left child continues to get split until it is of the right order.
                    free_blocks[nb_ord - MIN_ORDER - 1].push_back(split_page);
                    assert(!free_blocks[nb_ord - MIN_ORDER - 1].empty());
                    --nb_ord;
                } // At this point, new_blk points to a block of the order we desire
                log_printf("[kalloc] FINISHED splitting, new block of order %lu at addr 0x%x\n",
                    nb_ord, (new_blk-pgmap)*PAGESIZE);
                blk = new_blk;
                break;
            } 
        }
    } else { // Huzzah! There is already a block of the desired order free
        log_printf("[kalloc] SUCCESS! Found new block of order %lu\n", ord);
        blk = free_blocks[ord - MIN_ORDER].pop_front();
    }

    if (!blk) {
        log_printf("[kalloc] Error: insufficient memory blocks available for requested size 0x%x\n", sz);
        page_lock.unlock(irqs);
        return nullptr;
    }

    // Compute the physical address of the block. Note that the index of pgmap is also the index
    // of the page in physical memory! So the index is just the offset in physical memory
    // in units of PAGESIZE's.
    uint64_t addr = (blk - pgmap)*PAGESIZE;
    log_printf("[kalloc] ALLOCATED pa = 0x%x\n", addr);
    ptr = pa2kptr<void*>(addr);
    blk->returned = true;

    for (bapg* it = blk; it - blk < (1 << (ord - MIN_ORDER)); it++) {
        it->free = false;
        it->ord = ord;
    }

    page_lock.unlock(irqs);

    // Tell sanitizers the allocated page is accessible
    asan_mark_memory(ka2pa(ptr), 1 << ord, false);
    // Initialize to int3
    memset(ptr, 0xCC, 1 << ord);
    
    return ptr;
}

// buddy_to_the_left(addr)
//    Returns a yes (1) or no (0) of whether the (physical) addr block's memory has buddy on left.
//    Assumes as a promise that there is a buddy, either to right or left (in physical mem).
static bool buddy_to_the_left(bapg* blk) {
    uint64_t addr = (blk - pgmap) * PAGESIZE;

    // Normalize the addr so that the root block (ol' grandpa) of blk sits at tnorm-address 0.
    // (tnorm means translation-normalized).
    uint64_t tnorm_addr = addr - blk->r_addr;

    // Mask everything but the bit of the given order, if it is a right buddy (i.e. its buddy
    // is on the left) then the mask will fail to zero out the normed addr, 
    // otherwise it zeroes it out and the buddy is not on the left (i.e. right).
    return tnorm_addr & (1 << blk->ord);
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    if (!ptr) { // Do nothing
        return;
    }

    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    assert(addr & 0xfff == 0); // assert page-aligned pointer
    bapg *blk = &(pgmap[addr / PAGESIZE]);
    log_printf("[kfree] kfree called to free %p corr. to pa = 0x%x\n", ptr, addr);

    assert(blk->returned, 
        "Error: attempted free on pointer not returned by kalloc()\n"); // kalloc gave this addr it away in the first place
    assert(!blk->free, 
        "Error: attempted free on already freed block\n"); // it's not already free
    blk->returned = false;
    blk->free = true;

    // Iteratively search for free buddies to combine with until we cannot anymore, 
    // then push to the relevant free list.
    while (true) {
        if (blk->ord == blk->r_ord) { // No buddies left (base case)
            free_blocks[blk->ord - MIN_ORDER].push_back(blk);
            log_printf("[kfree] NO BUDDIES left; root order reached. Freeing immediately\n");
            break;
        } else { // We have a buddy, hooray! Let's check if it's free.
            uint64_t buddy_addr = buddy_to_the_left(blk) ?
                addr - (1 << blk->ord) /* left bud */ : addr + (1 << blk->ord) /* right bud */;
            bapg *buddy_blk = &(pgmap[buddy_addr / PAGESIZE]);
            log_printf("[kfree] FOUND BUDDY at pa = 0x%x (for blk at pa = 0x%x). Is it free?", buddy_addr, addr);
            if (buddy_blk->free) {
                assert(!buddy_blk->returned, "WTF you did something rly bad\n");
                buddy_blk->ord++;
                blk->ord++;
                if (buddy_addr < addr) { // If buddy is on the left then update the block for next iter
                    blk = buddy_blk;
                    addr = buddy_addr;
                }
                log_printf("Yes!\n [kfree] Adjoining block to %s buddy, updating new pa to 0x%x", 
                    buddy_addr < addr ? "left" : "right", addr);
            } else { // Darn! The buddy isn't free. Guess we'll just free what we have.
                log_printf("No (sad) \n[kfree] FREED block of order %lu at pa = 0x%x\n", 
                    blk->ord, PAGESIZE*(blk-pgmap));
                free_blocks[blk->ord - MIN_ORDER].push_back(blk);
                break;
            }
        }
    }

    if (ptr) {
        // tell sanitizers the freed page is inaccessible
        asan_mark_memory(ka2pa(ptr), PAGESIZE, true);
    }
}


// operator new, operator delete
//    Expressions like `new (std::nothrow) T(...)` and `delete x` work,
//    and call kalloc/kfree.
void* operator new(size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new(size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void operator delete(void* ptr) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
