#include "u-lib.hh"

void process_main() {
    sys_write(1, "Testing execv argument passing...\n", 35);

    const char* args[] = {
        "runargv", "a1", "a2", "a3", "a4", "a5", "a6", nullptr
    };
    int r = sys_execv("runargv", args);
    assert_eq(r, 0);

    sys_exit(0);
}