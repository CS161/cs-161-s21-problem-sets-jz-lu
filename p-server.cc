// This is the UDS server process.
#include "u-lib.hh"

void process_main(int argc, char** argv) {
    const char* name = argv[1];
    bool is_good = (strcmp(argv[2], "good") == 0);
    console_printf("[Server] Currently running the %s fd case\n", is_good? "valid" : "invalid");

    if (is_good) {
        console_printf("[ValidUDS] [Server] Setting up socket...\n");
    } else {
        console_printf("[InvalidUDS] [Server] Setting up socket...\n");
    }
    int errcode1 = sys_socket(name);
    assert_eq(errcode1, 0);

    int errcode2 = sys_listen(name);
    assert_eq(errcode2, 0);

    int errcode10 = sys_accept(name);
    assert_eq(errcode10, 0);
    sys_msleep(50);

    if (is_good) {
        console_printf("[ValidUDS] [Server] Now accepting!\n");
        int fd = sys_receivefd(name);
        assert_ge(fd, 0);
        char buf[200];
        int r = sys_read(fd, buf, 100);
        assert_eq(r, 57);
        assert_memeq(buf, "I am in the kernel...I AM THE KERNEL!!! -Anonymous poet\n", 57);
        int w = sys_write(1, buf, 57);
        assert_eq(w, 57);
        console_printf(0x8A00, "[ValidUDS] [Server] Test succeeeded.\n");
    } else {
        console_printf("[InvalidUDS] [Server] Now accepting!\n");
        int fd = sys_receivefd(name);
        assert_eq(fd, E_SOCKTIMEOUT);
        console_printf(0x8A00, "[InvalidUDS] [Server] Timed out (as expected). Test succeeded.\n");
    }

    sys_exit(0);
}