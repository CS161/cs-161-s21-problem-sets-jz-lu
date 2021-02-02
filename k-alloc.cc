#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;

bapg pg_map[MEMSIZE_PHYSICAL/PAGESIZE];

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



// init_kalloc
//    Initialize stuff needed by kalloc. Called from init_hardware,
//    after physical_ranges is initialized.
void init_kalloc() {

    auto range = physical_ranges.begin();

    while (range != physical_ranges.end()) {
        if (range->type() == mem_available) {

            uintptr_t curr_addr = range->first();
            uint64_t remaining_size = range->last() - curr_addr;

            while (remaining_size != 0) {
                unsigned int ord = largest_fitting_ord(remaining_size);
                assert(ord >= MIN_ORDER && ord <= MAX_ORDER);
                uint64_t current_size = 1 << ord;

                for (uint64_t i = curr_addr / PAGESIZE;  
                    i < (curr_addr + current_size)/PAGESIZE;
                    i++) {

                    pg_map[i].available = true;
                    pg_map[i].free = true;
                    pg_map[i].ord = ord;
                    pg_map[i].r_ord = ord;
                    pg_map[i].r_addr = curr_addr; // change this
                }
                curr_addr += current_size;
                remaining_size -= current_size;
            }
        }
        ++range;
    }
}


// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes of
//    memory. Returns `nullptr` if `sz == 0` or on failure.
//
//    The caller should initialize the returned memory before using it.
//    The handout allocator sets returned memory to 0xCC (this corresponds
//    to the x86 `int3` instruction and may help you debug).
//
//    If `sz` is a multiple of `PAGESIZE`, the returned pointer is guaranteed
//    to be page-aligned.
//
//    The handout code does not free memory and allocates memory in units
//    of pages.
void* kalloc(size_t sz) {
    if (sz == 0 || sz > PAGESIZE) {
        return nullptr;
    }
    static double te = 0;

    auto irqs = page_lock.lock();
    void* ptr = nullptr;

    auto range = physical_ranges.find(next_free_pa);
    while (range != physical_ranges.end()) {
        if (range->type() == mem_available) {
            // use this page
            ptr = pa2kptr<void*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        } else {
            // move to next range
            next_free_pa = range->last();
            ++range;
        }
    }

    page_lock.unlock(irqs);

    if (ptr) {
        // tell sanitizers the allocated page is accessible
        asan_mark_memory(ka2pa(ptr), PAGESIZE, false);
        // initialize to `int3`
        memset(ptr, 0xCC, PAGESIZE);
    }
    else {
        log_printf("[kalloc] FAILED to find available page.\n");
    }
    return ptr;
}


// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    if (ptr) {
        // tell sanitizers the freed page is inaccessible
        asan_mark_memory(ka2pa(ptr), PAGESIZE, true);
    }
    log_printf("kfree not implemented yet\n");
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
