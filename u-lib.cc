#include "u-lib.hh"

// dprintf
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.

int dprintf(int fd, const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(fd, buf, n);
    } else {
        return E_2BIG;
    }
}


// printf
//    Like `printf(1, ...)`.

int printf(const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(1, buf, n);
    } else {
        return E_2BIG;
    }
}


// panic, assert_fail
//     Call the SYSCALL_PANIC system call so the kernel loops until Control-C.

void panic(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buf[160];
    memcpy(buf, "PANIC: ", 7);
    int len = vsnprintf(&buf[7], sizeof(buf) - 7, format, val) + 7;
    va_end(val);
    if (len > 0 && buf[len - 1] != '\n') {
        strcpy(buf + len - (len == (int) sizeof(buf) - 1), "\n");
    }
    (void) console_printf(CPOS(23, 0), 0xC000, "%s", buf);
    sys_panic(nullptr);
}

int error_vprintf(int cpos, int color, const char* format, va_list val) {
    return console_vprintf(cpos, color, format, val);
}

void assert_fail(const char* file, int line, const char* msg,
                 const char* description) {
    cursorpos = CPOS(23, 0);
    if (description) {
        error_printf("%s:%d: %s\n", file, line, description);
    }
    error_printf("%s:%d: user assertion '%s' failed\n", file, line, msg);
    sys_panic(nullptr);
}


// sys_clone
//    Create a new thread.

pid_t sys_clone(int (*function)(void*), void* arg, char* stack_top) {
    asm volatile("mov %%rdi, %%r12 \n" : : : "%r12"); // function
    asm volatile("mov %%rsi, %%r13 \n" : : : "%r13"); // arg

    make_syscall(SYSCALL_CLONE, reinterpret_cast<uintptr_t>(function), 
        reinterpret_cast<uintptr_t>(arg), reinterpret_cast<uintptr_t>(stack_top));
        
    long r;
    asm volatile("movq %%rax, %0\n" : "=r"(r) : :);

    if (r) {
        return r;
    }

    asm volatile (
        "mov %%r13, %%rdi \n"
        "call *%%r12 \n"
        : : : "%rdi"
    );
    
    asm volatile(
        "mov %%rax, %%rdi\n"
        : : : "%rdi" // put return value in args
    );
    register uintptr_t rax asm("rax") = SYSCALL_TEXIT;
    asm volatile ("syscall" // call sys_texit
            : "+a" (rax)
            : /* all input registers are also output registers */
            : "cc", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11");
    return rax; // will never be reached
}

