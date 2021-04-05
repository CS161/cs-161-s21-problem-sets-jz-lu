#include "u-lib.hh"

// Run the shell command 'ls'
void process_main(int argc, char** argv) {
    char buf[256];

    int ret = sys_ls(buf, 256);
    if (ret == E_2BIG) {
    	console_printf(0xc00, "Error: print buffer too large\n");
    	sys_exit(1);
    }
    if (ret == E_FBIG) {
    	console_printf(0xe00, "Warning: too many entries to show all\n");
    }

    sys_write(1, buf, strlen(buf)+1);
    sys_exit(0);
}