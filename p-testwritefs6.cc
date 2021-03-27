#include "u-lib.hh"

void process_main() {
    printf("Starting testwritefs6 (assuming clean file system)...\n");

    // read and write to file
    printf("%s:%d: renaming and reading from files...\n", __FILE__, __LINE__);

    char buf[200];
    memset(buf, 0, sizeof(buf));

    char check_buf[200];
    memset(check_buf, 0, sizeof(buf));

    int check_f = sys_open("thoreau.txt", OF_READ);
    assert_gt(check_f, 2);

    ssize_t n2 = sys_read(check_f, check_buf, 200);
    assert_eq(n2, 200);

    sys_close(check_f);

    int ret = sys_rename("thoreau.txt", "thorwho?.txt");
    assert_eq(ret, 0);

    int f = sys_open("thorwho?.txt", OF_READ);
    assert_gt(f, 2);

    ssize_t n = sys_read(f, buf, 200);
    assert_eq(n, 200);


    assert_memeq(buf, check_buf, 200);

    sys_close(f);

    console_printf(0xA00, "renamed file content test passed\n");

    memset(buf, 0, sizeof(buf));

    printf("%s:%d: lseeking and renaming in-between reads...\n", __FILE__, __LINE__);

    f = sys_open("thorwho?.txt", OF_READ);
    assert_gt(f, 2);

    memset(buf, 0, sizeof(buf));
    n = sys_read(f, buf, 39);
    assert_eq(n, 39);
    assert_memeq(buf, "The moon now rises to her absolute rule", 39);

    ret = sys_rename("thorwho?.txt", "thoreau.txt");
    assert_eq(ret, 0);

    sys_lseek(f, 0, LSEEK_SET);

    n = sys_read(f, buf, 39);
    assert_eq(n, 39);
    assert_memeq(buf, "The moon now rises to her absolute rule", 39);

    sys_close(f);

    console_printf(0xA00, "rename lseek and rename in-between test passed\n");

    printf("%s:%d: unintialized file test...\n", __FILE__, __LINE__);

    ret = sys_rename("hi.txt", "my.txt");
    assert_eq(ret, E_NOENT);

    console_printf(0xA00, "rename uninitialized file test passed\n");

    printf("%s:%d: existing filename test...\n", __FILE__, __LINE__);

    ret = sys_rename("emerson.txt", "wheatley.txt");
    assert_eq(ret, E_SAMENAME);

    console_printf(0xA00, "rename file with existing filename has passed\n");

    console_printf(0xB00, "testwritefs6 succeeded.\n");
    sys_exit(0);
}