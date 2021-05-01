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
    // save function and args in callee-saved registers for child thread
    asm volatile("mov %%rdi, %%r12 \n" : : : "%r12"); // function
    asm volatile("mov %%rsi, %%r13 \n" : : : "%r13"); // arg

    // context switch into kernel, upon return 2 threads are running
    make_syscall(SYSCALL_CLONE, reinterpret_cast<uintptr_t>(function), 
        reinterpret_cast<uintptr_t>(arg), reinterpret_cast<uintptr_t>(stack_top));
        
    long r;
    asm volatile("movq %%rax, %0\n" : "=r"(r) : :);

    // nonzero return value indicates thread is parent thread
    if (r) {
        return r;
    }

    // child thread executes `function(arg)` saved earlier
    asm volatile (
        "mov %%r13, %%rdi \n"
        "call *%%r12 \n"
        : : : "%rdi"
    );
    
    // child thread calls sys_texit with return status given from return of `function(arg)`
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

void mutex::lock() {
    // phase 1: spin briefly
    for (int i = 0; i < 1; ++i) {
        int expected = 0;
        int w = word_.load(std::memory_order_release);
        console_printf("word=%d\n", w);
        if (word_.compare_exchange_weak(expected, 1)) {
            w = word_.load(std::memory_order_release);
            console_printf("grabbed lock in spin, word=%d\n", w);
            return;
        }
        sys_yield();
    }
    console_printf("phase 2\n");

    // phase 2: switch to kernel mode and sleep via futex
    int prev = word_.load(std::memory_order_relaxed);
    if (prev != 2) {
        prev = word_.exchange(2); // alert others that we are sleeping on lock
    }
    while (prev) {
        int w = word_.load(std::memory_order_release);
        console_printf("sleeping, word=%d\n", w);
        sys_futex((uint32_t*) &word_, FUTEX_WAIT, 2, 0); // no timeout
        prev = word_.exchange(2);
    }
}


void mutex::unlock() {
    int w = word_.load(std::memory_order_release);
    console_printf("FREE word=%d\n", w);
    if (--word_) { // branch executes only if there are threads sleeping on lock
        word_ = 0; // set to unlocked
        sys_futex((uint32_t*) &word_, FUTEX_WAKE, 1, 0); // no timeout
    }
}

