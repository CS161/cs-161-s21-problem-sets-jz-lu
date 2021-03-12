#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char** argv) {
    uintptr_t rsp;
    asm volatile("movq %%rsp, %0" : "=r" (rsp));
    printf("%%rsp 0x%lx\n", rsp);
    printf("argc %d\n", argc);
    printf("argv %p (%%rsp+0x%lx)\n",
           argv, reinterpret_cast<uintptr_t>(argv) - rsp);
    for (int i = 0; i < argc; ++i) {
        printf("argv[%d] @%p: %p (%%rsp+0x%lx) \"%s\"\n", i, &argv[i],
               argv[i], reinterpret_cast<uintptr_t>(argv[i]) - rsp,
               argv[i]);
    }
    char* s = "hi";
    printf("%d\n", strlen(s));
}
