#include "u-lib.hh"

void process_main(int argc, char** argv) {
    int i = 1;
    console_printf("argv=%p, argv[0]=%p\n", argv, argv[0]);
    while (argv[i]) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        sys_write(1, argv[i], strlen(argv[i]));
        ++i;
    }
    sys_write(1, "\n", 1);
    assert(i == argc);

    sys_exit(0);
}
