CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------
### Part A
1. `kalloc` allocates memory up to 1 `PAGESIZE = 4096 bytes`.
2. Experimentally determined to be `ptr = 0xffff800000001000`; this is 1 page up from the start of high canonical memory, where the physical memory map is allocated (which is consistent with the boot process of Chickadee where the physical memory map is the first allocation by direct linear translation to high canonical memory). In `kernel.hh` the function `init_physical_ranges()` reserves the zero page (for `nullptr`) and then sets the permissions of the lower canonical and upper canonical regions, so by the direct linear mapping of the physical memory table the first allocation begins 1 page up since that is the first allocatable page. 
3. The max is `0xffff8000001ff000`, after which `kalloc()` returns `nullptr`. We should expect this since this is an offset 1 page size smaller than `MEMSIZE_PHYSICAL = 0x200000`.
4. The address types are high canonical, as they are of the form `0xffff8000xxxxxxxx`. The line `ptr = pa2kptr<void*>(next_free_pa)` in `kalloc()` calls a function that returns `pa2ka(pa)`, which by the documentation returns the high canonical address of the PA.
5. We could change `MEMSIZE_PHYSICAL = 0x200000` to say `300000`, i.e. `#define MEMSIZE_PHYSICAL 0x300000`, or equivalently in `k-init.cc` change the same constant in `physical_ranges.set(0, MEMSIZE_PHYSICAL, mem_available)`. Then the max `kalloc()` goes to just short of `0x300000`, due to some reserved pages and 300000 being the `MEMSIZE_VIRTUAL`.
6. We switch to the following:
```
while (next_free_pa < physical_ranges.limit() && 
        physical_ranges.type(next_free_pa) != mem_available) {
        next_free_pa += PAGESIZE;    
}

if (next_free_pa < physical_ranges.limit()) {
    ptr = pa2kptr<void*>(next_free_pa);
    next_free_pa += PAGESIZE;
}
```
7. `find()` is a linear time search that walks through the pages starting from address 0 until one has been found. On the other hand, `type()` calls `find()`, and because our loop must call `type()` at every step (otherwise, there is no way to know whether the current page is generically available), it is a quadratic time search. Moreover, in the original loop using `find()`, a failure to find a range that is not of the right type will cause the program to jump to the next range instead of the next page, which makes the search much more efficient since there are at most `maxsize = 16` range blocks, skipping on average many pages which we know are of the same type. To measure the difference we counted the number of times the loop executed in ~980 (plus/minus 10) ticks of the OS clock, with the idea that the slower loop, needing to call `find()` more often, would execute less loop iterations. Indeed, in that time the `find()` method executed 145 times while `type()` did 5 times, so it performed about 29 times faster.
8. `page_lock` is essentially a mutex that prevents a race condition in which multiple kernel processes attempt to allocate at the same time, which could without the lock result in the same page being given to 2 processes, an obvious disaster.


### Part B
1. Line `86` in `memusage::refresh()`, which is `mark(pa, f_kernel)`.
2. Line `96` which is `mark(ka2pa(p), f_kernel | f_process(pid))` where `p` is the address (once converted) of the `proc`.
3. A user/unprivileged process should never interact with the physical pages since its addresses are all virtual addresses that the kernel gives it; therefore it needs to know whether a virtual page is accessible or not to it. The kernel on the other hand is at the interface of physical pages and needs to know whether a physical page is restricted just for it, or if it can be mapped into virtual memory by a pagetable for unprivileged use. If `ptiter` marked pages as user-accessible, it could pose a security concern since a process could, for lower virtual addresses, obtain access to an actual physical page that it should not.
4. It should be the type `mem_available = 1` since it is a loop over process memory (low canonical) allocation, and these processes (which are not of the kernel) should only have access to the unprivileged memory blocks. THe high canonical memory was initialized when the process was initialized, and the only reserved page in low canonical is the zero page, which was filtered out in `if (p) ...`.
5. Switching to `it += PAGESIZE`, which overloads `+=` in iterator context to increment the VA pointed to by `it` by `PAGESIZE`, does not have a very noticeable difference in the rate of memory allocations on QEMU UI (it might be marginally slower but we couldn't tell). To explain this, we logged the jumps in `.next()`, which by the documentation moves page by page skipping large non-present regions. 
A typical pattern of physical addresses followed something like
```
49152, 53248, 77824, 81920, 86016, 90112, 94208, 98304, 102400, 106496, 110592, 114688, 118784, 122880, 126976, 131072
```
We observe that most of the jumps are in fact just one page (a diff of 4096 in the decimal address). Thus it is not surprising that changing it to a fixed 1-page jump does not make a significant difference in the allocation speed. The exception to this nearly 1-page jumps is the jump from `643072, 647168, 651264, 1466368, 1470464, 1474560, 1478656`, but since this is the only major jump it does not cause a considerable difference.
6. For `NCPU = X` the number of missed pages is `X+1`, so there is 1 page missed for every CPU Qemu uses, plus 1. `X` pages are allocated in `cpustate::init_idle_task()` in `k-cpu.cc`. The purpose is to have an "idle task" process (and a process needs an allocation) that does nothing, to be run when the CPU is just waiting for an interrupt. 1 page is allocated by `memviewer::refresh`itself at the beginning, when it calls `kalloc()` at `v_ = reinterpret_cast<unsigned*>(kalloc(PAGESIZE))` to store the flags/states of each of the physical pages. Both of these are allocations made by the kernel but not for kernel text, which are currently not tracked by the marker (which tracks kernel text pages and user process pages).
7. Done. We marked, as kernel memory, each of the `proc* idle_task_` addresses (translated to pa) stored in the `cpustate cpus[MAXCPU]` array in `k-cpu.cc`, and marked the address of the `v_` flag array in `refresh()` immediately after its allocation and memset.


### Part C
The following are the 7 entry points.
1. Jumps to `kernel_start()` in line 35, `jmp _Z12kernel_startPKc`, of `k-exception.S`. Purpose: initialize hardware and boot processes upon startup. Called upon boot. The assembly allocates a stack size by moving the stack pointer before jump.
2. Calls `proc::exception(regstate*)` in line 143, `call _ZN4proc9exceptionEP8regstate` of `k-exception.S`. Purpose: exception handler for kernel that identifies the exception and updates the state of the kernel. Called by `k-exception.S` assembly when a processor raises an exception. The stack is the kernel task stack which is jumped to before calling.
3. Calls `cpustate::init_ap()` in `k-init.cc`, allocating a new CPU stack beforehand via the following.
```
leaq CPUSTACK_SIZE(%rdi), %rsp
// call `cpus[my_CPU_number].init_ap()`.
// This two-stage jump switches to high virtual addresses.
movabsq $_ZN8cpustate7init_apEv, %rbx
jmp *%rbx // ENTRY PT
```
This is done once per CPU to initialize the CPU state.
4. Calls `proc::syscall(regstate*)` in `kernel.cc` in line 228 `call _ZN4proc7syscallEP8regstate`. Purpose: handles system calls by classifying the call and calling the functions necessary from there. Called whenever a user process raises a syscall.
5. Calls `cpustate::schedule()` in via `jmp _ZN8cpustate8scheduleEP4proc`. Purpose: called to run the next process during things like a yield. A proc stack is allocated at the beginning of the entry prior to the call.
6. `boot` in `bootentry.S`. Called while switching CPU out of compatibility mode in `boot.cc` during bootup. The jump is static, in the line `ljmp    $SEGSEL_BOOT_CODE, $boot`. The stack is set up at the very beginning in `boot_start`.
7. `idle()` in `k-cpu.cc`. Called when there are no processes left to run, or when there are more CPUs asked for than `MAXCPU`, which `k-exception.S` handles by `jge ap_entry_failed` where `ap_entry_failed` is the imbedding of the `idle()` function (which is just an assembly inline) of doing `hlt` and then `jmp` forever. The allocation of the stack is done in `init_kernel()` by adding a `PROCSTACK_SIZE` to `%rsp`. That is called from `init_idle_task()`, called in `schedule()` the first time a CPU is initialized. Idling occurs in `resume()` when appropriate.

### Part D
Note that we used green dollar signs to decorate our console.

### Part E
Nothing to write here.

### Part F
Our nasty alloc recursively allocates a lot of local memory in an array. The canary is asserted after most system calls are made (except for things like `getpid` which we assert beforehand). It is not perfect in catching overflow, but it does detect our nasty alloc.

We added the flag `Wstack-usage=4096` (this is quite large but it was for demonstrative purposes vis a vis `sys_nasty()` and can be tuned down to detect unintended overflows). This produced a compiler warning, so it did successfully catch the overflow as the error was static.
```
kernel.cc:435:5: warning: stack usage is 12848 bytes [-Wstack-usage=]
  435 | int proc::syscall_nasty(regstate* regs) {
      |     ^~~~
```

*Extra credit*: `fstack-usage` also detects the problem. The file generated `kernel.su` gives
```
// stuff...
kernel.cc:435:5:int proc::syscall_nasty(regstate*)	12848	static
kernel.cc:452:5:int proc::syscall_testkalloc(regstate*)	16	static
kernel.cc:474:11:uintptr_t proc::syscall_read(regstate*)	48	static
kernel.cc:522:11:uintptr_t proc::syscall_write(regstate*)	48	static
// stuff...
```
which clearly detects the stack being far too large. If we change the allocation to a recursive loop style, then the `static` flag will change to `dynamic`. 

### Part G
Some high-level notes on the design of our buddy allocator. Our struct of information is as follows:
```
struct bapg {
    bool free = false; // whether the bapg is free or not
    bool available = false; // if mem_available is set
    bool returned = false; // if the address of this page has been returned in kalloc()
    uint64_t ord; // order of the block
    uint64_t r_ord; // order of the root block of the current block
    uintptr_t r_addr; // address of the root block of the current block
};

bapg pgmap[MEMSIZE_PHYSICAL/PAGESIZE]
```
The root block is the original block that has no buddy. To intialize, we walk through each range in `physical_ranges`. We iteratively compute the largest block of size `2**ord` that fits in the range, and set the relevant metadata. Allocating is done by walking through the lowest-order nonempty free list and breaking down blocks if needed, and freeing is done by computing buddy address, checking if free, and if so adjoining the blocks and repeating until no free buiddy is found, at which time the block is pushed onto the relevant free list.

*Note*: the buddy allocator revealed an additional 4 pages of memory being allocated by the kernel that are not marked. A backtrace showed that an allocation in `ahcistate::find()` was made in `kernel_start()`, which has initially been returned a `nullptr` by the driver `kalloc`ator due to allocation being larger than a page. We marked these in `memviewer::refresh()` so again no pages are left unmarked.

#### Buddy allocator test cases
Our test cases all involve some form of deterministic or random allocation and free, followed by checking that the metadata is set properly (i.e. all invariants are consistent) via assert statements.
1. Simple allocation: allocate a couple pages, then free them.
2. Random allocations of multiples of `PAGESIZE`.
3. Random allocations of random sizes (not necessarily a multiple of anything), allocating a lot and freeing a lot, repeatedly.
4. Edge cases (wild frees, double frees, unreasonably large allocs, etc.)
Checking on invariants is done in `check_kalloc()` and `check_kfree()`. These are called if the `BALLOC_PARANOIA` constant in `kernel.hh` is turned on.

Grading notes
-------------

**Part F**: for some reason the assertion failure error message appears behind the kernel, but the canary is still working---check `log.txt`.

**Part G (Buddy allocator)**: set the constant `BALLOC_PARANOIA = 1` in `kernel.hh` when running test cases so the invariant checker functions are fired. If you feel that the world is too fast and you have too much free time, set it to `2` for a very massive, very slow text dump of all the allocation steps and status updates (do not recommend).

**Extra credit attempts**: `fstack-usage` (see Part F). 