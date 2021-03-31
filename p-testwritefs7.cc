#include "u-lib.hh"

void process_main() {
    // this test sponsored by the "Aakash is cool" group

    printf("Starting testwritefs7 (assuming clean file system)...\n");

    // read and write to file
    printf("%s:%d: making a directory and creating new files...\n", __FILE__, __LINE__);

    int ret = sys_mkdir("/jonathan/");
    assert_eq(ret, 0);

    int f = sys_open("/jonathan/donut.txt", OF_CREATE | OF_WRITE);
    assert_gt(f, 2);

    ssize_t n = sys_write(f, "I only talk to published researchers\n", 38);
    assert_eq(n, 38);

    sys_close(f);

    console_printf(0xA00, "create directory and subfile test passed\n");

    printf("%s:%d: directory removal tests...\n", __FILE__, __LINE__);

    ret = sys_rm("/jonathan/");
    assert_lt(ret, 0);

    int r = sys_unlink("donut.txt");
    assert_eq(r, 0);

    ret = sys_rm("/jonathan/");
    assert_eq(ret, 0);

    ret = sys_rm("/aakash_can_never_be_removed/");
    assert_lt(ret, 0);

    console_printf(0xA00, "removal of directory tests passed\n");

    console_printf(0xB00, "testwritefs7 succeeded.\n");

    sys_exit(0);
}

