#include "u-lib.hh"

// shell function for 'pwd'
void process_main(int argc, char** argv) {
    char buf[256];

    int len = sys_pwd(buf);
    if (len < 0) {
        if (len == E_FAULT) {
            console_printf(0xA00, "Error: buffer invalid (you tryna hack...?)\n");
        } else if (len == E_PERM) {
            console_printf(0xA00, "Error: string copy error\n");
        } else {
            console_printf(0xA00, "Error: an unknown error has occurred\n");
        }
    } else {
        sys_write(1, buf, len+1);
    }

    sys_exit(0);
}