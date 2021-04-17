#include "u-lib.hh"
#define ALLOC_SLOWDOWN 8

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;
char bigbuf[4096];

void small_pause() {
    sys_msleep(500);
    sys_yield();
}

void large_pause() {
    sys_msleep(1000);
    sys_yield();
}

size_t prepare_bigbuf() {
    for (unsigned i = 0; i != 46; ++i) {
        memcpy(&bigbuf[i * 88], "I should not talk so much about myself if there were any body else whom I knew as well.\n", 88);
    }
    return 46 * 88;
}

void process_main() {
    sys_kdisplay(KDISPLAY_BUFCACHE);

    sys_map_console(console); // Map the console to the addr iin lib.hh

    while (true) {
        int f;
        char buf[256];
        small_pause();
        int wf = sys_open("thoreau.txt", OF_WRITE | OF_TRUNC);
        for (unsigned i = 0; i != 100; ++i) {
            dprintf(wf, "I should not talk so much about myself if there were any body else whom I knew as well.\n");
            if (i%25 == 24) {
                small_pause();
            }
        }
        f = sys_open("thoreau.txt", OF_READ);
        memset(buf, 'X', sizeof(buf));
        sys_read(f, buf, 9);
        small_pause();
        sys_lseek(f, 4000, LSEEK_SET);
        large_pause();
        sys_read(f, buf, 9);
        sys_lseek(f, 4090, LSEEK_SET);
        small_pause();
        sys_read(f, buf, 40);
        sys_close(f);
        small_pause();
        sys_lseek(wf, 0, LSEEK_SIZE);
        sys_close(wf);
        small_pause();
        sys_sync(2);
        large_pause();
        f = sys_open("emerson.txt", OF_WRITE);
        int f2 = sys_open("thoreau.txt", OF_WRITE);
        sys_lseek(f, 0, LSEEK_END);
        sys_lseek(f2, 0, LSEEK_END);
        ssize_t bbsz = prepare_bigbuf();

        for (int i = 0; i != 30; ++i) {
            dprintf(f, "Chick%ddee\n", i);
            sys_write(f, bigbuf, bbsz);
            if (i % 5 == 4) {
                large_pause();
            }

            dprintf(f2, "Chick%ddee\n", i);
            sys_write(f2, bigbuf, bbsz);
            if (i % 5 == 4) {
                large_pause();
            }
        }
        sys_close(f);
        sys_close(f2);
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
