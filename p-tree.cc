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
    } else if (err >= E_MINERROR && err < 0) {
        console_printf(0xc00, "Error: unexpected error %d has occurred\n", err);
        sys_exit(1);
    }

    assert_le(strlen(buf), PAGESIZE);
    sys_write(1, buf, strlen(buf));
    int nfile = err >> 16, ndir = err & 0xFFFF;
    console_printf(0xf00, "%d directories, %d files\n", ndir, nfile);

    if (warn) {
        console_printf(0xe00, "Warning: tree build stopped early due to buffer overflow\n");
    }
    sys_exit(0);
}
