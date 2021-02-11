#include "u-lib.hh"
#define ALLOC_SLOWDOWN 8

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

// NOTE: this defines which (if any) wild allocations to run. See pset1answers.md for details.
#define WILDNO 0
#define FORKTESTING 1

// Test cases for the buddy allocator.

void process_main() {
    sys_kdisplay(KDISPLAY_MEMVIEWER);
    sys_map_console(console); // Map the console to the addr iin lib.hh
    for (int i = 0; i < CONSOLE_ROWS * CONSOLE_COLUMNS; ++i) {
        console[i] = 'T' | 0x8C00; // T as in Troy...? NO YOU PLEBIAN IT'S T AS IN TEST 
    }

    (void) sys_fork();
    (void) sys_fork();

    if (FORKTESTING) {
        (void) sys_fork();
        (void) sys_fork();
        (void) sys_fork();
        (void) sys_fork();
    }

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

    if (FORKTESTING) {
        while (true) {
            if (rand(0, ALLOC_SLOWDOWN - 1) < p) {
                if (heap_top == stack_bottom || sys_page_alloc(heap_top) < 0) {
                    break;
                }
                *heap_top = p;      /* check we have write access to new page */
                heap_top += PAGESIZE;
            }
            sys_yield();
            if (rand() < RAND_MAX / 32) {
                sys_pause();
            }
        }
    } else {
        int tstart = 0;
        int ntests = 8;
        while (tstart < ntests) {
            sys_testkalloc(tstart++);
            sys_yield();
            if (rand() < RAND_MAX / 32) {
                sys_pause();
            }
        }

        // Run a wild allocation if specified.
        sys_wildkalloc(WILDNO);
    }

    // After tests are complete, do nothing forever
    while (true) {
        sys_yield();
    }

    panic("Should never reach here u bimbo!\n");
}