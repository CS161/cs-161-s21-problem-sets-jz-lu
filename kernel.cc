#include "kernel.hh"
#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-vfs.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"

// kernel.cc
//
//    This is the kernel.

// # timer interrupts so far on CPU 0.
std::atomic<unsigned long> ticks;

// Display type; initially KDISPLAY_CONSOLE.
std::atomic<int> kdisplay;

// Global Stdio vnode on VFS.
vnode* global_cnode = nullptr;

// Global blocking data.
const uint64_t NUM_WQS = 5;
wait_queue time_wheel[NUM_WQS]; // Sleep wait wheel
wait_heap time_heap; // Sleep wait heap
wait_queue parent_child_queue; // Waitpid queue (nondeterministic time)
uint64_t BLOCK_NUM_RESUMES = 0;

// Unix Domain Socket.
uds* sockets[NSOCK] = {0};

static void tick();
static void boot_process_start(pid_t pid, const char* program_name);

// k_proc_init()
//    The almighty init process, holding PID = PPID = 1.
//    Handles final zombie waitpid-ing.
void k_proc_init() {
    regstate regs;
    regs.reg_rdi =  0; // pid = 0 (Wait for first one to exit)
    regs.reg_rsi =  0; // status = nullptr (status not needed)
    regs.reg_rdx =  0; // options = 0 (blocking)
    proc *kinit = ptable[1];

    // Waitpid forever (Not the most exciting of jobs, but someone gotta do it).
    while (true) {
        {
        spinlock_guard guard(ptable_lock);
        if (WAITPID_PARANOIA >= 1 && kinit->nchildren_) {
            log_printf("Children of ALMIGHTY INIT with PID=%d (bow down): [", kinit->id_);
            for (int i = 0; i < (kinit->nchildren_); ++i) {
                log_printf("%d ", kinit->childpids_[i]);
            }
            log_printf("]\n");
        }
        }
        // Halt if there are no children, or if the only children are broken.
        if (kinit->syscall_waitpid(&regs) == (uint64_t) E_CHILD) {
            break; // No children
        }
        {
        spinlock_guard guard(ptable_lock);
        int nbroken = false;
        for (int i = 0; i < kinit->nchildren_; ++i) {
            if (ptable[kinit->childpids_[i]]->pstate_ == proc::ps_broken) {
                ++nbroken;
                break;
            }
        }   
        if (nbroken == kinit->nchildren_) {
            break;
        }
        }
    }
    if (TRUEBLOCK_TESTING) {
        log_printf("[k_proc_init] [TRUE BLOCK TEST] [TYPE=%s] Total num resumes recorded: %lu\n",
            USING_PSEUDO_BLOCKING ? "Pseudoblock" : "Trueblock", BLOCK_NUM_RESUMES);
    }
    log_printf("[k_proc_init] halting QEMU, goodbye cruel world\n");
    process_halt();
}

// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
    init_hardware();
    console_clear();

    proc *init_task = knew<proc>();
    {
        spinlock_guard guard(ptable_lock);
        // set up process descriptors
        for (pid_t i = 0; i < NPROC; i++) {
            ptable[i] = nullptr;
        }
        init_task->ppid_ = 1;
        init_task->init_kernel(1, k_proc_init);
        assert(!ptable[1]);
        ptable[1] = init_task;
        init_task->childpids_[init_task->nchildren_] = 2;
        ++init_task->nchildren_;
    }
    cpus[0].enqueue(init_task);

    // start first process, at pid = 2
    boot_process_start(2, CHICKADEE_FIRST_PROCESS);

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

    void* stkpg = kalloc(PAGESIZE);
    assert(stkpg);
    vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(stkpg, PTE_PWU);
    vmiter(p, CONSOLE_ADDR).map(CONSOLE_ADDR, PTE_PWU); // Map virtual console addr to physical console addr
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
        regs_ = regs; // Set the regs in the proc metadata for the return to process via resume regstate
        
        // Rise and shine! Wake up sleeping processes.
        if (WAITQ_PARANOIA >= 2 || WAITH_PARANOIA >= 2) {
            log_printf("[irq_timer] Timer fired at tick %ld, transferring to heap\n", 
                (uint64_t) ticks);
        }
        // Wake up all the relevant processes in case any need to stop sleeping.
        if (!USING_TIME_HEAP) {
            time_wheel[((uint64_t) ticks)%NUM_WQS].wake_all();
        }
        else {
            time_heap.flush(true);
        }

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
    // Record most recent user-mode %rip.
    recent_user_rip_ = regs->reg_rip;
    uintptr_t syscall_retval = 0;

    switch (regs->reg_rax) {

    case SYSCALL_KDISPLAY:
        if (kdisplay != (int) regs->reg_rdi) {
            console_clear();
        }
        kdisplay = regs->reg_rdi;
        break;

    case SYSCALL_PANIC:
        panic_at(0, 0, 0, "process %d called sys_panic()", id_);
        break;                  // will not be reached

    case SYSCALL_GETPID:
        syscall_retval = id_;
        break;
    
    case SYSCALL_GETPPID: {
        spinlock_guard guard(ptable_lock);
        if (PPID_PARANOIA >= 1) {
            log_printf("[syscall_ppid] Process PID=%d has PPID=%d\n", id_, ppid_);
        }
        syscall_retval = ppid_;
        break;
    }

    case SYSCALL_YIELD:
        yield();
        break;

    case SYSCALL_PAGE_ALLOC: {
        uintptr_t addr = regs->reg_rdi;
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            return -1;
        }
        void* pg = kalloc(PAGESIZE);
        if (!pg || vmiter(this, addr).try_map(ka2pa(pg), PTE_PWU) < 0) {
            return -1;
        }
        break;
    }

    case SYSCALL_VARALLOC: { // Variable allocations for buddy allocator
        uintptr_t addr = regs->reg_rdi; // USER VIRTUAL ADDR
        uint64_t sz = regs->reg_rsi;
        log_printf("[SYSCALL_VARALLOC] NEW request for alloc at UVA 0x%x of size 0x%x\n", addr, sz);
        if (addr >= VA_LOWEND || addr & 0xFFF) {
            return -1;
        }
        void* ptr = kalloc(sz); // KERNEL VIRTUAL ADDR
        if (!ptr) return -1;
        vmiter it(this, addr);
        log_printf("[SYSCALL_VARALLOC] MAPPING successful alloc\n");
        for (uint64_t off = 0; 
            off < (1UL << order(sz, true)); off += PAGESIZE, it += PAGESIZE) {
            if (it.try_map(ka2pa(ptr), PTE_PWU) < 0) {
                return -1;
            } // If there is no contiguous block available then it won't allocate anything.
        }
        break;
    }

    case SYSCALL_FREE: { // Free dat mem (without exiting like a n00b)
        // Use vmiter to get the physical address to pass into kfree
        vmiter it(this, regs->reg_rdi); // %rdi holds UVA
        log_printf("[SYSCALL_FREE] NEW free request for alloc at UVA 0x%x, PA 0x%x\n", 
            regs->reg_rdi, it.pa() /* , 1UL << blk_order(it.pa()) */);
        uint64_t blk_sz = 1UL << blk_order(it.pa());
        for (uint64_t off = 0; off < blk_sz; off += PAGESIZE, it += PAGESIZE) {
            it.kfree_page();
        }
        break;
    }

    case SYSCALL_TESTKALLOC: 
        syscall_retval = syscall_testkalloc(regs);
        break;

    case SYSCALL_WILDALLOC:
        syscall_retval = syscall_wildalloc(regs);
        break;

    case SYSCALL_PAUSE: {
        sti();
        for (uintptr_t delay = 0; delay < 1000000; ++delay) {
            pause();
        }
        break;
    }

    case SYSCALL_FORK: {
        pid_t pid = syscall_fork(regs);
        if (FORK_PARANOIA >= 3) {
            log_printf("[syscall] fork returned a pid %d\n", pid);
        }
        syscall_retval = pid;
        break;
    }
    
    case SYSCALL_NASTY: {
        long n = syscall_nasty(regs);
        syscall_retval = n % 10;
        break;
    }

    case SYSCALL_OPEN:
        syscall_retval = syscall_open(regs);
        break;

    case SYSCALL_DUP2:
        syscall_retval = syscall_dup2(regs);
        break;
    
    case SYSCALL_PIPE:
        syscall_retval = syscall_pipe(regs);
        break;

    case SYSCALL_READ:
        syscall_retval = syscall_read(regs);
        break;

    case SYSCALL_WRITE:
        syscall_retval = syscall_write(regs);
        break;
    
    case SYSCALL_CLOSE:
        syscall_retval = syscall_close(regs);
        break;

    case SYSCALL_EXECV:
        syscall_retval = syscall_execv(regs);
        break;

    case SYSCALL_READDISKFILE:
        syscall_retval = syscall_readdiskfile(regs);
        break;

    case SYSCALL_SYNC: {
        int drop = regs->reg_rdi;
        // `drop > 1` asserts that no data blocks are referenced (except
        // possibly superblock and FBB blocks). This can only be ensured on
        // tests that run as the first process.
        if (drop > 1 && strncmp(CHICKADEE_FIRST_PROCESS, "test", 4) != 0) {
            drop = 1;
        }
        syscall_retval = bufcache::get().sync(drop);
        break;
    }

    case SYSCALL_MAP_CONSOLE: {
        // Get the virtual address to be mapped to, stored in %rdi.
        uintptr_t addr = regs->reg_rdi;

        // Assert that the addr is in low-canonical memory and that the addr is page aligned.
        if (addr > VA_LOWMAX || addr & 0xFFF) {
            return E_INVAL;
        }
        syscall_retval = vmiter(this, addr).try_map(CONSOLE_ADDR, PTE_PWU); // Map the given addr to the console addr
        break;
    }

    case SYSCALL_EXIT:
        syscall_exit(regs);
        break;
    
    case SYSCALL_MSLEEP:
        syscall_retval = syscall_msleep(regs);
        break;
    
    case SYSCALL_WAITPID:
        syscall_retval = syscall_waitpid(regs);
        break;

    default:
        // no such system call
        log_printf("Process PID=%d: no such system call num=%u\n", id_, regs->reg_rax);
        syscall_retval = E_NOSYS;
        break;
    }
    assert(this->canary == CANARY_EV, "Kernel task stack overflow detected via canary corruption\n"); 
    return syscall_retval;
}

// proc::copy_memory_(child)
//     Copy all user memory to a child process.
int proc::copy_memory_(proc* child) {
    proc* parent = this;

    assert(ptable_lock.is_locked());
    if (!child) {
        return E_INVAL; // Null child given
    }

    auto irqs = parent->lock_pagetable_read();
    auto irqs_child = child->lock_pagetable_read();
    
    if (child) {
        if (parent->pagetable_ && parent->pagetable_ != early_pagetable) {
            for (vmiter itp(parent); itp.va() < MEMSIZE_VIRTUAL;) {
                if (itp.pa() == CONSOLE_ADDR) {
                    vmiter itc(child, itp.va());
                    int try_map_code = itc.try_map(itp.pa(), itp.perm());
                    if (try_map_code == -1) {
                        log_printf("Try_map failed from parent: %d\n", parent->id_);
                        goto free_mem_maps;
                    }
                    itp.next();
                } else if (itp.user()) {
                    void* npg = kalloc(PAGESIZE);
                    if (!npg) {
                        log_printf("[fork::copy_memory] kalloc allocation error from parent: pid = %d\n", parent->id_);
                        goto free_mem_maps;
                    } else if (FORK_PARANOIA >= 2) {
                        log_printf("[fork::copy_memory] NEW page copy at va %p, pa 0x%x from parent: pid = %d\n",
                            npg, kptr2pa(npg), parent->id_);
                    }
                    vmiter itc(child, itp.va());
                    int try_map_code = itc.try_map(npg, itp.perm());

                    // Simulate failed mapping, encompasses all test cases for memcopies
                    if (FORK_TESTING && rand(0, 10) < 4) { 
                        log_printf("[forktest] Simulating failed child mem copy for parent process %d\n", this->id_);
                        try_map_code = -1;
                    }
                    if (try_map_code == -1) {
                        log_printf("[fork::copy_memory] Try_map failed from parent: pid= %d\n", parent->id_);
                        if (!FORK_TESTING) {
                            kfree(npg); // Simulation would make this a double free!
                        }
                        goto free_mem_maps;
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

    free_mem_maps:
        // Walk the virtual address space and free all mem
        free_auto_allocs(child);
        if (FORK_TESTING || FORK_PARANOIA >= 2) {
            log_printf("[copy_memory] Finished freeing unfinished child's vmiter/ptiter mem\n");
        }
        parent->unlock_pagetable_read(irqs);
        child->unlock_pagetable_read(irqs_child);
        return E_NOMEM;
}


// proc::syscall_fork(regs)
//    Fork a child process.
int proc::syscall_fork(regstate* regs) {
    if (FORK_PARANOIA >= 1) {
        log_printf("[fork] Fork called by parent process %d\n", this->id_);
    }
    pid_t pid = 0;
    {
    spinlock_guard guard(ptable_lock);

    for (pid_t i = 1; i < NPROC; i++) {
        if (!ptable[i]) {
            pid = i;
            break;
        }
    }

    // Handle: No available process table entry found.
    if (!pid) {
        if (FORK_PARANOIA >= 1) {
            log_printf("No open processes, caller PID: %d\n", this->id_);
        }
        return -1;
    } else {
        if (FORK_PARANOIA >= 1) {
            log_printf("Successfully found a free PID = %d to fork from parent PID = %d\n", pid, this->id_);
        }
    }

    proc* child = knew<proc>(); // allocate new process
    if (FORK_TESTING == 1 && rand(0, 5) < 1) { // Testing: simulate struct proc alloc failure
        log_printf("[forktest] Simulating child struct proc failed alloc for parent process %d\n", this->id_);
        kfree(child);
        child = nullptr;
    }
    int memcpy_failed = 1; // Initialize variables before goto statements.
    x86_64_pagetable* child_pt = nullptr;

    if (!child) {
        if (FORK_PARANOIA >= 1) {
            log_printf("[fork] ERROR: no available memory remaining for child process struct.\n");
        }
        goto eret;
    } else {
        if (FORK_PARANOIA >= 2) {
            log_printf("[fork] Child proc struct va: %p, pa: 0x%x\n", child, kptr2pa(child));
        }
    }
    child_pt = kalloc_pagetable();
    if (FORK_TESTING == 1 && rand(0, 1) < 1) { // simulate child pt alloc failure
        log_printf("[forktest] Simulating child pt failed alloc for parent process %d\n", this->id_);
        kfree(child_pt);
        child_pt = nullptr;
    }
    if (!child_pt) {
        if (FORK_PARANOIA >= 1) {
            log_printf("[fork] ERROR: no available memory remaining for child pagetable.\n");
        }
        goto free_proc;
    } else {
        if (FORK_PARANOIA >= 2) {
            log_printf("[fork] Child pt va: %p, pa: 0x%x\n", child_pt, kptr2pa(child_pt));
        }
    }

    child->init_user(pid, child_pt);
    if (FORK_PARANOIA >= 2) {
        log_printf("[fork] Child initialized with early pagetable and set to runnable\n");
    }

    memcpy_failed = this->copy_memory_(child);
    if (memcpy_failed) {
        if (FORK_PARANOIA >= 1) {
            log_printf("[fork] Copying Memory During Fork FAILED, caller: %d\n", this->id_);
        }
        goto free_pt;
    } else {
        if (FORK_PARANOIA >= 2) {
            log_printf("[fork] Memory successfully copied from parent to child!\n");
        }
    }

    // Copy over parent's registers.
    memcpy(child->regs_, regs, sizeof(regstate));
    if (FORK_PARANOIA >= 2) {
        log_printf("[fork] Copied parent registers to child\n");
    }

    // Copy over parent's fdtable and increment refcount.
    // TODO [MULTITH] lock access to fdtable
    memcpy((void*) &(child->fdtable), (void*) &fdtable, MAX_FD*sizeof(vnode*));
    for (int fd = 0; fd < MAX_FD; ++fd) {
        if (fdtable[fd]) {
            spinlock_guard refguard(fdtable[fd]->open_close_lock_);
            ++fdtable[fd]->refcount_;
        }
    }
    if (FORK_PARANOIA >= 1 || VFS_PARANOIA >= 2) {
        log_printf("[fork] Copied parent's (PID=%d) fdtable to child (PID=%d), new state:\n",
            id_, pid);
        child->show_fdtable_();
    }

    // Add to process table (requires lock in case another CPU is already
    // running processes)
    assert(!ptable[pid]);
    ptable[pid] = child;
    
    child->regs_->reg_rax = 0; // making sure child returns 0

    cpus[pid % ncpu].enqueue(child); // enqueueing on a cpu
    if (WAITQ_PARANOIA >= 2) {
        log_printf("[fork] [pre-return] Child PID=%d to run on CPU %d\n", pid, pid % ncpu);
    }

    if (FORK_PARANOIA >= 1) {
        log_printf("[fork] Returning NEW process PID %d\n", pid);
    }

    child->ppid_ = this->id_;
    this->childpids_[this->nchildren_] = pid;
    this->nchildren_++;
    if (FORK_PARANOIA >= 1) {
        log_printf("[fork] Setting child ppid to %d and updating this parent's metadata\n", child->ppid_);
    }

    // Print updated metrics for waitpid.
    if (WAITPID_PARANOIA >= 1) {
        log_printf("[fork] [return] this pid: %i, num_children: %i, child pids: [", 
            this->id_, this->nchildren_);
        for (int i = 0; i < this->nchildren_; ++i) {
            log_printf("%d ", this->childpids_[i]);
        }
        log_printf("]\n");
    }

    return pid;

    // Fork failure cleanup methods, accessed via goto.
    free_pt:
        if (FORK_PARANOIA || FORK_TESTING) {
            log_printf("[fork] Fork failed, freeing process pagetable\n");
        }
        kfree(child_pt);
        if (FORK_PARANOIA || FORK_TESTING) {
            log_printf("[fork] Process pagetable freed\n");
        }
    free_proc:
        if (FORK_PARANOIA || FORK_TESTING) {
            log_printf("[fork] Fork failed, freeing struct proc\n");
        }
        kfree(child);
        if (FORK_PARANOIA || FORK_TESTING) {
            log_printf("[fork] Struct proc freed\n");
        }
    eret:
        if (FORK_PARANOIA || FORK_TESTING) {
            log_printf("[fork] Exiting with exit status %d (no memory)\n", E_NOMEM);
        }
        return E_NOMEM;
    }
}


// proc::syscall_nasty()
//    Nasty large array allocation to corrupt the stack.
int proc::syscall_nasty(regstate* regs) {
    volatile uint64_t arr[1600];

    for (int i = 0; i < 1600; ++i) {
        arr[i] = rand();
    }

    volatile int sum = 0;
    for (int i = 0; i < 1600; ++i) {
        sum += arr[i];
    }

    return sum;
}


// proc::syscall_testkalloc() 
//    Test cases for buddy allocator.
int proc::syscall_testkalloc(regstate* regs) {
    int tcase = regs->reg_rdi;
    int nallocs = 50;
    void* ptrs[nallocs];

    switch (tcase) {
        case 0: { // Single-page allocs
            uint64_t sz = PAGESIZE;
            for (int i = 0; i < nallocs; ++i) {
                ptrs[i] = kalloc(sz);
            }

            for (int i = 0; i < nallocs; ++i) {
                kfree(ptrs[i]);
            }
            // Allocate a bunch of pages in succession, then free them.
            log_printf("======= TEST CASE [0] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 1: { // Allocations of sizes with random orders
            // Expect lots of "no allocs" due to orders being huge.
            int ro = 0;
            uint64_t sz = PAGESIZE;

            for (int i = 0; i < nallocs; ++i) {
                ro = rand(MIN_ORDER, MAX_ORDER);
                sz = 1 << ro;
                ptrs[i] = kalloc(sz);
            }

            for (int i = 0; i < nallocs; ++i) {
                kfree(ptrs[i]);
            }
            log_printf("======= TEST CASE [1] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 2: { // Random allocations, not necessarily multiples of PAGESIZE
            // Again, expect lots of failed allocs due to large sizes.
            uint64_t sz;

            for (int i = 0; i < nallocs; ++i) {
                sz = rand(1 << MIN_ORDER, 1 << MAX_ORDER);
                ptrs[i] = kalloc(sz);
            }

            for (int i = 0; i < nallocs; ++i) {
                kfree(ptrs[i]);
            }
            log_printf("======= TEST CASE [2] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 3: { // smaller random non-PAGESIZE multiple allocations
            // Expect less failed allocations this time.
            uint64_t sz;
            for (int j = 0; j < 10; ++j) {
                for (int i = 0; i < nallocs; ++i) {
                    sz = rand(1 << MIN_ORDER, 1 << (MAX_ORDER - 5));
                    ptrs[i] = kalloc(sz);
                }

                for (int i = 0; i < nallocs; ++i) {
                    kfree(ptrs[i]);
                }
            }
            log_printf("======= TEST CASE [3] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }
        // ----- Slab allocator test starts here! -----
        case 4: { // Randomized small slab allocations
            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < nallocs; ++i) {
                    sz = rand(1 << 2, 1 << 6);
                    ptrs[i] = kalloc(sz);
                }
                for (int i = 0; i < nallocs; ++i) {
                    kfree(ptrs[i]);
                }
            }
            log_printf("======= {SLAB} TEST CASE [4] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;

        }

        case 5: { // Randomized big slab allocations
            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < nallocs; ++i) {
                    sz = rand(1 << 7, (1 << 9) - 8);
                    ptrs[i] = kalloc(sz);
                }
                for (int i = 0; i < nallocs; ++i) {
                    kfree(ptrs[i]);
                }
            }
            log_printf("======= {SLAB} TEST CASE [5] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 6: { // Randomly switch between the big slab and small slab
            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < nallocs; ++i) {
                    sz = rand(1 << 2, (1 << 9)- 8);
                    ptrs[i] = kalloc(sz);
                }
                for (int i = 0; i < nallocs; ++i) {
                    kfree(ptrs[i]);
                }
            }
            log_printf("======= {SLAB} TEST CASE [6] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        case 7: { // Randomly switch between the slab allocator and buddy allocator
            for (int j = 0; j < 10; ++j) {
                uint64_t sz;
                for (int i = 0; i < nallocs; ++i) {
                    sz = rand(1 << 2, 1 << (MIN_ORDER + 2));
                    ptrs[i] = kalloc(sz);
                }
                for (int i = 0; i < nallocs; ++i) {
                    kfree(ptrs[i]);
                }
            }
            log_printf("======= {SLAB} TEST CASE [7] for PROCESS [%d] COMPLETED =======\n", this->id_);
            break;
        }

        default: {
            log_printf("======= ERROR: Test case number %d not implemented  =======\n", tcase);
            break;
        }
    }
    return 0;
}


// proc::syscall_wildalloc(regs)
//    Wild test cases for buddy allocator.
int proc::syscall_wildalloc(regstate* regs) {
    int c = regs->reg_rdi;
    switch (c) {
        case 1: { // Invalid free of unallocated pointer
            log_printf("[sys_wildalloc] Running wild allocation 1: free unallocated pointer\n");
            void* null_ptr = nullptr;
            kfree(null_ptr); // This should do nothing.
            void *nasty_ptr = kalloc(PAGESIZE);
            kfree(nasty_ptr);
            kfree(nasty_ptr); // Should fail an assert statement with the allocator.
            break;
        }

        case 2: { // Invalid free of non page-aligned pointer
            log_printf("[sys_wildalloc] Running wild allocation 2: free non page-aligned pointer\n");
            void *ptr = kalloc(PAGESIZE);
            uint64_t addr = kptr2pa(ptr) + 6;
            kfree(pa2kptr<void*>(addr));
            break;
        }

        case 3: { // Invalid free in the middle of an allocation
            log_printf("[sys_wildalloc] Running wild allocation 3: free in middle of block\n");
            void *ptr = kalloc(2*PAGESIZE);
            uint64_t addr = reinterpret_cast<uint64_t>(ptr) + PAGESIZE;
            kfree(reinterpret_cast<void*>(addr));
            break;
        }

        default: {
            log_printf("[sys_wildalloc] No wild allocations were run.\n");
        }
    }
    return 0;
}


// proc::free_auto_allocs()
//    Frees all memory of a process except the struct proc and the L4 pt.
//    If freeing a runnable process, assumes locked.
void proc::free_auto_allocs(proc* p) {
    // Free all allocated pages.
    auto irqs = p->lock_pagetable_read();
    for (vmiter itc(p); itc.va() < MEMSIZE_VIRTUAL; ) {
        if (itc.va() == CONSOLE_ADDR) {
            itc.next(); // Ignore the console
        } else if (itc.user()) {
            if (EXIT_PARANOIA >= 2) {
                log_printf("[free_auto_allocs] FREEING VA 0x%x, i.e. PA 0x%x\n", itc.va(), itc.pa());
            }
            itc.kfree_page();
            itc.next();
        } else {
            itc.next_range(); // Skip unallocated ranges to save time
        }
    }

    // Now that all allocated mempages are freed, free the pagetable.
    for (ptiter it(p); it.low(); it.next()) {
        if (EXIT_PARANOIA >= 2) {
            log_printf("[free_auto_allocs] FREEING VA 0x%x, i.e. PA 0x%x\n", it.va(), it.pa());
        }
        it.kfree_ptp();
    }
    p->unlock_pagetable_read(irqs);
}


void proc::free_auto_allocs(x86_64_pagetable* pt) {
    // Free all allocated pages.
    auto irqs = this->lock_pagetable_read();
    for (vmiter itc(pt); itc.va() < MEMSIZE_VIRTUAL; ) {
        if (itc.va() == CONSOLE_ADDR) {
            itc.next(); // Ignore the console
        } else if (itc.user()) {
            if (EXIT_PARANOIA >= 2) {
                log_printf("[free_auto_allocs] FREEING VA 0x%x, i.e. PA 0x%x\n", itc.va(), itc.pa());
            }
            itc.kfree_page();
            itc.next();
        } else {
            itc.next_range(); // Skip unallocated ranges to save time
        }
    }

    // Now that all allocated mempages are freed, free the pagetable.
    for (ptiter it(pt); it.low(); it.next()) {
        if (EXIT_PARANOIA >= 2) {
            log_printf("[free_auto_allocs] FREEING VA 0x%x, i.e. PA 0x%x\n", it.va(), it.pa());
        }
        it.kfree_ptp();
    }
    this->unlock_pagetable_read(irqs);
}

// proc::syscall_exit(regs)
//    Exits a process without data races
void proc::syscall_exit(regstate* regs) {
    proc* p = this;
    pid_t pid = this->id_;

    {
    spinlock_guard guard(ptable_lock);

    if (EXIT_PARANOIA >= 1 || WAITQ_PARANOIA >= 1 || WAITPID_PARANOIA >= 1) {
        log_printf("[exit] Freeing process at VA 0x%x with pid %lu\n", p, pid);
    }

    // Close all open file descriptors.
    if (EXIT_PARANOIA >= 2 || VFS_PARANOIA >= 2) {
        log_printf("[exit] Closing all open file descriptors\n");
    }
    // TODO [MULTITH] lock fdtable accesses.
    for (int fd = 0; fd < MAX_FD; ++fd) {
        if (fdtable[fd]) {
            if (!fdtable[fd]->close()) { // If refcount hits 0
                if (VFS_PARANOIA >= 1 || EXIT_PARANOIA >= 2) {
                    log_printf("[exit] Closing node fd=%d empty, freeing\n", fd);
                }
                delete fdtable[fd];
            }
        }
    }

    // Set pagetable to early_pagetable and
    // Free all allocations except L4 pt and struct proc.
    set_pagetable(early_pagetable);
    free_auto_allocs(this);

    // Free the L4 pagetable of the process.
    kfree(this->pagetable_);
    this->pagetable_= nullptr;

    if (EXIT_PARANOIA >=1) {
        log_printf("[exit] this process has ppid %d, who has %d children\n", 
            this->ppid_, ptable[this->ppid_]->nchildren_);
        log_printf("[exit] Children PIDs: [");
        for (int i = 0; i < ptable[this->ppid_]->nchildren_; ++i) {
            log_printf("%d ", ptable[this->ppid_]->childpids_[i]);
        }
        log_printf("]\n");
    }

    // Don't update the process's parent's metadata in exit(). Instead, we will do it
    // in waitpid() once the process is fully freed from zombie mode.
    // BUT we do want to set the parent proc interrupt flag to E_INTR.
    ptable[ppid_]->e_intr = E_INTR;

    // Reparent process's children to k_proc_init.
    proc *kinit = ptable[1];
    for (int i = 0; i < this->nchildren_; ++i) {
       ptable[this->childpids_[i]]->ppid_ = 1; // Reparent to k_proc_init
       if (WAITPID_PARANOIA >= 1) {
           log_printf("[exit] REPARENTED child process PID=%d, NEW-PPID=%d to k_proc_init\n", 
            ptable[this->childpids_[i]]->id_, ptable[this->childpids_[i]]->ppid_);
       }
       kinit->childpids_[kinit->nchildren_] = this->childpids_[i];
       kinit->nchildren_++;
    }
    if (WAITPID_PARANOIA >= 1) {
        log_printf("[exit] UPDATED k_proc_init child array to [");
        for (int i = 0; i < ptable[1]->nchildren_; ++i) {
            log_printf("%d ", ptable[1]->childpids_[i]);
        }
        log_printf("]\n");
    }

    // Mark the process as transitioning, but don't clear its entry on pagetable.
    // (that's for waitpid to do!)
    p->pstate_ = USING_PSEUDO_BLOCKING ? ps_blank : ps_transition;
    p->retval = regs->reg_rdi; // Set the return value to the status specified by caller of exit.

    if (USING_TIME_HEAP) {
        if (WAITH_PARANOIA >= 2) {
            log_printf("[exit] Flushing time heap to wake all time-sleeping processes\n");
        }
        time_heap.flush(true);
    }

    if (EXIT_PARANOIA >= 1) {
        log_printf("[exit] Finished turning process PID=%d into a zombie\n", this->id_);
    }
    }
    yield_noreturn();
}


// proc::syscall_msleep(regs)
//    Sleeps for msec milliseconds rounded up to nearest 10.
int proc::syscall_msleep(regstate* regs) {
    unsigned long wakeup_time = ticks + (regs->reg_rdi + 9) / 10;
    // Make sure there's no overflow
    assert(wakeup_time > ticks, "Invalid sleep request, reboot Chickadee or make sleep time smaller");

    if (USING_PSEUDO_BLOCKING) {
        log_printf("[msleep] [PSEUDOBLOCK] msleep called by PID=%d, to wake at ticks=%lu\n", 
            id_, wakeup_time);
        while (long(wakeup_time - ticks) > 0 && e_intr == 0) {
            yield();
        }
    }
    else {
        if (WAITQ_PARANOIA >= 1 || WAITH_PARANOIA >= 1) {
            log_printf("[msleep] [TRUEBLOCK] msleep called by process PID=%d to sleep until %lu\n", 
                id_, wakeup_time);
        }

        if (USING_TIME_HEAP) {
            wait_heap* wh = &time_heap;
            if (WAITH_PARANOIA >= 2) {
                log_printf("[msleep] wh VA=%p, PA=0x%x, ticks=%ld\n",
                    wh, kptr2pa(wh), (unsigned long) ticks);
            }
            e_intr = 0; // Zero out flag until checking
            hwaiter().block_until(*wh, wakeup_time, [&] () {
                if (WAITH_PARANOIA >= 2) {
                    log_printf("[predicate] waketime=%d\n", wakeup_time);
                }
                return (long(wakeup_time - ticks) <= 0 || e_intr != 0);
            });
        }
        else { // Using time wheel
            wait_queue* wq = &time_wheel[wakeup_time%NUM_WQS];
            if (WAITQ_PARANOIA >= 2) {
                log_printf("[msleep] wq VA=%p, PA=0x%x, ticks=%ld\n", 
                    wq, kptr2pa(wq), (unsigned long) ticks);
            }
            e_intr = 0; // Zero out flag until checking
            waiter().block_until(*wq, [&] () {
                if (WAITQ_PARANOIA >= 2) {
                    log_printf("[predicate] waketime=%d\n", wakeup_time);
                }
                return (long(wakeup_time - ticks) <= 0 || e_intr != 0);
            });
        }
    }

    if (WAITQ_PARANOIA >= 2 || WAITH_PARANOIA >= 2) {
        log_printf("[msleep] SLEEP OVER from process PID=%d\n", id_);
    }

    return e_intr;
}


// release_child()
//    Helper function to sys_waitpid()
//    Updates the child metadata to complete the zombie reaping.
static void release_child(proc* p, pid_t cpid) { 
    if (WAITPID_PARANOIA >= 2) {
        log_printf("[release_child] Release child called\n");
    }
    int ch_ind = -1;
    for (int i = 0; i < p->nchildren_; ++i) {
        if (WAITPID_PARANOIA >= 2) {
            log_printf("[release_child] From parent pid %d, freeing child pid %d. Current search: arr[%d]=%d\n", 
                p->id_, cpid, i, p->childpids_[i]);
        }
        if (cpid == p->childpids_[i]) {
            ch_ind = i;
            if (WAITPID_PARANOIA >= 2) {
                log_printf("[release_child] MATCHED child to arr[%d]=%d\n", i, p->childpids_[i]);
            }
            break;
        }
    }
    assert(ch_ind != -1);

    // Update the child-array to get rid of the fully exited child proc.
    for (int i = ch_ind + 1; i < p->nchildren_; ++i) {
        p->childpids_[i-1] = p->childpids_[i];
    }
    --p->nchildren_;

    if (WAITPID_PARANOIA >= 2) {
        log_printf("[release_child] Updated child array: [");
        for (int i = 0; i < ptable[p->id_]->nchildren_; ++i) {
            log_printf("%d ", ptable[p->id_]->childpids_[i]);
        }
        log_printf("]; num (alive) children = %d\n", p->nchildren_);
    }
}


// proc::syscall_waitpid(regs)
//    Waits for either a specific child PID or the first to exit and cleans up.
uint64_t proc::syscall_waitpid(regstate *regs) {
    pid_t pid = regs->reg_rdi;
    if (pid < 0 || pid >= NPROC) {
        log_printf("[syscall_waitpid] Invalid pid=%d argument!\n", pid);
        return E_INVAL;
    }
    int* status = reinterpret_cast<int*>(regs->reg_rsi);
    int options = regs->reg_rdx;

    if (WAITPID_PARANOIA >= 1 && this->id_ != 1) {
        log_printf("[waitpid] WAITPID CALLED. Parent (caller) pid: %d, arg_pid: %d, status: %p, options: %d\n",
            this->id_, pid, status, options);
    }

    int ind_found = -1;
    {
    spinlock_guard guard(ptable_lock);
    for (int i = 0; i < this->nchildren_; ++i) {
        if (this->childpids_[i] == pid) {
            ind_found = i;
            break;
        }
    }

    // If the waitpid is invalid, return immediately.
    if ((this->nchildren_ == 0) || (ind_found == -1 && pid != 0)) {
        if (WAITPID_PARANOIA && this->id_ != 1) {
            log_printf("[waitpid] E_CHILD | parent (caller) pid: %d, arg_pid: %d, num_children: %d, ind_found: %d\n",
                this->id_, pid, this->nchildren_, ind_found);
        }
        return E_CHILD;
    }
    }
    
    if (pid != 0) { // If a pid is specified then search for it.
        if (!options) { // Block
            proc* child = ptable[pid];
            if (USING_PSEUDO_BLOCKING) {
                log_printf("[waitpid] [PSEUDOBLOCK] PID=%d waiting on process PID=%d\n",
                    id_, pid);
                while (true) {
                    {
                    spinlock_guard guard(ptable_lock);
                    if (child->pstate_ == ps_blank) {
                        break;
                    }
                    }
                    yield();
                }
                log_printf("[waitpid] [PSEUDOBLOCK]  Child process PID=%d exited\n", pid);
            }
            else {
                {
                    spinlock_guard guard(ptable_lock);
                    waiter().block_until(parent_child_queue, [&] () {
                        return (child->pstate_ == ps_blank);
                    }, guard);
                }
            }

            uint64_t retpid = (child->retval << 32) + child->id_;
            if (WAITPID_PARANOIA && this->id_ != 1) {
                log_printf("[waitpid] REAP INIT child PID=%d for parent PID=%d\n",
                    pid, this->id_);
                log_printf("[waitpid] PTABLE ENTRY for child struct proc=%p with retval %d\n", 
                    child, child->retval);
            }
            {
                spinlock_guard guard(ptable_lock);
                release_child(this, pid);
                kfree(child);
                ptable[pid] = nullptr;
                if (WAITPID_PARANOIA >= 1) {
                    log_printf("[waitpid] REAPED ZOMBIE PID=%d\n", pid);
                }
            }
            return retpid;
        } else { // Poll
            proc* child = ptable[pid];
            if (child->pstate_ != ps_blank) { // Child is not free. Return error: try again.
                if (WAITPID_PARANOIA >= 1 && this->id_ != 1) {
                    log_printf("[waitpid] TRY AGAIN: poll failed on ppid %d, cpid %d\n",
                        this->id_, pid);
                }
                return E_AGAIN;
            } else { // Child is free! Release and return.
                uint64_t retpid = (child->retval << 32) + child->id_;
                {
                    spinlock_guard guard(ptable_lock);
                    release_child(this, pid);
                    kfree(child);
                    ptable[pid] = nullptr;
                    if (WAITPID_PARANOIA >= 1) {
                        log_printf("[waitpid] REAPED ZOMBIE PID=%d\n", pid);
                    }
                }
                return retpid;
            }
        }
    } 
    else { // If no pid is specified then walk through to find the first free one.
        if (!options) { // Block
            int child_ind = -1;
            if (USING_PSEUDO_BLOCKING) {
                log_printf("[waitpid] [PSEUDOBLOCK] PID=%d waiting on first process to exit\n",
                    id_, pid);
                while (true) {
                    {
                    spinlock_guard guard(ptable_lock);
                    for (int i = 0; i < this->nchildren_; ++i) {
                        if (WAITPID_PARANOIA && this->id_ != 1) {
                            log_printf("[waitpid] SEARCHING child PID=%d for parent PID=%d\n",
                                this->childpids_[i], this->id_);
                        }
                        if (ptable[this->childpids_[i]]->pstate_ == ps_blank) {
                            child_ind = i;
                            if (WAITPID_PARANOIA && this->id_ != 1) {
                                log_printf("[waitpid] FOUND EXITED child PID=%d for parent PID=%d\n",
                                    this->childpids_[i], this->id_);
                            }
                            break;
                        }
                    }

                    if (child_ind != -1) {
                        log_printf("[waitpid] [PSEUDOBLOCK] Child process PID=%d exited\n", 
                            childpids_[child_ind]);
                        break;
                    }
                    if (WAITPID_PARANOIA && this->id_ != 1) {
                        log_printf("[waitpid] Did not find any exited children this time, yielding\n");
                    }
                    }
                    yield();
                }
            }

            else {
                {
                spinlock_guard guard(ptable_lock);
                waiter().block_until(parent_child_queue, [&] () {
                    for (int i = 0; i < this->nchildren_; ++i) {
                        if (WAITPID_PARANOIA && this->id_ != 1) {
                            log_printf("[waitpid] SEARCHING child PID=%d for parent PID=%d\n",
                                this->childpids_[i], this->id_);
                        }
                        if (ptable[this->childpids_[i]]->pstate_ == ps_blank) {
                            child_ind = i;
                            if (WAITPID_PARANOIA && this->id_ != 1) {
                                log_printf("[waitpid] FOUND EXITED child PID=%d for parent PID=%d\n",
                                    this->childpids_[i], this->id_);
                            }
                            break;
                        }
                    }
                    return (child_ind != -1);
                }, guard);
                }
            }

            proc* child = ptable[this->childpids_[child_ind]];
            if (WAITPID_PARANOIA && this->id_ != 1) {
                log_printf("[waitpid] REAP INIT child PID=%d (at ind=%d) for parent PID=%d\n",
                    this->childpids_[child_ind], child_ind, this->id_);
                log_printf("[waitpid] PTABLE ENTRY for child struct proc=%p with retval %d and cpid %d\n", 
                    child, child->retval, child->id_);
            }
            uint64_t retpid = (child->retval << 32) + child->id_;
            {
                spinlock_guard guard(ptable_lock);
                if (WAITPID_PARANOIA >= 1) {
                    log_printf("[waitpid] REAPING ZOMBIE, PID=%d\n", child->id_);
                }
                release_child(this, child->id_);
                ptable[child->id_] = nullptr;
                kfree(child);
                if (WAITPID_PARANOIA >= 1) {
                    log_printf("[waitpid] REAPED ZOMBIE\n");
                }
            }
            return retpid;

        } else { // Poll
            int child_ind = -1;
            {
            spinlock_guard guard(ptable_lock);
            for (int i = 0; i < this->nchildren_; ++i) {
                if (WAITPID_PARANOIA && this->id_ != 1) {
                    log_printf("[waitpid] SEARCHING child PID=%d for parent PID=%d\n",
                        this->childpids_[i], this->id_);
                }
                if (ptable[this->childpids_[i]]->pstate_ == ps_blank) {
                    child_ind = i;
                    if (WAITPID_PARANOIA && this->id_ != 1) {
                        log_printf("[waitpid] FOUND EXITED child PID=%d for parent PID=%d\n",
                            this->childpids_[i], this->id_);
                    }
                    break;
                }
            }
            }

            if (child_ind == -1) { // No exited processes found
                if (WAITPID_PARANOIA >= 1) {
                    log_printf("[waitpid] TRY AGAIN | WAITPID arg_pid: %d, num children: %d, child_ind: %d\n",
                        pid, this->nchildren_, child_ind);
                }
                return E_AGAIN;
            } else { // Exited process found! Release and return.
                proc* child = ptable[this->childpids_[child_ind]];
                if (WAITPID_PARANOIA && this->id_ != 1) {
                    log_printf("[waitpid] REAP INIT child PID=%d (at ind=%d) for parent PID=%d\n",
                        this->childpids_[child_ind], child_ind, this->id_);
                    log_printf("[waitpid] PTABLE ENTRY for child struct proc=%p with retval %d\n", 
                        child, child->retval);
                }
                uint64_t retpid = (child->retval << 32) + child->id_;
                {
                    spinlock_guard guard(ptable_lock);
                    if (WAITPID_PARANOIA >= 1) {
                        log_printf("[waitpid] REAPING ZOMBIE | WAITPID arg_pid: %d, num children: %d, cpid: %d, cind: %d\n",
                            pid, this->nchildren_, child->id_, child_ind);
                    }
                    release_child(this, child->id_);
                    ptable[child->id_] = nullptr;
                    kfree(child);
                    if (WAITPID_PARANOIA >= 1) {
                        log_printf("[waitpid] REAPED ZOMBIE\n");
                    }
                }
                if (WAITPID_PARANOIA >= 1) {
                    log_printf("[waitpid] retpid=%lu, pid=%lu, status=%lu\n",
                        retpid, retpid & 0xFFFFFFFF, retpid >> 32);
                }
                return retpid;
            }
        }
    }
}


// proc::find_open_fd()
//    Returns first available open file descriptor, i.e. null entry.
//    Can exclude one fd if necessary (e.g. if it were temporarily taken but not yet assigned).
//    If exclude_one is turned on then taken_fd must be passed in.
//    Returns E_MFILE if none available.
int proc::find_open_fd(bool exclude_one, int taken_fd=0) {
    // TODO [MULTITH] DO NOT LOCK THIS! Lock places that call this, as lock usually
    // TODO needed until after mem alloc successful.
    for (int fd = 3; fd < MAX_FD; ++fd) {
        if (!fdtable[fd] && (!exclude_one || fd != taken_fd)) {
            return fd;
        }
    }
    return E_MFILE;
}


// proc::show_fdtable_()
//   Prints state of the fdtable
void proc::show_fdtable_() {
    // TODO lock this function
    log_printf("[show_fdtable_] Process PID=%d fdtable HEAD --> [", id_);
    for (int fd = 0; fd < MAX_FD-1; ++fd) {
        log_printf("%d:%s | ", fd, fdtable[fd] ? "T" : "F"); // T is taken, F is free
    }
    log_printf("%d:%s] <-- TAIL\n", MAX_FD-1, fdtable[MAX_FD-1] ? "T" : "F");
}


// IO_invalid(p, start, end, check_writable)
//    Helper function that ensures read/write is valid. end is noninclusive.
//    Returns nonzero if invalid, due to bad memory or address integer overflow.
static int IO_invalid(proc* p, uintptr_t start, uintptr_t end, bool check_writable=false) {
    // Integer overflow check.
    if (end < start) {
        if (VFS_PARANOIA >= 2) {
            log_printf("[IO_invalid] Invalid I/O addr, range causes integer overflow\n");
        }
        return E_FAULT;
    }

    // Permission range check.
    for (vmiter it(p, start); it.va() < end; it.next()) {
        if (!(it.present() && it.user())) {
            if (VFS_PARANOIA >= 2) {
                log_printf("[IO_invalid] Invalid I/O addr, not user-accessible memory\n");
            }
            return E_FAULT;
        }
        if (check_writable && !it.writable()) {
            if (VFS_PARANOIA >= 2) {
                log_printf("[IO_invalid] Invalid read-to addr, not writeable memory\n");
            }
            return E_FAULT;
        }
    }
    if (VFS_PARANOIA >= 3) {
        log_printf("[IO_invalid] Validated addr 0x%x of sz %lu\n", start, end-start);
    }
    return 0;
}


// filename_invalid(pathname)
//    Returns 0 if a path name of valid length and in accessible memory. Fails otherwise.
static int pathname_invalid(proc* p, const char* pathname) {
    if (VFS_MF_PARANOIA >= 2) {
        log_printf("[pathname_invalid] Validating pathname\n");
    }
    if (!pathname) { // nullptr check
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[pathname_invalid] Error: nullptr passed in as path name\n");
        }
        return E_FAULT;
    } else if (!pathname[0]) { // empty string check
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[pathname_invalid] Error: empty string passed in as path name\n");
        }
        return E_FAULT;
    }
    uintptr_t pos = 0;

    // Generate the size, checking memory as we go. // TODO
    vmiter it(p, reinterpret_cast<uintptr_t>(pathname));
    if (!(it.present() && it.user())) { // Special case for first char
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[pathname_invalid] Invalid filename, entire string not in user-accessible memory\n");
        }
        return E_FAULT;
    }
    for (char* c = (char*) pathname; 
        *c && pos <= MAX_FILENAME_LEN; 
        ++c, ++pos) {
        it += 1;
        if (!(it.present() && it.user())) {
            if (VFS_MF_PARANOIA >= 1) {
                log_printf("[pathname_invalid] Invalid filename, part of string not in user-accessible memory\n");
            }
            return E_FAULT;
        }
    }

    if (pos >= MAX_FILENAME_LEN) {
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[pathname_invalid] Invalid file name: too long. Ensure buf ptr is correct\n");
        }
        return E_NAMETOOLONG;
    }

    if (VFS_MF_PARANOIA >= 2) {
        log_printf("[pathname_invalid] Filename '%s' validated\n", pathname);
    }
    return 0;
}


// proc::syscall_open(regs)
//    Opens a file and returns the file descriptor, or an error.
int proc::syscall_open(regstate* regs) {
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_open] Open called\n");
    }

    // First, validate arguments.
    const char* pathname = reinterpret_cast<const char*>(regs->reg_rdi);
    if (pathname_invalid(this, pathname)) { // Check filename
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_open] Invalid pathname\n");
        }
        return E_FAULT;
    }
    int flags = regs->reg_rsi;
    bool create = (flags & OF_CREAT);
    bool trunc = (flags & OF_TRUNC);
    int mode = flags & (OF_RDWR);
    if (!mode) { // Check for null read/write mode flag
        return E_INVAL;
    } else if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_open] Read? %s; Write? %s, Create? %s\n", 
            (mode & OF_READ) ? "Yes" : "No", 
            (mode & OF_WRITE) ? "Yes" : "No", 
            create? "Yes" : "No");
    }

    // Attempt to open the file.
    // TODO [MULTITH] lock access to fdtable until red comment.
    long fd = find_open_fd(false);
    if (fd == E_MFILE) { // Handle no open fdtable entries
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_open] No open fdtable entry: fd=%d\n", fd);
        }
        return E_MFILE;
    } else if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_open] Found valid fd=%d for new open\n", fd);
    }

    memfile_vnode* mf_vn = knew<memfile_vnode>(mode, nullptr); // Set memfile ptr after fs lookup
    if (!mf_vn) { // Error!
        if (VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_open] Failed to k-alloc memfile vnode\n");
        }
        return E_NOMEM;
    }
    int initfs_index = memfile::initfs_lookup(pathname, create);
    if (initfs_index < 0) { // Error code was returned
        return initfs_index;
    }
    assert(initfs_index < (int) memfile::namesize);
    mf_vn->set_mf(memfile::initfs + initfs_index); // Set memfile* ptr in vnode
    ++mf_vn->refcount_;
    fdtable[fd] = reinterpret_cast<vnode*>(mf_vn);
    //! Only here can we unlock fdtable access, since we know that we secured a node alloc.

    if (trunc) { // Truncate if requested
        int retstat = memfile::initfs[initfs_index].set_length(0);
        assert(retstat == 0);
    }

    if (VFS_MF_PARANOIA >= 2) {
        log_printf("[syscall_open] Open successful\n");
        show_fdtable_();
    }

    return fd;
}


// proc::syscall_dup2(regs)
//    Copies vnodes from a file descriptor to another. Returns new fd if successful.
int proc::syscall_dup2(regstate* regs) {
    // TODO [MULTITH] lock fdtable accesses
    int oldfd = regs->reg_rdi;
    int newfd = regs->reg_rsi;
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_dup2] Dup2 called by process PID=%d. old=%d, new=%d\n",
            id_, oldfd, newfd);
    }
    if (oldfd < 0 || oldfd >= MAX_FD || newfd < 0 || newfd >= MAX_FD) { // Invalid fd
        return E_BADF;
    } else if (!fdtable[oldfd]) { // Non-open old fd
        return E_BADF;
    } 

    // Edge case: the fd's are the same. Do nothing.
    if (oldfd == newfd) {
        return newfd;
    }

    if (fdtable[newfd]) { // Close newfd if open
            if (VFS_PARANOIA >= 2) {
                log_printf("[syscall_dup2] newfd=%d is open, closing\n", newfd);
            }
        if (!fdtable[newfd]->close()) { // If refcount hits 0
            if (VFS_PARANOIA >= 1) {
                log_printf("[syscall_dup2] Closed node at newfd=%d empty, freeing\n", newfd);
            }
            delete fdtable[newfd];
        }
    }

    // Set the old fd vnode ptr to the new one and increment refcount.
    fdtable[newfd] = fdtable[oldfd];
    ++fdtable[newfd]->refcount_;
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_dup2] Dup2 done, showing new fdtable state:\n");
        show_fdtable_();
    }
    return 0;
}


// proc::syscall_pipe(regs)
//    Creates a read and write pipe and writes fd's into a single long as rfd | (wfd << 32).
uintptr_t proc::syscall_pipe(regstate* regs) {
    // Examine the fd table and ensure that there are at least 2 available spots.
    // TODO [MULTITH] lock fdtable accesses
    if (PIPE_PARANOIA >= 2) {
        log_printf("[syscall_pipe] Pipe called by process PID=%d\n", id_);
    }
    long rfd, wfd;
    pipe_vnode *wr_vn = nullptr, *rd_vn = nullptr;

    rfd = find_open_fd(false);
    if (rfd == E_MFILE) {
        if (PIPE_PARANOIA >= 2) {
            log_printf("[syscall_pipe] No open READ entry: rfd=%d\n", rfd);
        }
        return E_MFILE;
    }
    wfd = find_open_fd(true, rfd);
    if (wfd == E_MFILE) {
        if (PIPE_PARANOIA >= 2) {
            log_printf("[syscall_pipe] No open WRITE entry: rfd=%d, wfd=5d\n", rfd, wfd);
        }
        return E_MFILE;
    }
    if (PIPE_PARANOIA >= 2) {
        log_printf("[syscall_pipe] Successfully found file descriptors READ=%d, WRITE=%d\n",
            rfd, wfd);
    }

    // Create a write node, get the allocated bbuf, and create the read node.
    if (PIPE_PARANOIA >= 2) {
        log_printf("[syscall_pipe] PID=%d allocating a new WRITE pipe\n", id_);
    }
    wr_vn = knew<pipe_vnode>(OF_WRITE, nullptr);

    // Perform checks on allocation.
    if (!wr_vn) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[syscall_pipe] Failed to allocate a new WRITE pipe vnode, returning to user\n");
        }
        goto emem;
    }
    if (!wr_vn->bbuf_) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[syscall_pipe] Syscall detected failed allocation of pipe bbuf, returning to user\n");
        }
        goto free_wr;
    }

    if (PIPE_PARANOIA >= 2) {
        log_printf("[syscall_pipe] PID=%d allocating a new READ pipe\n", id_);
    }
    rd_vn = knew<pipe_vnode>(OF_READ, wr_vn->get_bbuf());
    if (!rd_vn) {
        if (PIPE_PARANOIA >= 1) {
            log_printf("[syscall_pipe] Failed to allocate a new READ pipe vnode, returning to user\n");
        }
        goto free_bbuf;
    }

    // At this points all allocations have been successfully made.
    // TODO [MULTITH] lock accesses here.
    fdtable[wfd] = reinterpret_cast<vnode*>(wr_vn);
    fdtable[rfd] = reinterpret_cast<vnode*>(rd_vn);
    ++fdtable[wfd]->refcount_;
    ++fdtable[rfd]->refcount_;

    if (PIPE_PARANOIA >= 2) {
        log_printf("[syscall_pipe] Successfully made pipe, updated state below\n");
        show_fdtable_();
    }

    return rfd | (wfd << 32); // concatenated fd, see title comment of function

    free_bbuf:
        delete wr_vn->bbuf_;
        wr_vn->bbuf_ = nullptr;
    free_wr:
        delete wr_vn;
    emem:
        return E_NOMEM;
}


// proc::syscall_read(regs), proc::syscall_write(regs),
//    Handle read and write system calls.
uintptr_t proc::syscall_read(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();
    int fd = regs->reg_rdi;
    // TODO [MULTITH] lock ftable access
    if (fd < 0 || fd >= MAX_FD || !fdtable[fd]) {
        if (VFS_KBC_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_read] fd %d invalid or not open\n", fd);
        }
        return E_BADF;
    }
    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;

    // Validate the read buffer.
    if (IO_invalid(this, addr, addr+sz, true)) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_read] INVALID read request for PID=%d, fd=%d\n", id_, fd);
        }
        return E_FAULT;
    }

    // Read from open file.
    // TODO [MULTITH] lock ftable access
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_read] NEW read request for PID=%d, fd=%d\n", id_, fd);
    }
    return fdtable[fd]->read(addr, sz);
}


uintptr_t proc::syscall_write(regstate* regs) {
    // This is a slow system call, so allow interrupts by default
    sti();

    int fd = regs->reg_rdi;
    // TODO [MULTITH] lock ftable access
    if (fd < 0 || fd >= MAX_FD || !fdtable[fd]) {
        if (VFS_KBC_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_write] fd %d invalid or not open\n", fd);
        }
        return E_BADF;
    }
    uintptr_t addr = regs->reg_rsi;
    size_t sz = regs->reg_rdx;

    // Validate the write buffer.
    if (IO_invalid(this, addr, addr+sz)) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_write] INVALID write request for PID=%d, fd=%d\n", id_, fd);
        }
        return E_FAULT;
    }

    // Write to the file.
    // TODO [MULTITH] lock ftable access
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_write] NEW write request for PID=%d, fd=%d\n", id_, fd);
    }
    return fdtable[fd]->write(addr, sz);
}


// proc::syscall_close(regs)
//    Closes a file descriptor, freeing if necessary.
int proc::syscall_close(regstate* regs) {
    // TODO [MULTITH] lock ftable access
    int fd = regs->reg_rdi;
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_close] Closing fd=%d for process PID=%d\n", fd, id_);
    }
    if (fd < 0 || fd >= MAX_FD || !fdtable[fd]) {
        if (VFS_PARANOIA >= 2) {
            log_printf("[syscall_close] fd=%d invalid, returning\n", fd);
        }
        return E_BADF;
    }
    if (!fdtable[fd]->close()) { // If refcount hits 0
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_close] Closed node at fd=%d empty, freeing\n", fd);
        }
        delete fdtable[fd];
    }
    fdtable[fd] = nullptr;
    if (VFS_PARANOIA >= 2) {
        log_printf("[syscall_close] Close successful, new state:\n");
        show_fdtable_();
    }
    return 0;
}


// proc::show_argv()
//    Prints argv. Don't call unless you know it's valid or for debugging.
void proc::show_argv(int argc, const char** argv) {
    log_printf("argv --> [");
    for (int i = 0; i < argc-1; ++i) {
        log_printf("'%s', ", argv[i]);
    }
    log_printf("'%s']\n", argv[argc-1]);
}

// argv_invalid(argc, argv)
//    Checks whether a argv for execution is valid or not. 
//    Returns negative error code upon detecting invalid string, and total length otherwise.
int proc::argv_invalid(int argc, const char** argv) {
    if (VFS_PARANOIA >= 2) {
        log_printf("[argv_invalid] Validating argv below with argc=%d\n", argc);
        show_argv(argc, argv);
    }
    if (!argv || argc < 1 || argc >= (int) MAX_ARGV_LEN) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[argv_invalid] Invalid (null) argv or invalid argc\n");
        }
        return E_FAULT;
    }

    int nchars = 0; // Total num chars read
    int nstrs = 0; // Num strings (elements of argv) read
    for (int i = 0; i < argc && nchars <= (int) MAX_ARGV_LEN; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            break;
        }
        int arg_invalid = pathname_invalid(this, arg);
        if (arg_invalid) {
            return arg_invalid;
        }
        ++nstrs;
        nchars += strlen(arg) + 1; // Add 1 for null terminator
    }
    if (nchars >= (int) MAX_ARGV_LEN) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[argv_invalid] Too many args and/or arg too long. Total length must be below %d\n",
                MAX_ARGV_LEN);
        }
        return E_2BIG;
    }
    if (nstrs != argc) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[argv_invalid] Number of strings in argv does not match argc\n");
        }
        return E_INVAL;
    }
    if (VFS_PARANOIA >= 2) {
        log_printf("[argv_invalid] argv of total length %d validated\n", nchars);
    }
    return nchars;
}


// proc::copy_argv(stkpg_kptr, argc, argv)
//    Copies the arguments into a stack page passed in as a kptr. Returns UVA of new argv.
//    NOTE: Assumes argv has been validated and the stkpg has been mapped in to user proc.
uintptr_t proc::copy_argv(x86_64_pagetable* pt, void* stkpg_uptr, int argc, 
    const char** argv, int total_length) {
    vmiter it(pt, reinterpret_cast<uintptr_t>(stkpg_uptr));
    uintptr_t stkpg_pa = it.pa(); // Physical addr
    uintptr_t stkpg_uva = it.va(); // User virtual addr
    void* stkpg_kptr = pa2kptr<void*>(stkpg_pa);
    if (VFS_PARANOIA >= 2) {
        log_printf("[copy_argv] Copying argv below to stack page KVA=%p, UVA=%p, PA=0x%x. Total length: %d\n",
            stkpg_kptr, stkpg_uptr, stkpg_pa, total_length);
        show_argv(argc, argv);
    }
    assert(stkpg_uptr == reinterpret_cast<void*>(stkpg_uva));

    // Start the copy off from the top of the stack minus the total length 
    // of all the arguments in the array when concatenated.
    uintptr_t stkpg_top_kva = reinterpret_cast<uintptr_t>(stkpg_kptr) + PAGESIZE;
    uintptr_t cpy_kva = stkpg_top_kva - total_length;
    uintptr_t cpy_uva = stkpg_uva + PAGESIZE - total_length; // Addresses in the new argv must be user-level
    assert(stkpg_top_kva - cpy_kva < MAX_ARGV_LEN);

    // One by one, copy in the strings (which have been assumed to be validated)
    // and increment cpy_kva. At the same time, build the pointers into an array.
    uintptr_t argv_arr_kva = cpy_kva - sizeof(char*)*(argc+1); // +1 for last element nullptr
    uintptr_t argv_arr_uva = stkpg_uva + PAGESIZE - (stkpg_top_kva - argv_arr_kva);
    uintptr_t next_arg_kva = argv_arr_kva;
    for (int i = 0; i < argc; ++i) {
        char* arg = (char*) argv[i];
        int arglen = strlen(arg) + 1; // Must include null terminator

        // Copy in the string, and copy in the corresponding user-virtual address to an array.
        memcpy(reinterpret_cast<void*>(cpy_kva), reinterpret_cast<void*>(arg), arglen);
        memcpy(reinterpret_cast<void*>(next_arg_kva), &cpy_uva, sizeof(char*));

        next_arg_kva += sizeof(char*);
        cpy_kva += arglen;
        cpy_uva += arglen;
        assert(cpy_kva <= stkpg_top_kva);
        assert(cpy_uva <= stkpg_uva + PAGESIZE);
    }
    memset(reinterpret_cast<void*>(next_arg_kva), 0, sizeof(char*));
    next_arg_kva += sizeof(char*);

    // Run some tests.
    if (VFS_PARANOIA >= 3) {
        assert(cpy_kva == stkpg_top_kva);
        assert(cpy_uva == stkpg_uva + PAGESIZE);
        assert(next_arg_kva == stkpg_top_kva - total_length);
        next_arg_kva = argv_arr_kva;
        log_printf("[TEST] [copy_argv] NEW char** argv=0x%x is an array of UVA ptrs --> [", 
            argv_arr_kva);
        for (int i = 0; i < argc-1; ++i) {
            log_printf("0x%x, ", next_arg_kva);
            next_arg_kva += sizeof(char*);
        }
        log_printf("0x%x]\n", next_arg_kva);
        log_printf("[TEST] [copy_argv] Corresponding strings pointed to by argv: {");
        char** str_arr = reinterpret_cast<char**>(argv_arr_kva);
        for (int i = 0; i < argc-1; ++i) {
            log_printf("%s, ", pa2kptr<char*>(vmiter(pt, (uintptr_t) str_arr[i]).pa()));
        }
        log_printf("%s}\n", pa2kptr<char*>(vmiter(pt, (uintptr_t) str_arr[argc-1]).pa()));
        log_printf("[TEST] [copy_argv] new argv arr KVA=%p, UVA=%p, PA=%p\n",
            (void*) argv_arr_kva, (void*) argv_arr_uva, ka2pa(argv_arr_kva));
    }

    if (VFS_PARANOIA >= 2) {
        log_printf("[copy_argv] argv copy successful\n");
    }
    return argv_arr_uva;
}


// proc::syscall_execv(regs)
//    Replaces current process image with a fresh binary given in args.
int proc::syscall_execv(regstate* regs) {
    if (VFS_PARANOIA >= 2 || VFS_MF_PARANOIA >= 2) {
        log_printf("[syscall_execv] Execv called by process PID=%d\n", id_);
    }
    const char* prgm_name = reinterpret_cast<const char*>(regs->reg_rdi);
    const char** argv = reinterpret_cast<const char**>(regs->reg_rsi);
    int argc = regs->reg_rdx;
    
    // Validate pathname.
    if (pathname_invalid(this, prgm_name)) { // Check filename
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_execv] Invalid program name\n");
        }
        return E_FAULT;
    }

    // Validate argv and argc.
    int total_length = argv_invalid(argc, argv);
    assert(total_length);
    if (total_length < 0) {
        if (VFS_PARANOIA >= 1) {
            log_printf("[syscall_execv] Invalid argv/argc\n");
        }
        return total_length;
    }

    // Look up the process name in memfiles.
    int initfs_index = memfile::initfs_lookup(prgm_name);
    if (initfs_index < 0) { // Error code was returned
        // NOTE: Kernel logs done in the lookup function.
        return initfs_index;
    }
    assert(initfs_index < (int) memfile::namesize);

    // Allocate a new pagetable and stack page.
    x86_64_pagetable* pt = kalloc_pagetable();
    if (!pt) {
        if (VFS_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_execv] Failed to allocate a new pagetable\n");
        }
        return E_NOMEM;
    } else if (VFS_PARANOIA >= 2 || VFS_MF_PARANOIA >= 2) {
        log_printf("[syscall_execv] New pt alloc at KVA=%p, PA=0x%x\n",
            pt, kptr2pa(pt));
    }
    void* stkpg = kalloc(PAGESIZE);
    if (!stkpg) {
        if (VFS_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_execv] Failed to allocate a new stack page\n");
        }
        kfree(pt);
        return E_NOMEM;
    } else if (VFS_PARANOIA >= 2 || VFS_MF_PARANOIA >= 2) {
        log_printf("[syscall_execv] New stack page alloc at KVA=%p, PA=0x%x\n",
            stkpg, kptr2pa(stkpg));
    }

    // Load the process.
    memfile_loader mld(initfs_index, pt);
    int load_error = load(mld);
    if (load_error) { // Restore and return error upon load failure
        if (VFS_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_execv] Failed to load new process\n");
        }
        kfree(pt);
        kfree(stkpg);
        free_auto_allocs(pt); // Free any allocations made by loader
        return load_error;
    } else if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] Process successfully loaded\n",
            pt, kptr2pa(pt));
    }

    // Map stack page and the console to pt. 
    // (See boot_process_start for a more detailed explanation.)
    int stk_map_error = vmiter(pt, MEMSIZE_VIRTUAL-PAGESIZE).try_map(stkpg, PTE_PWU);
    int cons_map_error = vmiter(pt, CONSOLE_ADDR).try_map(CONSOLE_ADDR, PTE_PWU);
    if (stk_map_error || cons_map_error) {
        if (VFS_PARANOIA >= 1 || VFS_MF_PARANOIA >= 1) {
            log_printf("[syscall_execv] Failed to map stack and/or console to new pt\n");
        }
        free_auto_allocs(pt); // Free any allocations made by loader
        kfree(pt);
        if (stk_map_error) {
            kfree(stkpg); // If map didn't work then free_auto_allocs() won't free stack pg.
        }
        return E_NOMEM;
    } else if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] Stack / Console successfully mapped to UVA=0x%x / 0x%x\n",
            MEMSIZE_VIRTUAL-PAGESIZE, CONSOLE_ADDR);
    }

    // Copy in the argv and argc.
    uintptr_t new_argv_uva = copy_argv(pt, (void*) (MEMSIZE_VIRTUAL-PAGESIZE), argc, argv, total_length);
    assert(new_argv_uva);

    // At this point, the system call will succeeed. The next line stomps on regstate.
    // Initialize a new set of registers for the process.
    x86_64_pagetable* old_pt = pagetable_; // Store old pt before init user clears it out
    if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] Old pagetable %p stored\n", old_pt);
    }
    init_user(id_, pt); // Install pt, reset regs_
    if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] Process regs_ reset to initialized values\n");
    }

    // Set instruction and stack ptr regs.
    regs_->reg_rip = mld.entry_rip_;
    // %rsp starts at the argv array in user-level memory.
    regs_->reg_rsp = new_argv_uva;
    if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] \%rsp set to 0x%x, \%rip set to 0x%x\n", 
            regs_->reg_rsp, mld.entry_rip_);
        char** start = pa2kptr<char**>(vmiter(pt, regs_->reg_rsp).pa());
        log_printf("[syscall_execv] \%rsi=%p points to array with first string='%s'\n",
            start, pa2kptr<char**>(vmiter(pt, (uintptr_t) start[0]).pa()));
    }

    // Set functional argument regs to argc and the NEW argv pointer in the stack page.
    regs_->reg_rdi = argc;
    regs_->reg_rsi = regs_->reg_rsp;
    if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] set \%rdi=%lu and \%rsi=0x%x\n", 
            regs_->reg_rdi, regs_->reg_rsi);
    } 

    // Set the pagetable and free the old one.
    set_pagetable(pt);
    free_auto_allocs(old_pt);
    kfree(old_pt);
    if (VFS_PARANOIA >= 3 || VFS_MF_PARANOIA >= 3) {
        log_printf("[syscall_execv] New pagetable %p set, old pagetable %p and mem freed\n",
            pagetable_, old_pt);
        log_printf("[syscall_execv] Execv setup complete, yielding\n");
    }
    
    // yield_noreturn() so the scheduler treats resume() like a regstate
    // and uses regs_ in the resumption state register set inseead of a yieldstate.
    yield_noreturn();
}

// proc::syscall_readdiskfile(regs)
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