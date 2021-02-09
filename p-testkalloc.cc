#include "u-lib.hh"
#define ALLOC_SLOWDOWN 8

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

    (void) sys_fork();
    (void) sys_fork();

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
    int test_counter = 0;
    int ntests = 4;
    while (test_counter < ntests) {
        sys_testkalloc(test_counter++);
        sys_yield();
        if (rand() < RAND_MAX / 32) {
            sys_pause();
        }
    }

    // After test is complete, do nothing forever
    while (true) {
        sys_yield();
    }

    panic("Should never reach here u bimbo!\n");
}