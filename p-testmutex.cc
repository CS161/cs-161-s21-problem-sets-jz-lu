#include "u-lib.hh"
#define NLOOP 1 // raise probability of catching race conditions
#define FACTOR 6

mutex lock;

void change_val(int val, int nullfd) {
    // writes to /dev/null avoid the increments being optimized away
    assert_eq(val%FACTOR, 0);
    for (int k = 0; k < FACTOR; ++k) {
        ++val;
        assert_eq(val%FACTOR, (k+1)%FACTOR);
        char c[2] = {(char) val%sizeof(char), '\0'};
        sys_write(nullfd, c, 2);
    }
    assert_eq(val%FACTOR, 0);
}

void spintest_invariant(int val, int nullfd) {
    lock.lock();
    change_val(val, nullfd);
    lock.unlock();
}

// Sleep before doing anything to encourage other threads to get past phase 1
// of the mutex and call sys_futex.
void sleeptest_invariant(int val, int nullfd) {
    console_printf("[%d] trying to get lock\n", sys_getpid());
    lock.lock();
    console_printf("[%d] acquired lock\n", sys_getpid());
    console_printf("[%d] about to sleep\n", sys_getpid());
    sys_msleep(100);
    change_val(val, nullfd);
    // lock.unlock();
}

// Tests mutex by trying to induce a race condition. Invariant: `val` should always be a multipl
// of 6, but we increment it one by one so that if the lock doesn't work properly, it should race.
void process_main() {
    int nullfd = sys_open("/dev/null", OF_WRITE);
    uint64_t val = 0;
    pid_t p1 = 0, p2 = 0;

    sys_write(1, "basic mutex tests...\n", 22);

    p1 = sys_fork();
    assert_ge(p1, 0);
    if (p1) {
        p2 = sys_fork();
        assert_ge(p2, 0);
    }
    if (p1 && p2) { // parent
        sys_waitpid(p1);
        sys_waitpid(p2);
    } else {
        for (int i = 0; i < NLOOP; ++i) {
            spintest_invariant(val, nullfd);
        }
        sys_exit(0);
    }
    console_printf(0xA00, "basic test succeeded.\n");
    sys_exit(0);

    sys_write(1, "encourage sleeping...\n", 23); 
    val = 0;
    p1 = p2 = 0;
    p1 = sys_fork();
    assert_ge(p1, 0);
    if (p1) {
        p2 = sys_fork();
        assert_ge(p2, 0);
    }
    if (p1 && p2) { // parent
        sys_waitpid(p1);
        sys_waitpid(p2);
    } else {
        for (int i = 0; i < NLOOP; ++i) {
            sleeptest_invariant(val, nullfd);
        }
        sys_exit(0);
    }
    console_printf(0xA00, "sleep test succeeded.\n");

    sys_write(1, "bookkeeping test...\n", 21);
    lock.unlock(); // should do nothing since its already unlocked
    {
    mutex_guard guard(lock); 
    assert_eq(guard.is_locked(), true);
    guard.unlock();
    assert_eq(guard.is_locked(), false);
    }

    console_printf(0xA00, "bookkeeping test succeeded.\n");
    sys_write(1, "Mutex guard tests...\n", 22);
    val = 0;
    p1 = p2 = 0;
    p1 = sys_fork();
    assert_ge(p1, 0);
    if (p1) {
        p2 = sys_fork();
        assert_ge(p2, 0);
    }
    if (p1 && p2) { // parent
        sys_waitpid(p1);
        sys_waitpid(p2);
    } else {
        for (int i = 0; i < NLOOP; ++i) {
            mutex_guard guard(lock);
            change_val(val, nullfd);
        }
        sys_exit(0);
    }

    console_printf(0xA00, "guard tests succeeded.\n");
    console_printf(0xB00, "testmutex succeeded.\n");
    sys_exit(0);
}