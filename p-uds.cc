// Test cases for UDS
#include "u-lib.hh"

void process_main() {
    sys_write(1, "Running UDS test 1 (valid fd passing)\n", 39);
    int p1 = sys_fork();
    int p2 = -1;
    if (p1) {
        p2 = sys_fork();
    }

    if (p1 && p2) { // Process 1: test 1 server
        const char* args[] = {
            "server", "sock1", "good", nullptr
        };
        int r = sys_execv("server", args);
        assert_eq(r, 0);
        sys_exit(0);
    } else if (!p1 && p2) { // Process 2: test 1 client
        sys_msleep(10); // Give server some time to write
        const char* args[] = {
            "client", "sock1", "good", nullptr
        };
        int r = sys_execv("client", args);
        assert_eq(r, 0);
        sys_exit(0);
    } else { // Process 3
        sys_msleep(400); // Let the first test finish
        sys_write(1, "Running UDS test 2 (invalid fd passing)\n", 41);
        int p3 = sys_fork();
        if (p3) { // Process 3: test 2 server
            const char* args[] = {
                "server", "sock2", "bad", nullptr
            };
            int r = sys_execv("server", args);
            assert_eq(r, 0);
            sys_exit(0);
        } else { // Process 4: test 2 client
            sys_msleep(10); // Give server some time to write
            const char* args[] = {
                "client", "sock2", "bad", nullptr
            };
            int r = sys_execv("client", args);
            assert_eq(r, 0);
            sys_exit(0);
        }
    }
}