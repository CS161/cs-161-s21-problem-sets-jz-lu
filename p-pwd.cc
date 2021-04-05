#include "u-lib.hh"

// shell function for 'pwd'
void process_main(int argc, char** argv) {
    char buf[256];
    if (argv[1]) {
        console_printf(0xe00, "Warning: pwd arguments are ignored\n");
    }

    int len = sys_pwd(buf);
    if (len < 0) {
        if (len == E_FAULT) {
            console_printf(0xc00, "Error: buffer invalid (you tryna hack...?)\n");
        } else if (len == E_PERM) {
            console_printf(0xc00, "Error: string copy error\n");
        } else {
            console_printf(0xc00, "Error: an unknown error has occurred\n");
        }
        sys_exit(1);
    } else {
        assert_le(len, 255);
        buf[len+1] = '\0';
        buf[len] = '\n';
        sys_write(1, buf, len+1);
    }

    sys_exit(0);
}