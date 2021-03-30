#include "u-lib.hh"

void process_main() {
    printf("Starting testwritefs5 (assuming clean file system)...\n");

    // read and write to file
    printf("%s:%d: read and write /dev/null...\n", __FILE__, __LINE__);

    int f = sys_open("/dev/null", OF_WRITE);
    assert_gt(f, 2);

    char buf[200];
    memset(buf, 0, sizeof(buf));
    
    ssize_t n = sys_write(f, "If I could, I would eat only flavorless brown smoothies containing all the untrients needed by the human animal", 112);
    assert_eq(n, 112);  

    sys_close(f);

    f = sys_open("/dev/null", OF_READ);
    assert_gt(f, 2);

    n = sys_read(f, buf, 8);
    assert_eq(n, 1);
    assert_memeq(buf, "\0", 1);

    sys_close(f);

    console_printf(0xA00, "/dev/null test have succeeded\n");

    // read and write to file
    printf("%s:%d: read and write /dev/random...\n\n", __FILE__, __LINE__);

    f = sys_open("/dev/random", OF_WRITE);
    assert_gt(f, 2);
    
    n = sys_write(f, "A TRUE ENGLISH PANCAKE YOU ARE", 30);
    assert_eq(n, 0);  

    sys_close(f);

    memset(buf, 0, sizeof(buf));

    f = sys_open("/dev/random", OF_READ);
    assert_gt(f, 2);

    n = sys_read(f, buf, 10);
    assert_eq(n, 10);
    buf[n] = '\n';
    ++n;

    n += sys_read(f, buf + n, 10);
    assert_eq(n, 21);
    buf[n] = '\n';
    ++n;

    n += sys_read(f, buf + n, 10);
    assert_eq(n, 32);
    buf[n] = '\n';
    ++n;

    n += sys_read(f, buf + n, 10);
    assert_eq(n, 43);
    buf[n] = '\n';
    ++n;

    n += sys_read(f, buf + n, 10);
    assert_eq(n, 54);
    buf[n] = '\n';
    ++n;

    int i = 0;
    while (buf[i]) {
        // Color me random
        if (buf[i] == '\n') {
            console_printf(10*((int)buf[i]), "%c", buf[i]);
        } else {
            console_printf(100*((int)buf[i]), "%c", buf[i]);
        }

        i++;
    }
    
    console_printf("\n You should see a square of random characters above (now in color TV!)\n", buf);

    sys_close(f);

    console_printf(0xA00, "/dev/random test have succeeded\n");

    printf("testwritefs5 succeeded.\n");
    sys_exit(0);
}