#include "u-lib.hh"
extern uint8_t end[];

// print file system directory tree.
void process_main(int argc, char** argv) {
    if (argv[1]) {
        console_printf(0xe00, "Warning: tree arguments are ignored\n");
    }
    char* buf = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 16 * PAGESIZE
    );
    int r = sys_page_alloc(buf);
    if (r) {
        console_printf(0xc00, "Error: out of memory\n");
        sys_exit(r);
    }

    bool warn = false;
    int err = sys_tree(buf, PAGESIZE);
    if (err == E_FAULT) {
        console_printf(0xc00, "Error: buffer invalid (you tryna hack...?)\n");
        sys_exit(1);
    } else if (err == E_NOSPC) {
        warn = true;
    } else {
        console_printf(0xc00, "Error: an unknown error has occurred\n");
        sys_exit(1);
    }

    assert_le(strlen(buf), PAGESIZE);
    sys_write(1, buf, strlen(buf));

    if (warn) {
        console_printf(0xe00, "Warning: tree build stopped early\n");
    }
    sys_exit(0);
}
