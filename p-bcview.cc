#include "u-lib.hh"
#define ALLOC_SLOWDOWN 8

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void small_pause() {
    sys_msleep(100);
    sys_yield();
}

void large_pause() {
    sys_msleep(500);
    sys_yield();
}

void process_main() {
    sys_kdisplay(KDISPLAY_BUFCACHE);

    sys_map_console(console); // Map the console to the addr iin lib.hh

    while (true) {
        int f = sys_open("emerson.txt", OF_READ);
        char buf[200];
        memset(buf, 0, sizeof(buf));
        ssize_t n = sys_read(f, buf, 200);
        sys_close(f);
        large_pause();
        f = sys_open("emerson.txt", OF_WRITE | OF_TRUNC);
        sys_close(f);
        sys_msleep(100);
        f = sys_open("emerson.txt", OF_READ);
        n = sys_read(f, buf, 200);
        sys_close(f);
        small_pause();
        int r = sys_sync(2);
        large_pause();
        f = sys_open("emerson.txt", OF_READ);
        n = sys_read(f, buf, 200);
        sys_close(f);
        small_pause();
        f = sys_open("emerson.txt", OF_WRITE | OF_TRUNC);
        n = sys_write(f, "CLARE ROJAS WAS HERE", 20);
        sys_close(f);
        small_pause();
        f = sys_open("emerson.txt", OF_READ);
        memset(buf, 0, sizeof(buf));
        n = sys_read(f, buf, 200);
        sys_close(f);
        small_pause();
        r = sys_sync(2);
        large_pause();
        f = sys_open("emerson.txt", OF_READ);
        memset(buf, 0, sizeof(buf));
        n = sys_read(f, buf, 200);
        sys_close(f);
        small_pause();
        f = sys_open("thoreau.txt", OF_READ);
        n = sys_read(f, buf, 4);
        small_pause();
        ssize_t sz = sys_lseek(f, 0, LSEEK_SIZE);
        small_pause();
        n = sys_read(f, buf, 4);
        sz = sys_lseek(f, 0, LSEEK_SET);
        small_pause();
        n = sys_read(f, buf, 8);
        sz = sys_lseek(f, -4, LSEEK_CUR);
        small_pause();
        n = sys_read(f, buf, 4);
        sys_close(f);
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
        n = sys_read(f, buf, 9);
        small_pause();
        sz = sys_lseek(f, 4000, LSEEK_SET);
        large_pause();
        n = sys_read(f, buf, 9);
        sz = sys_lseek(f, 4090, LSEEK_SET);
        small_pause();
        n = sys_read(f, buf, 40);
        sys_close(f);
        small_pause();
        sz = sys_lseek(wf, 0, LSEEK_SIZE);
        sys_close(wf);
        small_pause();
        r = sys_sync(2);
        large_pause();
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
