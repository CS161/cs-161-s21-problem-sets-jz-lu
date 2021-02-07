#include "u-lib.hh"
#define ALLOC_SLOWDOWN 24

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

#define TESTNO 1 // Change this number from 0 to (TODO) for the different tests.

// Test cases for the buddy allocator.

void process_main() {
    sys_kdisplay(KDISPLAY_MEMVIEWER);
    sys_map_console(console); // Map the console to the addr iin lib.hh
    for (int i = 0; i < CONSOLE_ROWS * CONSOLE_COLUMNS; ++i) {
        console[i] = 'T' | 0x8C00; // T as in Troy...? NO YOU PLEBIAN IT'S T AS IN TEST 
    }

    if (TESTNO == 1) {
        // Allocate and then free 100 blocks of sz 3 * PAGESIZE.
        pid_t p = sys_getpid();
        srand(p);
        
        // The heap starts on the page right after the 'end' symbol,
        // whose address is the first address not allocated to process code
        // or data.
        heap_top = reinterpret_cast<uint8_t*>(
            round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE)
        );

        // The bottom of the stack is the first address on the current
        // stack page (this process never needs more than one stack page).
        stack_bottom = reinterpret_cast<uint8_t*>(
            round_down(rdrsp() - 1, PAGESIZE)
        );

        const uint64_t num_allocs = 100;
        uint64_t sz = 3 * PAGESIZE;
        void *ptrs[num_allocs];
        for (uint64_t i = 0; i < num_allocs; ++i) {
            if (heap_top == stack_bottom || sys_varalloc(heap_top, PAGESIZE) < 0) {
                break;
            }
            ptrs[i] = heap_top;
            *heap_top = p;      /* check we have write access to new page */
            heap_top += sz;
            
            sys_yield();
            if (rand() < RAND_MAX / 32) {
                sys_pause();
            }
        }

        for (uint64_t i = 0; i < num_allocs; ++i) {
            sys_free(ptrs[i]);
        }
    }

    // After test is complete, do nothing forever
    while (true) {
        sys_yield();
    }

    panic("Should never reach here u bimbo!\n");
}
