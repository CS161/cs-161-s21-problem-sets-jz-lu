#include "u-lib.hh"

// shell 'rm' function
void process_main(int argc, char** argv) {
    if (!argv[1]) {
        console_printf(0xc00, "Error: no argument specified\n");
        sys_exit(1);
    }
    
    char* name = argv[1];
    if (name[strlen(name)-1] == '/') {
        // Case 1: remove a directory.
        int r = sys_rm(name);
        if (r < 0) {
            if (r == E_FAULT) {
                console_printf(0xc00, "Error: pathname invalid (you tryna hack...?)\n");
            } else if (r == E_NAMETOOLONG) {
                console_printf(0xc00, "Error: pathname is too long\n");
            } else if (r == E_PERM) {
                console_printf(0xc00, "Error: cannot remove root directory\n");
            } else if (r == E_NOENT) {
                console_printf(0xc00, "Error: no such directory, do not end with '/' if removing file\n");
            } else if (r == E_NONEMPTY) {
                console_printf(0xc00, "Error: cannot remove nonempty directory\n");
            } else if (r == E_GETOUT) {
                console_printf(0xc00, "Error: cannot remove a directory containing wd, cd out and try again\n");
            } else {
                console_printf(0xc00, "Error: an unknown I/O error occurred\n");
            }
            sys_exit(1);
        }
    } else {
        // Case 2: remove a file.
        int r = sys_unlink(name);
        if (r < 0) {
            if (r == E_FAULT) {
                console_printf(0xc00, "Error: pathname invalid (you tryna hack...?)\n");
            } else if (r == E_NOENT) {
                console_printf(0xc00, "Error: no such file, end with '/' if removing directory\n");
            } else if (r == E_INVAL) {
                console_printf(0xc00, "Error: name is a directory, end path with '/' to remove\n");
            } else {
                console_printf(0xc00, "Error: an unknown I/O error occurred\n");
            }
            sys_exit(1);
        }
    }

    sys_exit(0);
}