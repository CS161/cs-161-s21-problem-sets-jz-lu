#include "u-lib.hh"
extern uint8_t end[];

// print vfs fdtable.
void process_main(int argc, char** argv) {
    if (argv[1]) {
        console_printf(0xe00, "Warning: fdshow arguments are ignored\n");
    }
    char* buf = reinterpret_cast<char*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE) + 16 * PAGESIZE
    );
    int r = sys_page_alloc(buf);
    if (r) {
        console_printf(0xc00, "Error: out of memory\n");
        sys_exit(r);
    }

    int err = sys_fdshow(buf, PAGESIZE);
    if (err) {
        if (err == E_FAULT) {
            console_printf(0xc00, "Error: buffer invalid (you tryna hack...?)\n");
        } else if (err == E_NOSPC) {
            console_printf(0xc00, "Error: buffer invalid (you tryna hack...?)\n");
        } else {
            console_printf(0xc00, "Error: unexpected error %d has occurred\n", err);
        }
        sys_exit(1);
    }

    size_t len = strlen(buf);
    assert_le(len, PAGESIZE);
    sys_write(1, buf, len+1);
    console_printf(0xd00, "Key: (mode@type)\n");
    console_printf(0xf00, "Modes = {R: read, W: write}\n");
    console_printf(0xf00, 
        "Types = {C: console, P: pipe, M: memory file, D: disk file, S: special dev}\n");
    sys_exit(0);
}
