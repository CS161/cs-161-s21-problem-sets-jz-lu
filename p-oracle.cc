#include "u-lib.hh"
#define NQUOTE      25
#define BUFSZ       256

void process_main(int argc, char** argv) {
    char buf[BUFSZ];

    for (int i = 1; i == 1 || i < argc; ++i) {
        console_printf(0xB00, "Big Ka$h: ");
        console_printf(0xd00, "Yo yo! Im Aakash-bot, the guy who finished implementing networking before everyone else even started on threading. Ask me for advice!\n");
        int f = 0;
        if (i < argc && strcmp(argv[i], "-") != 0) {
            f = sys_open(argv[i], OF_READ);
            if (f < 0) {
                dprintf(2, "%s: error %d\n", argv[i], f);
                sys_exit(1);
            }
        }
        while (true) {
            console_printf(0xf00, "Me: ");
            ssize_t n = sys_read(f, buf, sizeof(buf));
            if (n == 0 || (n < 0 && n != E_AGAIN)) {
                break;
            }
            bool keep_going = nlstrcmp(buf, "quit") && nlstrcmp(buf, "q") && nlstrcmp(buf, "exit");
            if (!keep_going) {
                console_printf(0xB00, "Big Ka$h: ");
                console_printf(0xd00, "You may be leaving now, but youll be back! Youre nothing without me!\n");
                console_printf(0xA00, "Exited (*cought* escaped *cough*) successfully\n");
                sys_exit(0);
            }
            console_printf(0xB00, "Big Ka$h: ");
            if (n > 0 && buf[0] != '\n') {
                get_advice(hash(buf, BUFSZ, NQUOTE));
            } else {
                console_printf(0xc00, "You uh...gotta ask me something bro\n");
            }
        }
        if (f != 0) {
            sys_close(f);
        }
    }

    sys_exit(0);
}
