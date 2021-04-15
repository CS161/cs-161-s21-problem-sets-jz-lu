#include "u-lib.hh"
#define ALLOC_SLOWDOWN 124

extern uint8_t end[];

void process_main() {
    sys_kdisplay(KDISPLAY_FDVIEWER);

    sys_map_console(console); // Map the console to the addr in lib.hh
    // Fork three new copies. (But ignore failures.)
    (void) sys_fork();
    (void) sys_fork();

    pid_t p = sys_getpid();
    srand(p);

    while (true) {
        int rn = rand(0, ALLOC_SLOWDOWN - 1);
        if (rn < p) {
            int rnmod = rn%5;
            int r;
            switch (rnmod) {
                case 0: // pipe
                    int pfd[2];
                    r = sys_pipe(pfd);
                    break;
                
                case 1: // disk file, read
                    r = sys_open("thoreau.txt", OF_READ);
                    break;

                case 2: // disk file, write
                    r = sys_open("thoreau.txt", OF_WRITE);
                    break;

                case 3: // disk file, read/write
                    r = sys_open("thoreau.txt", OF_RDWR);
                    break;
                
                default: // special file
                    r = sys_open("/dev/null", OF_RDWR);
                    break;
            }
            if (r == E_MFILE) {
                break;
            }
        }
        sys_yield();
        if (rand() < RAND_MAX / 32) {
            sys_pause();
        }
    }

    // After running out of memory, do nothing forever
    while (true) {
        sys_yield();
    }
}
