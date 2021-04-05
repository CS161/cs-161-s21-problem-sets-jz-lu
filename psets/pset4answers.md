CS 161 Problem Set 4 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset4collab.md`.

Answers to written questions
----------------------------
**IMPORTANT**: most of everything below is just documentation, useful if something in the code seems cryptic. The only parts possibly worth reading for its own sake is the synchronization plan in *Part B* and of course, all of the extra credit in the grading notes.

### Part A

#### Eviction
Algorithm: LRU. Implementation scheme: Use a queue, `bufcache::evictq_` holding `bcentry` pointers with zero refcount. When refcount goes up in `bufcache::get_disk_entry()`, pop the entry index off of the queue, and when it goes to zero, push it onto the (back of the) queue. Thus the front of the queue will always hold the least recently used with zero refcount, and can be evicted in `O(1)` time. Popping an index can be done by `erase()` via the Chickadee list structure which it also `O(1)` time. The superblock shall never be evicted. We allow the free block bitmap to be evicted, though it is generally unlikely that it will make it to the front of the queue before another extension is needed. One additional eviction policy has been implemented; see the note after *Prefetching*. 

**N.B.** If the cache is full, and no eviction can be made, then any function discussed will return error or `nullptr`, depending on the return type of the function. As such henceforth in this document we will not explicitly discuss the case of a full cache.

#### Prefetching
Design: prefetch the first `nfetch = 8` (or as many as available) blocks whenever `open(), lseek(), read(), write()` are called. For the former two, include the block containing the current offset; for the latter two, start prefetching one block ahead of the current offset, since the current block is already being fetched normally. Implementation: we added a `read_or_write_nonblocking()` function to `struct ahcistate` which mimics everything about the driver function `read_or_write`, except that (a) it simply returns error if no slot is available and (b) after `issue_ncq()` is called it returns without waiting for it to finish. We moreover added an atomic flag `bcentry::std::atomic<int> pfstatus_` initialized to `E_AGAIN`; when the SATA disk completes a fetch, it will set this status to something other than `E_AGAIN`, so we will use this flag as a signal in the algorithm below. The method of prefetching follows the below steps.

1. A new function `bufcache::prefetch()` loops through each block number to prefetch. (It locks the cache inside the loop only, since prefetching is low-priority so other processes should be able to squeeze in between iterations if they need to grab a block from the cache.) For each block, it searches for an available entry in the cache, evicting if necessary. 
2. Once a spot has been found, `bufcache::prefetch()` calls `bcentry::prefetch_block()` on the allocated entry, locking the entry before and unlocking immediately after return (see *Part B* for synchronization plan and lock ordering). `prefetch_block()` is a nonblocking version of `bcentry::load()`, calling the nonblocking version of `ahcistate::read()` which has an additional argument `r` that is passed by reference to the NCQ to be set to something other than `E_AGAIN` upon completion of fetch. `bcentry::prefetch_block()` resets `bcentry::pfstatus_` to `E_AGAIN` and passes it in. Importantly, `prefetch_block()`, which is called under lock, sets the `estate_` of the block to a newly added state `es_prefetching` and pushes it onto a newly added `bufcache::pfq_` prefetching queue. An invariant of the queue is that every entry on it must have state `es_prefetching`. This queue is used for eviction (see Note below).
3. Suppose at some point a process registers interest in a prefetched block with block number `b` by calling `get_disk_entry()` on `b`. If a prefetcher has already been called on `b` (if not then the function works as usual), then it will already appear in the cache. We modified `get_disk_entry()` to block, in the case that the block is already in the cache with `estate_ = es_prefetching`, until `pfstatus_ != E_AGAIN` (it may not block at all, if the fetch was already done). Once the disk completes the write, signaling via setting of `pfstatus_`, the waiting process wakes, sets `estate_ = es_clean`, and pops the entry off of the prefetch queue.
4. Suppose no process registers interest in `b` for a sufficiently long time (i.e. before eviction--see note below). Then the block will be evicted according to the prefetch block eviction policy outlined below.

**Note**: following the completion of prefetching, we added an additional eviction policy. If the eviction queue is empty, `bufcache::evict()` will instead try to evict a block that has finished prefetched but no one has reigstered interest in. This is implemented by looping through `bufcache::pfq_` and evicting the first block satisfying `e->pfstatus_ != E_AGAIN`, indicating prefetch was complete. There is an inherent race condition with the disk, but not a harmful one, since the worst that cna happen is that while looping through the queue the disk finished the fetch on a block after it has already been iterated through, so we may end up missing an evictable block, but this does not have any major problems (more on this in the comments of `evict()`).

## Part B
Synchronization plan: TODO FIX talk about the basic inode locks, and then about serializing vnode offset changes to prevent parent/child/sibling processes from making offset ludicrous (i.e. serialize reads within a vnode) using the same per-vnode lock `vnode::open_close_lock_`. If multiple processes sharing a `vnode` (e.g. parent-child) with separate `fdtable`s read at the same time, it is up to the processes to synchronize, as while `offset_` is atomic, the entire function will not be locked, so if not synchronized the processes could be reading the same thing twice (this is not a problem with writing, since the inode is locked, but the r/w lock allows multiple readers at the same time).

Invariant changes: 
1. The `bcentry::buf_` is no longer constant after writing, but may not be modified without holding the `bcentry` write reference via `bcentry::get_write()`. 
2. When a kernel task holds a reference to a bufcache entry, the state must satisfy `buf_ != nullptr` and `estate_ == es_clean || es_dirty`. 
3. Just as one may check whether the state is `es_empty` without entry lock, one may also do so with `es_prefetching`. The queue for prefetching (either completed or not, but no process has yet to ask for it) is protected by the `bufcache::lock_`, as are all the other queues/lists in `bufcache`. 
4. Reading an (initialized) inode type does not require any locking.

**Lock ordering**: every subset of locks must obey the following order invariant. Directory `inode`s are to be locked first, followed by file `inode` locking. Each layer of locks is called in order of `lock_write()/lock_read()`, bufcache spinlocks, entry spinlocks, and then `inode->entry()->get_write()` for writing (for reading, the entry has no read reference and thus there is no ambiguity).

Subdirectory locking strategy: every subdirectory will be protected by the root directory lock, and every file will be protected by its parent directory lock, with the exception that unlinked files are protected by the root directory. New allocations of inodes are protected by the root lock. Under this paradigm, one can check that there are no modification races, and although two processes can race to create/delete something, only one will succeed and there is no undefined behavior. However, it is difficult to prevent races on searching the entire directory tree, and thus calls to `lookup_directory()` must be protected by *either* the `fdtable` lock (to be implemented in pset 5--thus any calls to directory lookups in syscall functions at the moment are not yet locked), or the global root directory r/w lock.

Note: The only function which may be called without lock is `lookup_directory(inode)`. 

Grading notes
-------------

**Extra Credit**: ()
1. Parallel writes: TODO
2. `sys_unlink`: TODO
3. `sys_rename`: TODO
4. Subdirectory support: TODO
5. `sys_mkdir`: TODO
6. `sys_rm`: TODO
7. `sys_ftruncate`: TODO
8. Support for `/dev/null`: TODO
9. Support for `/dev/random`: TODO
10. Support for `/dev/zero`: Writing to it will do nothing (returns 0).
11. Support for `/dev/full`: Reading `sz` bytes from it will provide a stream of `sz` 0 bytes; writing to it will always return `E_NOSPC`. Useful for testing/simulating writes to a full disk.
12. Support for a per-process *current working directory*: TODO
13. `sys_pwd`: TODO
14. `sys_cd`: TODO
15. Lots of things you can now do in the shell! We added programs `mkdir, ` and `rm`, which will actually deduce . As we know from CS 61 PSet 5, `cd, pwd` are special because we can't just spawn a child to call `sys_cd`; for those we directory modified `p-sh.cc` to allow for changeing and printing the directory, following the lead from the driver code's implementation of the shell command `exit`. Try it out! See example below.
```
mkdir /mickens/
cd /mickens/
pwd
cat thoreau.txt > javascript_sux.txt
cat javascript_sux.txt
rm javascript_sux.txt
cd /
pwd
rm /mickens/
```

