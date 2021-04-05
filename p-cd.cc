#include "u-lib.hh"

// test 'cd' function
void process_main(int argc, char** argv) {
    int ret = sys_cd(argv[1]);
    if (ret < 0) {
        if (ret == E_FAULT) {
            console_printf(0xA00, "Error: pathname invalid (you tryna hack...?)\n");
        } else if (ret == E_NAMETOOLONG) {
            console_printf(0xA00, "Error: path name too long\n");
        } else {
            console_printf(0xA00, "Error: an unknown error has occurred\n");
        }
        sys_exit(1);
    }

    sys_exit(0);
}