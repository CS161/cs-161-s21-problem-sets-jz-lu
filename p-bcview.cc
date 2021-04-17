#include "u-lib.hh"
#define ALLOC_SLOWDOWN 8

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main() {
    sys_msleep(10);
    sys_kdisplay(KDISPLAY_BUFCACHE);

    sys_map_console(console); // Map the console to the addr iin lib.hh

    // Fork three new copies. (But ignore failures.)
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

    while (true) {
        if (rand(0, ALLOC_SLOWDOWN - 1) < p) {
            // TODO
        }
        sys_yield();
        if (rand() < RAND_MAX / 32) {
            sys_pause();
        }
    }

    // Do nothing forever
    while (true) {
        sys_yield();
    }
}
