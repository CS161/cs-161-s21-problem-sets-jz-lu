#include "u-lib.hh"
#define NQUOTE      25
#define BUFSZ       256

void process_main(int argc, char** argv) {
    if (!argv[1] || argv[1][0] == '\n') {
        console_printf(0xc00, "Foolish mortal! ");
        console_printf(0xf00, "You dare awaken me from my slumber without a question?\n");
    } else {
        console_printf(0xB00, "Big Ka$h says: ");
        get_advice(hash(argv[1], BUFSZ, NQUOTE));
    }

    sys_exit(0);
}
