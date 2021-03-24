// This is the UDS client process.
#include "u-lib.hh"

void process_main(int argc, char** argv) {
    const char* name = argv[1];
    bool is_good = (strcmp(argv[2], "good") == 0);
    console_printf("[Client] Currently running the %s fd case\n", is_good? "valid" : "invalid");

    int errcode1 = sys_connect(name);
    assert_eq(errcode1, 0);
    if (is_good) {
        console_printf("[ValidUDS] [Client] Connected to socket\n");
    } else {
        console_printf("[InvalidUDS] [Client] Connected to socket\n");
    }

    if (is_good) {
        int pfd[2];
        sys_pipe(pfd);
        assert_eq(pfd[0], 3);
        assert_eq(pfd[1], 4);
        console_printf("[ValidUDS] [Client] Opened a pipe, sending server the read end\n");

        int w = sys_write(pfd[1], "I am in the kernel...I AM THE KERNEL!!! -Anonymous poet\n", 57);
        assert_eq(w, 57);

        int errcode3 = sys_sendfd(name, pfd[0]);
        assert_eq(errcode3, 0);

        console_printf("[ValidUDS] [Client] Client done.\n");
        sys_msleep(100);
    } else {
        int errcode3 = sys_sendfd(name, 3);
        assert_eq(errcode3, E_BADF);
        console_printf("[InvalidUDS] [Client] Sent an invalid fd...\n");

        console_printf(0xA00, "[InvalidUDS] [Client] Test succeeded.\n");
    }

    sys_exit(0);
}