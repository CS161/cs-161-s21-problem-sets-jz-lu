CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------
### Part A
1. `kalloc` allocates memory up to 1 `PAGESIZE = 4096 bytes`.
2. [CHECK IN OH] Experimentally determined to be `ptr = 0xffff800000001000`; this is 1 page up from the start of high canonical memory, where the physical memory map is allocated (which is consistent with the boot process of Chickadee where the physical memory map is the first allocation by direct linear translation to high canonical memory). In `kernel.hh` the function `init_physical_ranges()` reserves the zero page and then sets the permissions of the lower canonical and upper canonical regions, so by the direct linear mapping of the physical memory table the first allocation begins 1 page up since that is the first allocatable page. 
3. The max is `0xffff8000001ff000`, after which `kalloc()` returns `nullptr`. We should expect this since this is an offset 1 page size smaller than `MEMSIZE_PHYSICAL = 0x200000`.
4. The address types are high canonical, as they are of the form `0xffff8000xxxxxxxx`. The line `ptr = pa2kptr<void*>(next_free_pa)` in `kalloc()` calls a function that returns `pa2ka(pa)`, which by the documentation returns the high canonical address of the PA.
5. [CHECK IN OH] We could change `MEMSIZE_PHYSICAL = 0x200000` to say `300000`, i.e. `#define MEMSIZE_PHYSICAL 0x300000`. Then the max `kalloc()` goes becomes ``.
6. [CHECK IN OH--DO WE KEEP THE LOOP IN THE CODE?] We switch to the following:
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
7. `find()` is a linear time search that walks through the pages starting from address 0 until one has been found. On the other hand, `type()` calls `find()`, and because our loop must call `type()` at every step (otherwise, there is no way to know whether the current page is generically available), it is a quadratic time search. Moreover, in the original loop using `find()`, a failure to find a range that is not of the right type will cause the program to jump to the next range instead of the next page, which makes the search much more efficient since there are at most `maxsize = 16` range blocks, skipping on average many pages which we know are of the same type.

8. `page_lock` is essentially a mutex that prevents a race condition in which multiple kernel processes attempt to allocate at the same time, which could without the lock result in the same page being given to 2 processes, an obvious disaster.


### Part B
1. 


Grading notes
-------------
