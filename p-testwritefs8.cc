#include "u-lib.hh"

// Test ftruncate by slicing and enlarging donuts :-D
void process_main() {
    printf("Starting testwritefs8 (assuming clean file system)...\n");

    // read and write to file
    printf("%s:%d: creating donut.txt ...\n", __FILE__, __LINE__);

    int f = sys_open("donut.txt", OF_CREATE | OF_WRITE);
    assert_gt(f, 2);

    printf("%s:%d: writing to donut.txt ...\n", __FILE__, __LINE__);

    ssize_t n = sys_write(f, "I only talk to published researchers\n", 38);
    // This message NOT endorsed by the "Jonathan is cool" group
    assert_eq(n, 38);

    sys_close(f);

    f = sys_open("donut.txt", OF_READ);
    assert_gt(f, 2);

    printf("%s:%d: using ftruncate to cut our donut ...\n", __FILE__, __LINE__);
    int ret = sys_ftruncate(f, 11);
    assert_gt(ret, 0);

    char buf[200];
    memset(buf, 0, sizeof(buf));

    printf("%s:%d: examining the cut donut ...\n", __FILE__, __LINE__);

    n = sys_read(f, buf, 200);
    assert_eq(n, 11);
    assert_memeq(buf, "I only talk", 11);


    printf("%s:%d: making the donut bigger ...\n", __FILE__, __LINE__);

    memset(buf, 0, sizeof(buf));
    ret = sys_ftruncate(f, 200);
    assert_gt(ret, 0);

    printf("%s:%d: examining the bigger donut ...\n", __FILE__, __LINE__);
    ret = sys_lseek(f, 0, LSEEK_SET);

    n = sys_read(f, buf, 200);
    assert_eq(n, 200);

    console_printf(0xA00, "testwritefs8 succeeded.\n");

    sys_exit(0);
}