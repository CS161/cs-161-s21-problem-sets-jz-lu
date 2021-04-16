#include "kernel.hh"
#include "k-vmiter.hh"
#include "k-ahci.hh"
#include "k-chkfs.hh"

// k-memviewer.cc
//
//    The `memusage` class tracks memory usage by walking page tables,
//    looks for errors, and prints the memory map to the console.


class memusage {
  public:
    // tracks physical addresses in the range [0, maxpa)
    static constexpr uintptr_t maxpa = 1024 * PAGESIZE;
    // shows physical addresses in the range [0, max_view_pa)
    static constexpr uintptr_t max_view_pa = 512 * PAGESIZE;
    // shows virtual addresses in the range [0, max_view_va)
    static constexpr uintptr_t max_view_va = 768 * PAGESIZE;

    memusage()
        : v_(nullptr) {
    }

    // Flag bits for memory types:
    static constexpr unsigned f_kernel = 1;     // kernel-restricted
    static constexpr unsigned f_user = 2;       // user-accessible
    // `f_process(pid)` is for memory associated with process `pid`
    static constexpr unsigned f_process(int pid) {
        if (pid >= 30) {
            return 2U << 31;
        } else if (pid >= 1) {
            return 2U << pid;
        } else {
            return 0;
        }
    }
    // Pages such as process page tables and `struct proc` are counted
    // both as kernel-only and process-associated.


    // Refresh the memory map from current state
    void refresh();

    // Return the symbol (character & color) associated with `pa`
    uint16_t symbol_at(uintptr_t pa) const;

  private:
    unsigned* v_;

    // add `flags` to the page containing `pa`
    // This is safe to call even if `pa >= maxpa`.
    void mark(uintptr_t pa, unsigned flags) {
        if (pa < maxpa) {
            v_[pa / PAGESIZE] |= flags;
        }
    }
    // return one of the processes set in a mark
    static int marked_pid(unsigned v) {
        return lsb(v >> 2);
    }
    // print an error about a page table
    void page_error(uintptr_t pa, const char* desc, int pid) const;
};


// memusage::refresh()
//    Calculate the current physical usage map, using the current process
//    table.

void memusage::refresh() {
    if (!v_) {
        // Kernel allocates a page to hold the flags of the pages.
        v_ = reinterpret_cast<unsigned*>(kalloc(PAGESIZE));
        assert(v_ != nullptr);
    }
    memset(v_, 0, (maxpa / PAGESIZE) * sizeof(*v_));
    mark(ka2pa(v_), f_kernel);

    // Mark the ahcistate for the SATA disk allocated in kernel_start()
    for (int pg = 0; pg < (1 << (order(sizeof(ahcistate)) - MIN_ORDER)); ++pg) {
        mark(ka2pa(sata_disk) + pg*PAGESIZE, f_kernel);
    }

    // Mark the buffer cache entries if they are allocated.
    bufcache& bc = bufcache::get();
    for (size_t i = 0; i < bc.ne; ++i) {
        if (bc.e_[i].buf_) {
            mark(ka2pa(bc.e_[i].buf_), f_kernel);
        }
    }

    for (auto i = 0; i < ncpu; ++i) {
        mark(ka2pa(cpus[i].idle_task_), f_kernel);
    }

    // mark kernel ranges of physical memory
    // We handle reserved ranges of physical memory separately.
    for (auto range = physical_ranges.begin();
         range != physical_ranges.end();
         ++range) {
        if (range->type() == mem_kernel) {
            for (uintptr_t pa = range->first();
                 pa != range->last();
                 pa += PAGESIZE) {
                mark(pa, f_kernel);
            }
        }
    }

    // walk process page tables
    assert(ptable_lock.is_locked());
    for (int pid = 1; pid < NPROC; ++pid) {
        proc* p = ptable[pid];
        if (p) {
            mark(ka2pa(p), f_kernel | f_process(pid));
            if (p->pwd_) { // mark pwd allocation
                mark(kptr2pa(p->pwd_), f_kernel | f_process(pid));
            }
            if (p->thgrp_) { // mark thread group allocation
                mark(kptr2pa(p->thgrp_), f_kernel | f_process(pid));
            }

            auto irqs = p->lock_pagetable_read();
            if (p->pagetable_ && p->pagetable_ != early_pagetable) {
                for (ptiter it(p); it.low(); it.next()) {
                    mark(it.pa(), f_kernel | f_process(pid));
                }
                mark(ka2pa(p->pagetable_), f_kernel | f_process(pid));

                for (vmiter it(p); it.low(); ) {
                    if (it.user()) {
                        mark(it.pa(), f_user | f_process(pid));
                        it.next();
                    } else {
                        it.next_range();
                    }
                }
            }
            p->unlock_pagetable_read(irqs);
        }
    }
}

void memusage::page_error(uintptr_t pa, const char* desc, int pid) const {
    const char* fmt = pid >= 0
        ? "PAGE TABLE ERROR: %lx: %s (pid %d)\n"
        : "PAGE TABLE ERROR: %lx: %s\n";
    error_printf(CPOS(22, 0), COLOR_ERROR, fmt, pa, desc, pid);
    log_printf(fmt, pa, desc, pid);
}

uint16_t memusage::symbol_at(uintptr_t pa) const {
    auto range = physical_ranges.find(pa);
    if (range == physical_ranges.end()
        || (pa >= maxpa && range->type() == mem_available)) {
        return '?' | 0xF000;
    }

    if (pa >= maxpa) {
        if (range->type() == mem_kernel) {
            return 'K' | 0x4000;
        } else {
            return '?' | 0x4000;
        }
    }

    auto v = v_[pa / PAGESIZE];
    if (range->type() == mem_console) {
        return 'C' | 0x4F00;
    } else if (range->type() == mem_reserved) {
        return 'R' | (v ? 0xC000 : 0x4000);
    } else if (range->type() == mem_kernel) {
        return 'K' | (v > f_kernel ? 0xCD00 : 0x4D00);
    } else if (range->type() == mem_nonexistent) {
        return ' ' | 0x0700;
    } else {
        if (v == 0) {
            return '.' | 0x0700;
        } else if (v == f_kernel) {
            return 'K' | 0x4000;
        } else if ((v & f_kernel) && (v & f_user)) {
            // kernel-restricted + user-accessible = error
            page_error(pa, "sharing error, kernel-restricted + user-accessible\n",
                       marked_pid(v));
            return 'E' | 0xF400;
        } else {
            // find lowest process involved with this page
            pid_t pid = marked_pid(v);
            // foreground color is that associated with `pid`
            static const uint8_t colors[] = { 0xF, 0xC, 0xA, 0x9, 0xE };
            uint16_t ch = colors[pid % 5] << 8;
            if (v & f_kernel) {
                // kernel page: dark red background
                ch |= 0x4000;
            }
            if (v > (f_process(pid) | f_kernel | f_user)) {
                // shared page
                ch = (ch & 0x7700) | 'S';
            } else {
                // non-shared page
                static const char names[] = "K123456789ABCDEFGHIJKLMNOPQRST??";
                ch |= names[pid];
            }
            return ch;
        }
    }
}


static void console_memviewer_virtual(memusage& mu, proc* vmp) {
    console_printf(CPOS(10, 26), 0x0F00,
                   "VIRTUAL ADDRESS SPACE FOR %d\n", vmp->id_);

    for (vmiter it(vmp);
         it.va() < memusage::max_view_va;
         it += PAGESIZE) {
        unsigned long pn = it.va() / PAGESIZE;
        if (pn % 64 == 0) {
            console_printf(CPOS(11 + pn / 64, 3), 0x0F00,
                           "0x%06X ", it.va());
        }
        uint16_t ch;
        if (!it.present()) {
            ch = ' ';
        } else {
            ch = mu.symbol_at(it.pa());
            if (it.user()) { // switch foreground & background colors
                uint16_t z = (ch & 0x0F00) ^ ((ch & 0xF000) >> 4);
                ch ^= z | (z << 4);
            }
        }
        console[CPOS(11 + pn/64, 12 + pn%64)] = ch;
    }
}


void console_memviewer(proc* vmp) {
    // track physical memory
    static memusage mu;
    mu.refresh();
    // must be called with `ptable_lock` held

    // print physical memory
    console_printf(CPOS(0, 32), 0x0F00,
                   "PHYSICAL MEMORY                  @%lu\n",
                   ticks.load());

    for (int pn = 0; pn * PAGESIZE < memusage::max_view_pa; ++pn) {
        if (pn % 64 == 0) {
            console_printf(CPOS(1 + pn/64, 3), 0x0F00, "0x%06X", pn << 12);
        }
        console[CPOS(1 + pn/64, 12 + pn%64)] = mu.symbol_at(pn * PAGESIZE);
    }

    // print virtual memory
    bool need_clear = true;
    if (vmp) {
        auto irqs = vmp->lock_pagetable_read();
        if (vmp->pagetable_ && vmp->pagetable_ != early_pagetable) {
            console_memviewer_virtual(mu, vmp);
            need_clear = false;
        }
        vmp->unlock_pagetable_read(irqs);
    }
    if (need_clear) {
        console_printf(CPOS(10, 0), 0x0F00, "\n\n\n\n\n\n\n\n\n\n");
    }
}

void console_fdviewer(proc* fdp, const char* buf) {
    const int DELAY = 100;
    static proc* last_showing = nullptr;
    static int delay = DELAY;
    --delay;
    if (fdp != last_showing) {
        // console_clear();
        last_showing = fdp;
    }
    // track physical memory
    static memusage mu;
    mu.refresh();
    // must be called with `ptable_lock` held

    // print physical memory
    console_printf(CPOS(0, 32), 0x0F00,
                   "PHYSICAL MEMORY                  @%lu\n",
                   ticks.load());

    for (int pn = 0; pn * PAGESIZE < memusage::max_view_pa; ++pn) {
        if (pn % 64 == 0) {
            console_printf(CPOS(1 + pn/64, 3), 0x0F00, "0x%06X", pn << 12);
        }
        console[CPOS(1 + pn/64, 12 + pn%64)] = mu.symbol_at(pn * PAGESIZE);
    }

    // print virtual memory
    bool need_clear = true;
    if (fdp) {
        console_printf(CPOS(10, 33), 0x0F00,
                   "VFS MAP FOR %d\n", fdp->id_);
        console_printf(CPOS(11, UI_DEEPCENTER-3), 0xf00, "Modes = {R: read, W: write}\n");
        console_printf(CPOS(12, UI_CENTER-1), 0xf00, 
            "Types = {C: console, P: pipe, M: mfile, D: disk file, S: special dev}\n");
        if (fdp->pagetable_ && fdp->pagetable_ != early_pagetable) {
            console_printf(CPOS(15, UI_CENTER), 0xF00, buf);
            need_clear = false;
        }
        console_printf(CPOS(18, UI_DEEPCENTER+4), 0xd00, "Key: (mode@type)\n");
    }
    console_printf(CPOS(23, 0), 0xB00, "Words of wisdom, courtesy of Aakash \"Big Ka$h\" Mishra:\n");
    if (delay <= 0) {
        delay = DELAY;
        int rn = rand(0, 20);
        switch (rn) {
        case 0:
            console_printf(CPOS(24, 0), 0xd00, "\"Wait, Anaconda isnt an Eminem song?\"\n");
            break;

        case 1:
            console_printf(CPOS(24, 0), 0xd00, "\"There are more dollar signs in my LaTeX file than in my income.\"\n");
            break;
        
        case 2:
            console_printf(CPOS(24, 0), 0xd00, "\"I mean, my mind is like a treasure\"\n");
            break;
        
        case 3:
            console_printf(CPOS(24, 0), 0xd00, "\"Yeah I dont use social media I just gain validation through Ed.\"\n");
            break;
        
        case 4:
            console_printf(CPOS(24, 0), 0xd00, "\"PEOPLE FALL IN LOVE IN MYSTERIOUS WAYYYYSS, MAYBE ITS ALL PART OF A PLANNNNNNNNNN\"\n");
            break;
        
        case 5:
            console_printf(CPOS(24, 0), 0xd00, "\"You know how when you put your finger in and you expect it to be firm but then it’s all soft and squishy and you’re just like *ugh*…\"\n");
            break;
        
        case 6:
            console_printf(CPOS(24, 0), 0xd00, "\"I just want you to know...that I have 100%% credibility! At all times!\"\n");
            break;
        
        case 7:
            console_printf(CPOS(24, 0), 0xd00, "\"My doctor thinks Im an anti-vaxxer.\"\n");
            break;
        
        case 8:
            console_printf(CPOS(24, 0), 0xd00, "\"No, YOURE a category error!\"\n");
            break;

        case 9:
            console_printf(CPOS(24, 0), 0xd00, "\"Sometimes my genius even manages to amaze me.\"\n");
            break;
        
        case 10:
            console_printf(CPOS(24, 0), 0xd00, "\"Oh you said sick? I thought you said thicc and I was like yeahhhh\"\n");
            break;
        
        case 11:
            console_printf(CPOS(24, 0), 0xd00, "\"Variance is more apparent in this picture. Its like if you pack the variance into a burrito\"\n");
            break;
        
        case 12:
            console_printf(CPOS(24, 0), 0xd00, "\"If I were a superhero, my name would be P-A-C man!...wait thats pac man.\"\n");
            break;
        
        case 13:
            console_printf(CPOS(24, 0), 0xd00, "*Humming softly* \"kill em with sadness, kill em with pity\"\n");
            break;
        
        case 14:
            console_printf(CPOS(24, 0), 0xd00, "\"Its not really networking if most of my network is family right\"\n");
            break;
        
        case 15:
            console_printf(CPOS(24, 0), 0xd00, "\"I pity those in finals clubs, but maybe its just my extreme antisocial abilities\"\n");
            break;
        
        case 16:
            console_printf(CPOS(24, 0), 0xd00, "\"I have never been a nerd. I am ultimate non-nerd.\"\n");
            break;
        
        case 17:
            console_printf(CPOS(24, 0), 0xd00, "\"My next meeting? My next meeting is with FOOD...when Im HUNGRY\"\n");
            break;
        
        case 18:
            console_printf(CPOS(24, 0), 0xd00, "\"I dont talk like you! I talk more like whats hangin bro yeee yeee\"\n");
            break;

        case 19:
            console_printf(CPOS(24, 0), 0xd00, "\"This is just...the perfect banana. I feel kind of like a monkey right now.\"\n");
            break;

        case 20:
            console_printf(CPOS(24, 0), 0xd00, "\"Criticizing me?? Everyone should be apologizing to me!\"\n");
            break;
        
        default:
            break;
        }
    }
    if (need_clear) {
        console_printf(CPOS(10, 0), 0x0F00, "\n\n\n\n\n\n\n\n\n\n");
    }
}
