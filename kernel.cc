#include "kernel.hh"
#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"

// kernel.cc
//
//    This is the kernel.

// # timer interrupts so far on CPU 0
std::atomic<unsigned long> ticks;

// display type; initially KDISPLAY_CONSOLE
std::atomic<int> kdisplay;

static void tick();
static void boot_process_start(pid_t pid, const char* program_name);


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    init_hardware();
    console_clear();

    // set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i] = nullptr;
    }

    // start first process
    boot_process_start(1, CHICKADEE_FIRST_PROCESS);

    // start running processes
    cpus[0].schedule(nullptr);
}


// boot_process_start(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.
//    Only called at initial boot time.

void boot_process_start(pid_t pid, const char* name) {
    // look up process image in initfs
    memfile_loader ld(memfile::initfs_lookup(name), kalloc_pagetable());
    assert(ld.memfile_ && ld.pagetable_);
    int r = proc::load(ld);
    assert(r >= 0);

    // allocate process, initialize memory
    proc* p = knew<proc>();
    p->init_user(pid, ld.pagetable_);
    p->regs_->reg_rip = ld.entry_rip_;

    log_printf("Hello from boot_start()\n");
    log_backtrace();

    void* stkpg = kalloc(PAGESIZE);
    assert(stkpg);
    vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(stkpg, PTE_PWU);
    p->regs_->reg_rsp = MEMSIZE_VIRTUAL;

    // add to process table (requires lock in case another CPU is already
    // running processes)
    {
        spinlock_guard guard(ptable_lock);
        assert(!ptable[pid]);
        ptable[pid] = p;
    }

    // add to run queue
    cpus[pid % ncpu].enqueue(p);
}


// proc::exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the current CPU stack, then jumping to kernel assembly code (in
//    k-exception.S). That code transfers the state to the current kernel
//    task's stack, then calls proc::exception().

void proc::exception(regstate* regs) {
    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    //log_printf("proc %d: exception %d @%p\n", id_, regs->reg_intno, regs->reg_rip);

    // Record most recent user-mode %rip.
    if ((regs->reg_cs & 3) != 0) {
        recent_user_rip_ = regs->reg_rip;
    }

    // Show the current cursor location.
    consolestate::get().cursor();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER: {
        cpustate* cpu = this_cpu();
        if (cpu->cpuindex_ == 0) {
            tick();
        }
        lapicstate::get().ack();
        regs_ = regs;
        yield_noreturn();
        break;                  /* will not be reached */
    }

    case INT_PF: {              // pagefault exception
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* operation = regs->reg_errcode & PFERR_WRITE
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PFERR_PRESENT
                ? "protection problem" : "missing page";

        if ((regs->reg_cs & 3) == 0) {
            panic_at(regs->reg_rsp, regs->reg_rbp, regs->reg_rip,
                     "Kernel page fault for %p (%s %s)!\n",
                     addr, operation, problem);
        }

        error_printf(CPOS(24, 0), 0x0C00,
                     "Process %d page fault for %p (%s %s, rip=%p)!\n",
                     id_, addr, operation, problem, regs->reg_rip);
        pstate_ = proc::ps_broken;
        yield();
        break;
    }

    case INT_IRQ + IRQ_KEYBOARD:
        keyboardstate::get().handle_interrupt();
        break;

    default:
        if (sata_disk && regs->reg_intno == INT_IRQ + sata_disk->irq_) {
            sata_disk->handle_interrupt();
        } else {
            panic_at(regs->reg_rsp, regs->reg_rbp, regs->reg_rip,
                     "Unexpected exception %d!\n", regs->reg_intno);
        }
        break;                  /* will not be reached */

    }

    // return to interrupted context
}


// proc::syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value from `proc::syscall()` is returned to the user
//    process in `%rax`.

uintptr_t proc::syscall(regstate* regs) {
    //log_printf("proc %d: syscall %ld @%p\n", id_, regs->reg_rax, regs->reg_rip);

    // Record most recent user-mode %rip.
    recent_user_rip_ = regs->reg_rip;

    switch (regs->reg_rax) {

    case SYSCALL_KDISPLAY:
        if (kdisplay != (int) regs->reg_rdi) {
            console_clear();
        }
        kdisplay = regs->reg_rdi;
        assert(this->canary == CANARY_EV, "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;

    case SYSCALL_PANIC:
        panic_at(0, 0, 0, "process %d called sys_panic()", id_);
        break;                  // will not be reached

    case SYSCALL_GETPID:
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return id_;

    case SYSCALL_YIELD:
        yield();
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            return -1;
        }
        void* pg = kalloc(PAGESIZE);
        if (!pg || vmiter(this, addr).try_map(ka2pa(pg), PTE_PWU) < 0) {
            return -1;
        }
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;
    }

    case SYSCALL_VARALLOC: { // Variable allocations for buddy allocator
        uintptr_t addr = regs->reg_rdi; // USER VIRTUAL ADDR
        uint64_t sz = regs->reg_rsi;
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            return -1;
        }
        void* ptr = kalloc(sz); // KERNEL VIRTUAL ADDR
        if (!ptr) return -1;
        for (uintptr_t off = 0; 
            off < (1 << order(sz, true)); addr += PAGESIZE, off += PAGESIZE) {
            if (vmiter(this, addr).try_map(ka2pa(ptr), PTE_PWU) < 0) {
                return -1;
            } // If there is no contiguous block available then it won't allocate anything.
        }
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;
    } // CHANGEMADE

    case SYSCALL_FREE: { // Free dat mem (without exiting like a n00b)
        // Use vmiter to get the physical address to pass into kfree
        vmiter it(this, reinterpret_cast<void*>(regs->reg_rdi));
        kfree(pa2kptr(it.pa()));
        if (ptr) {
            
        }
        return 0;
    } // CHANGEMADE

    case SYSCALL_PAUSE: {
        sti();
        for (uintptr_t delay = 0; delay < 1000000; ++delay) {
            pause();
        }
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;
    }

    case SYSCALL_FORK:
        return syscall_fork(regs);
    
    case SYSCALL_NASTY:
        syscall_nasty(100);
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;

    case SYSCALL_READ:
        return syscall_read(regs);

    case SYSCALL_WRITE:
        return syscall_write(regs);

    case SYSCALL_READDISKFILE:
        return syscall_readdiskfile(regs);

    case SYSCALL_SYNC: {
        int drop = regs->reg_rdi;
        // `drop > 1` asserts that no data blocks are referenced (except
        // possibly superblock and FBB blocks). This can only be ensured on
        // tests that run as the first process.
        if (drop > 1 && strncmp(CHICKADEE_FIRST_PROCESS, "test", 4) != 0) {
            drop = 1;
        }
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return bufcache::get().sync(drop);
    }

    case SYSCALL_MAP_CONSOLE: {
        // Get the virtual address to be mapped to, stored in %rdi.
        uintptr_t addr = regs->reg_rdi;

        // Assert that the addr is in low-canonical memory and that the addr is page aligned.
        if (addr > VA_LOWMAX || addr & 0xFFF) {
            return E_INVAL;
        }
        vmiter(this, addr).map(CONSOLE_ADDR, PTE_PWU); // Map the given addr to the console addr
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return 0;
    }

    default:
        // no such system call
        log_printf("%d: no such system call %u\n", id_, regs->reg_rax);
        assert(this->canary == CANARY_EV, 
            "Kernel task stack overflow, detected change in canary\n"); // canary checker
        return E_NOSYS;
    }
}

// proc::copy_memory_(child)
//     Copy all user memory to a child process.
int proc::copy_memory_(proc* child) {
    proc* parent = this;

    //log_printf("itp_va:%lu , itc_va: %lu\n", itp.va(), itc.va());

    assert(ptable_lock.is_locked());
    
    if (child) {
        auto irqs = parent->lock_pagetable_read();
        auto irqs_child = child->lock_pagetable_read();

        if (parent->pagetable_ && parent->pagetable_ != early_pagetable) {
            for (vmiter itp(parent); itp.va() < MEMSIZE_VIRTUAL;) {
                if (itp.pa() == CONSOLE_ADDR) {
                    vmiter itc(child, itp.va());
                    int try_map_code = itc.try_map(itp.pa(), itp.perm());
                    if (try_map_code == -1) {
                        log_printf("Try_map failed from parent: %i\n", parent->id_);
                        parent->unlock_pagetable_read(irqs);
                        child->unlock_pagetable_read(irqs_child);
                        return -1;
                    }
                    itp.next();
                } else if (itp.user()) {
                    void* npg = kalloc(PAGESIZE);
                    if (!npg) {
                        log_printf("kalloc allocation error from parent: pid = %d\n", parent->id_);
                        parent->unlock_pagetable_read(irqs);
                        child->unlock_pagetable_read(irqs_child);
                        return -1;
                    }
                    vmiter itc(child, itp.va());
                    int try_map_code = itc.try_map(npg, itp.perm());
                    if (try_map_code == -1) {
                        log_printf("Try_map failed from parent: pid= %d\n", parent->id_);
                        parent->unlock_pagetable_read(irqs);
                        child->unlock_pagetable_read(irqs_child);
                        return -1;
                    }
                    memcpy((void*) npg, (void*) itp.va(), PAGESIZE);
                    itp.next();
                } else {
                    itp.next_range();
                }
            }
        }
        parent->unlock_pagetable_read(irqs);
        child->unlock_pagetable_read(irqs_child);
    }
    return 0;
}


// proc::syscall_fork(child)
//    Fork a child process.
int proc::syscall_fork(regstate* regs) {
    pid_t pid = 0;
    {
    spinlock_guard guard(ptable_lock);

    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i]) {
            pid = i;
            break;
        }
    }

    // no open pid was found
    if (!pid) {
        log_printf("No open processes, caller PID: %i\n", this->id_);
        return -1;
    } else {
        log_printf("Successfully found a free PID = %d to fork from parent PID = %d\n", pid, this->id_);
    }

    proc* child = knew<proc>(); // allocate new process
    if (!child) {
        log_printf("ERROR: no available memory remaining for child process alloc.\n");
        return -1;
    }
    x86_64_pagetable* child_pt = kalloc_pagetable();
    if (!child_pt) {
        log_printf("ERROR: no available memory remaining for child process alloc.\n");
        return -1;
    }

    child->init_user(pid, child_pt);
    log_printf("Child initialized with early pagetable and set to runnable\n");

    int flag = this->copy_memory_(child);
    if (flag != 0) {
        log_printf("Copying Memory During Fork FAILED, caller: %i\n", this->id_);
        return -1;
    } else {
        log_printf("Memory successfully copied from parent to child!\n");
    }

    // Copy over parent's registers.
    memcpy(child->regs_, regs, sizeof(regstate)); 
    log_printf("Copied parent registers to child\n");

    // add to process table (requires lock in case another CPU is already
    // running processes)
    assert(!ptable[pid]);
    ptable[pid] = child;
    
    child->regs_->reg_rax = 0; // making sure child returns 0

    cpus[pid % ncpu].enqueue(child); // enqueueing on a cpu
    }

    return pid;
}


// proc::syscall_nasty()
//    Nasty recursive allocation to corrupt the stack.
__attribute__((optimize("O0"))) int proc::syscall_nasty(int c) {
    volatile long j  = 100; // some local var
    (void) j;
    if (c <= 0) {
        return 0;
    } else {
        return syscall_nasty(c - 1);
    }
}

// proc::syscall_read(regs), proc::syscall_write(regs),
// proc::syscall_readdiskfile(regs)
//    Handle read and write system calls.

uintptr_t proc::syscall_read(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;

    // Your code here!
    // * Read from open file `fd` (reg_rdi), rather than `keyboardstate`.
    // * Validate the read buffer.
    auto& kbd = keyboardstate::get();
    auto irqs = kbd.lock_.lock();

    // mark that we are now reading from the keyboard
    // (so `q` should not power off)
    if (kbd.state_ == kbd.boot) {
        kbd.state_ = kbd.input;
    }

    // yield until a line is available
    // (special case: do not block if the user wants to read 0 bytes)
    while (sz != 0 && kbd.eol_ == 0) {
        kbd.lock_.unlock(irqs);
        yield();
        irqs = kbd.lock_.lock();
    }

    // read that line or lines
    size_t n = 0;
    while (kbd.eol_ != 0 && n < sz) {
        if (kbd.buf_[kbd.pos_] == 0x04) {
            // Ctrl-D means EOF
            if (n == 0) {
                kbd.consume(1);
            }
            break;
        } else {
            *reinterpret_cast<char*>(addr) = kbd.buf_[kbd.pos_];
            ++addr;
            ++n;
            kbd.consume(1);
        }
    }

    kbd.lock_.unlock(irqs);
    return n;
}

uintptr_t proc::syscall_write(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;

    // Your code here!
    // * Write to open file `fd` (reg_rdi), rather than `consolestate`.
    // * Validate the write buffer.
    auto& csl = consolestate::get();
    spinlock_guard guard(csl.lock_);
    size_t n = 0;
    while (n < sz) {
        int ch = *reinterpret_cast<const char*>(addr);
        ++addr;
        ++n;
        console_printf(0x0F00, "%c", ch);
    }
    return n;
}

uintptr_t proc::syscall_readdiskfile(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    const char* filename = reinterpret_cast<const char*>(regs->reg_rdi);
    unsigned char* buf = reinterpret_cast<unsigned char*>(regs->reg_rsi);
    size_t sz = regs->reg_rdx;
    off_t off = regs->reg_r10;

    if (!sata_disk) {
        return E_IO;
    }

    // read root directory to find file inode number
    auto ino = chkfsstate::get().lookup_inode(filename);
    if (!ino) {
        return E_NOENT;
    }

    // read file inode
    ino->lock_read();
    chkfs_fileiter it(ino);

    size_t nread = 0;
    while (nread < sz) {
        // copy data from current block
        if (bcentry* e = it.find(off).get_disk_entry()) {
            unsigned b = it.block_relative_offset();
            size_t ncopy = min(
                size_t(ino->size - it.offset()),   // bytes left in file
                chkfs::blocksize - b,              // bytes left in block
                sz - nread                         // bytes left in request
            );
            memcpy(buf + nread, e->buf_ + b, ncopy);
            e->put();

            nread += ncopy;
            off += ncopy;
            if (ncopy == 0) {
                break;
            }
        } else {
            break;
        }
    }

    ino->unlock_read();
    ino->put();
    return nread;
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

static void memshow() {
    static unsigned long last_redisplay = 0;
    static unsigned long last_switch = 0;
    static int showing = 1;

    // redisplay every 0.04 sec
    if (last_redisplay != 0 && ticks - last_redisplay < HZ / 25) {
        return;
    }
    last_redisplay = ticks;

    // switch to a new process every 0.5 sec
    if (ticks - last_switch >= HZ / 2) {
        showing = (showing + 1) % NPROC;
        last_switch = ticks;
    }

    spinlock_guard guard(ptable_lock);

    int search = 0;
    while ((!ptable[showing]
            || !ptable[showing]->pagetable_
            || ptable[showing]->pagetable_ == early_pagetable)
           && search < NPROC) {
        showing = (showing + 1) % NPROC;
        ++search;
    }

    console_memviewer(ptable[showing]);
    if (!ptable[showing]) {
        console_printf(CPOS(10, 29), 0x0F00, "VIRTUAL ADDRESS SPACE\n"
            "                          [All processes have exited]\n"
            "\n\n\n\n\n\n\n\n\n\n\n");
    }
}


// tick()
//    Called once every tick (0.01 sec, 1/HZ) by CPU 0. Updates the `ticks`
//    counter and performs other periodic maintenance tasks.

void tick() {
    // Update current time
    ++ticks;

    // Update memviewer display
    if (kdisplay.load(std::memory_order_relaxed) == KDISPLAY_MEMVIEWER) {
        memshow();
    }
}
