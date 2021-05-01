#include "u-lib.hh"
#include <atomic>
#define VAL 10
#define NEWVAL VAL*2

extern uint8_t end[];

std::atomic_flag message_lock;
std::atomic<int> phase = 0;
int pfd[2] = {-1, -1};
uint32_t shared_int = VAL;

static void message(const char* x) {
    while (message_lock.test_and_set()) {
        pause();
    }
    console_printf("T%d (P%d): %s\n", sys_gettid(), sys_getpid(), x);
    message_lock.clear();
}



static int thfunc(void* ptr) {
    message("futex-waiting on handshake");
    int r = sys_futex(&shared_int, FUTEX_WAIT, VAL, 0);
    assert_eq(r, 0);
    assert_eq((int) shared_int, NEWVAL);
    message("thread done");
    sys_texit(0);
}



static void run_cloner() {
    // create thread
    message("cloning");
    char* stack1 = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 16 * PAGESIZE
    );
    int r = sys_page_alloc(stack1);
    assert_eq(r, 0);

    pid_t t = sys_clone(thfunc, pfd, stack1 + PAGESIZE);
    assert_gt(t, 0);
    
    message("waiting to change value");
    for (int counter = 0; counter < 10; ++counter) {
        sys_yield();
        assert_eq((int) shared_int, VAL);
    }
    message("changed value and waking");
    shared_int = NEWVAL;
    r = sys_futex(&shared_int, FUTEX_WAKE, 1, 0);
    message("thread done");
    sys_texit(0);
}

// Handshake-based test for futexes.
void process_main() {
    int p = sys_fork();
    assert_ge(p, 0);
    if (!p) { // child
        // test normal wait-on-value
        run_cloner();
    } else { // parent
        sys_waitpid(p);
    }

    // ensure that futex returns E_AGAIN if value not set before
    message("E_AGAIN test...");
    shared_int = VAL;
    int r = sys_futex(&shared_int, FUTEX_WAIT, NEWVAL, 0);
    assert_eq(r, E_AGAIN);

    // ensure that timeouts work properly
    message("Timeout test...");
    shared_int = 0;
    r = sys_futex(&shared_int, FUTEX_WAIT, 0, 1);
    assert_eq(r, E_TIMEDOUT);

    console_printf(0xA00, "testfutex succeeded.\n");
    sys_exit(0);
}
