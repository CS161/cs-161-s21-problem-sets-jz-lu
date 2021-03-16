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
        int fd = sys_open("hi.txt", OF_WRITE | OF_CREAT | OF_TRUNC);
        assert_ge(fd, 3);
        console_printf("[ValidUDS] [Client] Opened a file to send\n");

        int w = sys_write(fd, "I am in the kernel...I AM THE KERNEL!!! -Anonymous poet", 56);
        assert_eq(w, 56);

        int errcode3 = sys_sendfd(name, fd);
        assert_eq(errcode3, 0);

        console_printf("[ValidUDS] [Client] Client done.\n");
    } else {
        int errcode3 = sys_sendfd(name, 0);
        assert_eq(errcode3, E_BADF);
        console_printf("[InvalidUDS] [Client] Sent an invalid fd...\n");

        console_printf("[InvalidUDS] [Client] Test succeeded.\n");
    }

    sys_exit(0);
}