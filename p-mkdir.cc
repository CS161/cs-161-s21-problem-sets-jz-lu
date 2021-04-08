#include "u-lib.hh"

// shell 'mkdir' function
void process_main(int argc, char** argv) {
    if (!argv[1]) {
        console_printf(0xc00, "Error: no argument specified\n");
        sys_exit(1);
    }

    int r = sys_mkdir(argv[1]);
    if (r) {
        if (r == E_FAULT) {
            console_printf(0xc00, "Error: pathname invalid (you tryna hack...?)\n");
        } else if (r == E_SAMENAME) {
            console_printf(0xc00, "Error: name taken\n");
        } else if (r == E_NOSPC) {
            console_printf(0xc00, "Error: buffer full\n");
        } else if (r == E_NOENT) {
            console_printf(0xc00, "Error: kernel failed to execute request\n");
        } else {
            console_printf(0xc00, "Error: an unknown error occurred\n");
        }
        sys_exit(1);
    }

    sys_exit(0);
}