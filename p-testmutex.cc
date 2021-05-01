#include "u-lib.hh"
#define NSPINLOOP 100000 // raise probability of catching race conditions
#define NSLEEPLOOP 500
#define FACTOR 6

extern uint8_t end[];
mutex lock;
uint32_t shared_val = 0;
int nullfd;
std::atomic_flag message_lock;

static void message(const char* x) {
    while (message_lock.test_and_set()) {
        pause();
    }
    console_printf("T%d (P%d): %s\n", sys_gettid(), sys_getpid(), x);
    message_lock.clear();
}

static void change_val(void* fdptr) {
    // writes to /dev/null avoid the increments being optimized away
    assert_eq((int) shared_val%FACTOR, 0);
    for (int k = 0; k < FACTOR; ++k) {
        ++shared_val;
        assert_eq((int) shared_val%FACTOR, (k+1)%FACTOR);
        char c[2] = {(char) shared_val%sizeof(char), '\0'};
        sys_write(nullfd, c, 2);
    }
    assert_eq((int) shared_val%FACTOR, 0);
}

static int spintest_invariant(void* fdptr) {
    message("detecting races...(this will take a few seconds)");
    for (int i = 0; i < NSPINLOOP; ++i) {
        lock.lock();
        change_val(fdptr);
        lock.unlock();
    }
    message("thread done");
    return 0;
}

// Sleep before doing anything to encourage other threads to get past phase 1
// of the mutex and call sys_futex.
static int sleeptest_invariant(void* fdptr) {
    message("detecting races...(this will take several seconds)");
    for (int i = 0; i < NSLEEPLOOP; ++i) {
        lock.lock();
        change_val(fdptr);
        sys_msleep(1);
        lock.unlock();
    }
    message("thread done, no races found");
    return 0;
}


static int guardtest_invariant(void* fdptr) {
    message("detecting races...(this will take a few seconds)");
    for (int i = 0; i < NSPINLOOP; ++i) {
        mutex_guard guard(lock);
        change_val(fdptr);
    }
    message("thread done, no races found");
    return 0;
}


static void test1() {
    // create thread
    message("(1) cloning");
    char* stack1 = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 16 * PAGESIZE
    );
    int r = sys_page_alloc(stack1);
    assert_eq(r, 0);

    pid_t t = sys_clone(spintest_invariant, &nullfd, stack1 + PAGESIZE); // we don't need an arg here
    assert_gt(t, 0);
    spintest_invariant(&nullfd);
    
    sys_texit(0);
}


static void test2() {
    // create thread
    message("(2) cloning");
    char* stack2 = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 17 * PAGESIZE
    );
    int r = sys_page_alloc(stack2);
    assert_eq(r, 0);

    pid_t t = sys_clone(sleeptest_invariant, &nullfd, stack2 + PAGESIZE); // we don't need an arg here
    assert_gt(t, 0);
    sleeptest_invariant(&nullfd);
    sys_texit(0);
}


static void test3() {
    // create thread
    message("(3) cloning");
    char* stack3 = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 18 * PAGESIZE
    );
    int r = sys_page_alloc(stack3);
    assert_eq(r, 0);

    pid_t t = sys_clone(guardtest_invariant, &nullfd, stack3 + PAGESIZE); // we don't need an arg here
    assert_gt(t, 0);
    guardtest_invariant(&nullfd);
    sys_texit(0);
}


// Tests mutex by trying to induce a race condition. Invariant: `val` should always be a multiple
// of `FACTOR`, but we increment it one by one so that if the lock doesn't work properly the invariant
// will be violated as the threads will race to increment. Loop many times to make probability
// of detecting a race very high.
void process_main() {
    nullfd = sys_open("/dev/null", OF_WRITE);

    sys_write(1, "basic mutex tests...\n", 22);

    int p = sys_fork();
    assert_ge(p, 0);
    if (!p) { // child
        test1();
    } else { // parent
        p = sys_waitpid(p);
        assert_ge(p, 0);
    }
    console_printf(0xA00, "basic test succeeded.\n");

    sys_write(1, "encourage mutex to pass spin phase and sleep...\n", 49); 
    p = sys_fork();
    assert_ge(p, 0);
    if (!p) { // child
        test2();
    } else { // parent
        p = sys_waitpid(p);
        assert_ge(p, 0);
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

    sys_write(1, "mutex guard struct tests...\n", 29);
    p = sys_fork();
    assert_ge(p, 0);
    if (!p) { // child
        test3();
    } else { // parent
        p = sys_waitpid(p);
        assert_ge(p, 0);
    }
    
    console_printf(0xA00, "guard tests succeeded.\n");
    console_printf(0xB00, "testmutex succeeded.\n");
    sys_exit(0);
}