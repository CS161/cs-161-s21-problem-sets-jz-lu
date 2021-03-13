#include "u-lib.hh"

void process_main() {
    sys_write(1, "About to greet you...\n", 22);

    const char* args[] = {
        "runargv", "a1", "a2", nullptr
    };
    int r = sys_execv("runargv", args);
    assert_eq(r, 0);

    sys_exit(0);
}