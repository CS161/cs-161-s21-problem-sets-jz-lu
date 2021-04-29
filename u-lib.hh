#ifndef CHICKADEE_U_LIB_HH
#define CHICKADEE_U_LIB_HH
#include "lib.hh"
#include "x86-64.h"
#include <atomic>
#if CHICKADEE_KERNEL
#error "u-lib.hh should not be used by kernel code."
#endif

// u-lib.hh
//
//    Support code for Chickadee user-level code.


// make_syscall, access_memory, clobber_memory
//    These functions define the Chickadee system call calling convention.

__always_inline uintptr_t make_syscall(int syscallno) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax)
            : /* all input registers are also output registers */
            : "cc", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0)
            :
            : "cc", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1)
            :
            : "cc", "rcx", "rdx", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1, uintptr_t arg2) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2)
            :
            : "cc", "rcx", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1, uintptr_t arg2,
                                       uintptr_t arg3) {
    register uintptr_t rax asm("rax") = syscallno;
    register uintptr_t r10 asm("r10") = arg3;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2), "+r" (r10)
            :
            : "cc", "rcx", "r8", "r9", "r11");
    return rax;
}

__always_inline void clobber_memory(void* ptr) {
    asm volatile ("" : "+m" (*(char*) ptr));
}

__always_inline void access_memory(const void* ptr) {
    asm volatile ("" : : "m" (*(const char*) ptr));
}


// sys_getpid
//    Return current process ID.
inline pid_t sys_getpid() {
    return make_syscall(SYSCALL_GETPID);
}

// sys_map_console
//    Maps the console at a user-specified virtual address.
inline int sys_map_console(void* addr) {
    return make_syscall(SYSCALL_MAP_CONSOLE, reinterpret_cast<uintptr_t>(addr));
}

// sys_yield
//    Yield control of the CPU to the kernel. The kernel will pick another
//    process to run, if possible.
inline void sys_yield() {
    make_syscall(SYSCALL_YIELD);
}

// sys_pause()
//    A version of `sys_yield` that spins in the kernel long enough
//    for kernel timer interrupts to occur.
inline void sys_pause() {
    make_syscall(SYSCALL_PAUSE);
}

inline void sys_nasty(long sz) {
    make_syscall(SYSCALL_NASTY, sz);
}

// sys_testkalloc(tcase)
//    Runs a test case for buddy allocator.
inline int sys_testkalloc(long tcase) {
    return make_syscall(SYSCALL_TESTKALLOC, tcase);
}

// sys_wildalloc(tcase)
//    Wild allocation for buddy allocator.
inline int sys_wildkalloc(long tcase) {
    return make_syscall(SYSCALL_WILDALLOC, tcase);
}


// sys_kdisplay(display_type)
//    Set the display type (one of the KDISPLAY constants).
inline int sys_kdisplay(int display_type) {
    return make_syscall(SYSCALL_KDISPLAY, display_type);
}

// sys_panic(msg)
//    Panic.
[[noreturn]] inline void sys_panic(const char* msg) {
    make_syscall(SYSCALL_PANIC, reinterpret_cast<uintptr_t>(msg));
    while (true) {
    }
}

// sys_page_alloc(addr)
//    Allocate a page of memory at address `addr`. `Addr` must be page-aligned
//    (i.e., a multiple of PAGESIZE == 4096). Return 0 on success, E_NOMEM on
//    out of memory, and E_INVAL on invalid `addr`.
inline int sys_page_alloc(void* addr) {
    return make_syscall(SYSCALL_PAGE_ALLOC, reinterpret_cast<uintptr_t>(addr));
}

// sys_varalloc(addr, sz)
//    Allocate sz bytes of memory at address `addr`. `Addr` must be page-aligned
//    (i.e., a multiple of PAGESIZE == 4096). Return 0 on success, E_NOMEM on
//    out of memory, and E_INVAL on invalid `addr`.
inline int sys_varalloc(void* va, uint64_t sz) {
    return make_syscall(SYSCALL_VARALLOC, reinterpret_cast<uintptr_t>(va), sz);
}

// sys_free(addr)
//    Free memory under buddy allocation system.
inline int sys_free(void* va) {
    return make_syscall(SYSCALL_FREE, reinterpret_cast<uintptr_t>(va));
}

// sys_fork()
//    Fork the current process. On success, return the child's process ID to
//    the parent, and return 0 to the child. On failure, return E_NOMEM on out
//    of memory, E_AGAIN if the process table is full.
inline pid_t sys_fork() {
    return make_syscall(SYSCALL_FORK);
}

// sys_exit(status)
//    Exit this process. Does not return.
[[noreturn]] inline void sys_exit(int status) {
    make_syscall(SYSCALL_EXIT, status);
    assert(false);
}

// sys_msleep(msec)
//    Block for approximately `msec` milliseconds.
inline int sys_msleep(unsigned msec) {
    return make_syscall(SYSCALL_MSLEEP, msec);
}

// sys_getppid()
//    Return parent process ID.
inline pid_t sys_getppid() {
    return make_syscall(SYSCALL_GETPPID);
}

// sys_waitpid(pid, status, options)
//    Wait until process `pid` exits and report its status. The status
//    is stored in `*status`, if `status != nullptr`. If `pid == 0`,
//    waits for any child. If `options == W_NOHANG`, returns immediately.
inline pid_t sys_waitpid(pid_t pid, int* status = nullptr, int options = 0) {
    uint64_t retpid = make_syscall(SYSCALL_WAITPID, pid, 
        reinterpret_cast<uintptr_t>(status), options);
    // Unpack the retpid into the ret status (left 32 bits) and pid (right 32 bits).
    if (status) {
        *status = retpid >> 32;
    }
    return retpid & 0xFFFFFFFF;
}

// sys_read(fd, buf, sz)
//    Read bytes from `fd` into `buf`. Read at most `sz` bytes. Return
//    the number of bytes read, which is 0 at EOF.
inline ssize_t sys_read(int fd, char* buf, size_t sz) {
    clobber_memory(buf);
    return make_syscall(SYSCALL_READ, fd,
                        reinterpret_cast<uintptr_t>(buf), sz);
}

// sys_write(fd, buf, sz)
//    Write bytes to `fd` from `buf`. Write at most `sz` bytes. Return
//    the number of bytes written.
inline ssize_t sys_write(int fd, const char* buf, size_t sz) {
    access_memory(buf);
    return make_syscall(SYSCALL_WRITE, fd,
                        reinterpret_cast<uintptr_t>(buf), sz);
}

// sys_dup2(oldfd, newfd)
//    Make `newfd` a reference to the same file structure as `oldfd`.
inline int sys_dup2(int oldfd, int newfd) {
    return make_syscall(SYSCALL_DUP2, oldfd, newfd);
}

// sys_close(fd)
//    Close `fd`.
inline int sys_close(int fd) {
    return make_syscall(SYSCALL_CLOSE, fd);
}

// sys_open(path, flags)
//    Open a new file descriptor for pathname `path`. `flags` should
//    contain at least one of `OF_READ` and `OF_WRITE`.
inline int sys_open(const char* path, int flags) {
    access_memory(path);
    return make_syscall(SYSCALL_OPEN, reinterpret_cast<uintptr_t>(path),
                        flags);
}

// sys_pipe(pfd)
//    Create a pipe. The array is set to [readfd | writefd].
inline int sys_pipe(int pfd[2]) {
    uintptr_t r = make_syscall(SYSCALL_PIPE);
    if (!is_error(r)) {
        pfd[0] = r;
        pfd[1] = r >> 32;
        r = 0;
    }
    return r;
}

// sys_execv(program_name, argv, argc)
//    Replace this process image with a new image running `program_name`
//    with `argc` arguments, stored in argument array `argv`. Returns
//    only on failure.
inline int sys_execv(const char* program_name, const char* const* argv,
                     size_t argc) {
    access_memory(program_name);
    access_memory(argv);
    return make_syscall(SYSCALL_EXECV,
                        reinterpret_cast<uintptr_t>(program_name),
                        reinterpret_cast<uintptr_t>(argv), argc);
}

// sys_execv(program_name, argv)
//    Replace this process image with a new image running `program_name`
//    with arguments `argv`. `argv` is a null-terminated array. Returns
//    only on failure.
inline int sys_execv(const char* program_name, const char* const* argv) {
    size_t argc = 0;
    while (argv && argv[argc] != nullptr) {
        ++argc;
    }
    return sys_execv(program_name, argv, argc);
}

// sys_socket(sockname)
//    Open a Unix Domain Socket with the user process as the server.
inline int sys_socket(const char* sockname) {
    return make_syscall(SYSCALL_SOCKET, reinterpret_cast<uintptr_t>(sockname));
}

// sys_listen(sockname)
//    Server marks UDS as listening.
inline int sys_listen(const char* sockname) {
    return make_syscall(SYSCALL_LISTEN, reinterpret_cast<uintptr_t>(sockname));
}

// sys_accept(sockname)
//    Server marks UDS as accepting.
inline int sys_accept(const char* sockname) {
    return make_syscall(SYSCALL_ACCEPT, reinterpret_cast<uintptr_t>(sockname));
}

// sys_connect(sockname)
//    Establish client connection to an open socket.
inline int sys_connect(const char* sockname) {
    return make_syscall(SYSCALL_CONNECT, reinterpret_cast<uintptr_t>(sockname));
}

// sys_disconnect(sockname)
//    Close connection to a socket, for both a client and server to call.
inline int sys_disconnect(const char* sockname) {
    return make_syscall(SYSCALL_DISCONNECT, reinterpret_cast<uintptr_t>(sockname));
}

// sys_sendfd(sockname, fd)
//    Client sends a file descriptor to a socket.
inline int sys_sendfd(const char* sockname, int fd) {
    return make_syscall(SYSCALL_SENDFD, 
                        reinterpret_cast<uintptr_t>(sockname), fd);
}

// sys_receivefd(sockname)
//    Server reads a file descriptor from a socket and opens it.
//    Returns the file descriptor to the file opened on the server proc::fdtable.
inline int sys_receivefd(const char* sockname) {
    return make_syscall(SYSCALL_RECEIVEFD, 
                        reinterpret_cast<uintptr_t>(sockname));
}

// sys_unlink(pathname)
//    Remove the file named `pathname`.
inline int sys_unlink(const char* pathname) {
    access_memory(pathname);
    return make_syscall(SYSCALL_UNLINK, reinterpret_cast<uintptr_t>(pathname));
}

// sys_readdiskfile(pathname, buf, sz, off)
//    Read bytes from disk file `pathname` into `buf`. Read at most `sz`
//    bytes starting at file offset `off`. Return the number of bytes
//    read, which is 0 at EOF.
inline ssize_t sys_readdiskfile(const char* pathname,
                                char* buf, size_t sz, off_t off) {
    access_memory(pathname);
    clobber_memory(buf);
    return make_syscall(SYSCALL_READDISKFILE,
                        reinterpret_cast<uintptr_t>(pathname),
                        reinterpret_cast<uintptr_t>(buf), sz, off);
}

// sys_sync(drop)
//    Synchronize all modified buffer cache contents to disk.
//    If `drop == 1`, then additionally clear the buffer cache so that
//    future reads start from an empty cache.
//    If `drop == 2`, then assert that the process has no open files and
//    that no data blocks are referenced.
inline int sys_sync(int drop = 0) {
    return make_syscall(SYSCALL_SYNC, drop);
}

// sys_lseek(fd, offset, origin)
//    Set the current file position for `fd` to `off`, relative to
//    `origin` (one of the `LSEEK_` constants). Returns the new file
//    position (or, for `LSEEK_SIZE`, the file size).
inline ssize_t sys_lseek(int fd, off_t off, int origin) {
    return make_syscall(SYSCALL_LSEEK, fd, off, origin);
}

// sys_ftruncate(fd, len)
//    Set the size of file `fd` to `len`. If the file was previously
//    larger, the extra data is lost; if it was shorter, it is extended
//    with zero bytes. Returns new size upon success, or error code.
inline int sys_ftruncate(int fd, off_t len) {
    return make_syscall(SYSCALL_FTRUNCATE, fd, len);
}

// sys_ls(buf)
//    List everything inside the current directory.
inline int sys_ls(const char* buf, size_t sz) {
    return make_syscall(SYSCALL_LS, reinterpret_cast<uintptr_t>(buf), sz);
}

// sys_rename(oldpath, newpath)
//    Rename the file with name `oldpath` to `newpath`.
inline int sys_rename(const char* oldpath, const char* newpath) {
    access_memory(oldpath);
    access_memory(newpath);
    return make_syscall(SYSCALL_RENAME, reinterpret_cast<uintptr_t>(oldpath),
                        reinterpret_cast<uintptr_t>(newpath));
}

// sys_mkdir(path)
//    Make a new subdirectory along `path`
inline int sys_mkdir(const char* path) {
    access_memory(path);
    return make_syscall(SYSCALL_MKDIR, reinterpret_cast<uintptr_t>(path));
}

// sys_rm(path)
//    Remove a blank subdirectory along `path`
inline int sys_rm(const char* path) {
    access_memory(path);
    return make_syscall(SYSCALL_RM, reinterpret_cast<uintptr_t>(path));
}

// sys_pwd(buf)
//    Reads current working directory into `buf`.
inline int sys_pwd(const char* buf) {
    access_memory(buf);
    return make_syscall(SYSCALL_PWD, reinterpret_cast<uintptr_t>(buf));
}

// sys_tree(buf)
//    Stores a file system tree representation in buf.
//    Returns a concatenated nfile, ndir in one int.
inline int sys_tree(const char* buf, size_t bufsz) {
    access_memory(buf);
    return make_syscall(SYSCALL_TREE, reinterpret_cast<uintptr_t>(buf), bufsz);
}

// sys_fdshow(buf)
//    Stores a VFS table representation in buf.
inline int sys_fdshow(const char* buf, size_t bufsz) {
    access_memory(buf);
    return make_syscall(SYSCALL_FDSHOW, reinterpret_cast<uintptr_t>(buf), bufsz);
}

// sys_cd(path)
//    Changes current working directory to `path`.
inline int sys_cd(const char* path) {
    access_memory(path);
    return make_syscall(SYSCALL_CD, reinterpret_cast<uintptr_t>(path));
}

// sys_gettid()
//    Return the current thread ID.
inline pid_t sys_gettid() {
    return make_syscall(SYSCALL_GETTID);
}

// sys_clone(function, arg, stack_top)
//    Create a new thread running `function` with `arg`, starting at
//    stack address `stack_top`. Returns the new thread's thread ID.
//
//    In the context of the new thread, when the `function` returns,
//    the thread should call `sys_texit` with the function's return value
//    as argument.
pid_t sys_clone(int (*function)(void*), void* arg, char* stack_top);

// sys_texit(status)
//    Exit the current thread with exit status `status`. If this is
//    the last thread in the process, this will have the same effect
//    as `sys_exit(status)`.
[[noreturn]] inline void sys_texit(int status) {
    make_syscall(SYSCALL_TEXIT, status);
    assert(false);
}


// dprintf(fd, format, ...)
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.
int dprintf(int fd, const char* format, ...);

// printf(format, ...)
//    Like `dprintf(1, format, ...)`.
int printf(const char* format, ...);

// ¯\_(ツ)_/¯
inline void get_advice(int n) {
    switch (n) {
        case 0:
            console_printf(0xd00, "Wait, Anaconda isnt an Eminem song?\n");
            break;

        case 1:
            console_printf(0xd00, "There are more dollar signs in my LaTeX file than in my income.\n");
            break;
        
        case 2:
            console_printf(0xd00, "I mean, my mind is like a treasure\n");
            break;
        
        case 3:
            console_printf(0xd00, "Yeah I dont use social media I just get validation through hearts on Ed.\n");
            break;
        
        case 4:
            console_printf(0xd00, "PEOPLE FALL IN LOVE IN MYSTERIOUS WAYYYYSS, MAYBE ITS ALL PART OF A PLANNNNNNNNNN\n");
            break;
        
        case 5:
            console_printf(0xd00, "You know how when you put your finger in and you expect it to be firm but then it’s all soft and squishy and you’re just like *ugh*…\n");
            break;
        
        case 6:
            console_printf(0xd00, "I just want you to know...that I have 100%% credibility! At all times!\n");
            break;
        
        case 7:
            console_printf(0xd00, "My doctor thinks Im an anti-vaxxer.\n");
            break;
        
        case 8:
            console_printf(0xd00, "No, YOURE a category error!\n");
            break;

        case 9:
            console_printf(0xd00, "Sometimes my genius even manages to amaze me.\n");
            break;
        
        case 10:
            console_printf(0xd00, "Oh you said sick? I thought you said thicc and I was like yeahhhh\n");
            break;
        
        case 11:
            console_printf(0xd00, "Getting this stack usage flag to notice me is harder than getting a girl's attention\n");
            break;
        
        case 12:
            console_printf(0xd00, "If I were a superhero, my name would be P-A-C man!...wait thats pac man.\n");
            break;
        
        case 13:
            console_printf(0xd00, "*Humming softly* kill em with sadness, kill em with pity\n");
            break;
        
        case 14:
            console_printf(0xd00, "Its not really networking if most of my network is family right\n");
            break;
        
        case 15:
            console_printf(0xd00, "I pity those in finals clubs, but maybe its just my extreme antisocial abilities\n");
            break;
        
        case 16:
            console_printf(0xd00, "I have never been a nerd. I am ultimate non-nerd.\n");
            break;
        
        case 17:
            console_printf(0xd00, "My next meeting? My next meeting is with FOOD...when Im HUNGRY\n");
            break;
        
        case 18:
            console_printf(0xd00, "I dont talk like you! I talk more like whats hangin bro yeee yeee\n");
            break;

        case 19:
            console_printf(0xd00, "This is just...the perfect banana. I feel kind of like a monkey right now.\n");
            break;

        case 20:
            console_printf(0xd00, "Criticizing me?? Everyone should be apologizing to me!\n");
            break;

        case 21:
            console_printf(0xd00, "My grandmother said it seems that my genes should have been killed off by natural selection.\n");
            break;
        
        case 22:
            console_printf(0xd00, "No, YOURE a race condition!\n");
            break;
        
        case 23:
            console_printf(0xd00, "I can’t even spell eigenvector dude, lower the bar\n");
            break;
        
        case 24:
            console_printf(0xd00, "*demonic screeching*\n");
            break;
        
        default:
            break;
        }
}

// Basic hash function for strings, returns num modulo NQUOTE.
inline int hash(const char* buf, int BUFSZ, int NQUOTE) {
    int sum = 0, off = 0;
    while (buf[off] && buf[off] != '\n' && off < BUFSZ) {
        sum = (sum + (int) buf[off++]) % NQUOTE;
    }
    sum = (sum + off) % NQUOTE;
    return sum;
}

// strcmp but with newlines instead of null terminators
inline int nlstrcmp(const char* a, const char* b) {
    while (true) {
        unsigned char ac = *a, bc = *b;
        if (ac == '\n') {
            ac = '\0';
        }
        if (ac == 0 || bc == 0 || ac != bc) {
            return (ac > bc) - (ac < bc);
        }
        ++a, ++b;
    }
}

// User-space mutexes.
struct mutex {
    mutex() : word_(0) {} // start off mutex as unlocked
    void lock();
    void unlock();
 private:
    // 0 == unlocked, 1 == locked with no contention, 2 == locked with contention
    std::atomic<int> word_;
};


#endif
