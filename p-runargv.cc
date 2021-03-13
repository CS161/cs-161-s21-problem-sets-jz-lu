#include "u-lib.hh"

void process_main(int argc, char** argv) {
    uintptr_t rsp;
    asm volatile("movq %%rsp, %0" : "=r" (rsp));
    printf("%%rsp 0x%lx\n", rsp);
    printf("argc %d\n", argc);
    printf("argv %p (%%rsp+0x%lx)\n",
           argv, reinterpret_cast<uintptr_t>(argv) - rsp);
    printf("argv[0] %p = '%s'\n", argv[0], argv[0]);
    for (int i = 0; i < argc; ++i) {
        printf("argv[%d] @%p: %p (%%rsp+0x%lx) \"%s\"\n", i, &argv[i],
               argv[i], reinterpret_cast<uintptr_t>(argv[i]) - rsp,
               argv[i]);
    }
    console_printf("If the above printed a1 and a2 as args, test succeeded\n");
}