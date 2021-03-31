CS 161 Problem Set 4 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset4collab.md`.

Answers to written questions
----------------------------
### Part A
Eviction strategy: LRU. Use a queue of cache entry indices with zero refcount. When refcount goes up in `bufcache::get_disk_entry()`, pop the entry index off of the queue, and when it goes to zero, push it onto the (back of the) queue. Thus the front of the queue will always hold the least recently used with zero refcount, and can be evicted in `O(1)` time. Popping an index can be done by `erase()` which it also `O(1)` time. TODO FIX

## Part B
Synchronization plan: TODO FIX talk about the basic inode locks, and then about serializing vnode offset changes to prevent parent/child/sibling processes from making offset ludicrous (i.e. serialize reads within a vnode) using the same per-vnode lock `vnode::open_close_lock_`.

Invariant changes: 
1. The `bcentry::buf_` is no longer constant after writing, but may not be modified without holding the `bcentry` write reference via `bcentry::get_write()`. 
2. When a kernel task holds a reference to a bufcache entry, the state must satisfy `buf_ != nullptr` and `estate_ == es_clean || es_dirty`. 
3. Just as one may check whether the state is `es_empty` without entry lock, one may also do so with `es_prefetching`. The queue for prefetching (either completed or not, but no process has yet to ask for it) is protected by the `bufcache::lock_`, as are all the other queues/lists in `bufcache`. 
4. Reading an (initialized) inode type does not require any locking.

**Lock ordering**: every subset of locks must obey the following order invariant. Directory `inode`s are to be locked first, followed by file `inode` locking. Each layer of locks is called in order of `lock_write()/lock_read()`, bufcache spinlocks, entry spinlocks, and then `inode->entry()->get_write()` for writing (for reading, the entry has no read reference and thus there is no ambiguity).

Subdirectory locking strategy: every subdirectory and file but the root directory has a "parent", which is the directory that holds the file/subdirectory as a `direntry`. The parent r/w lock shall be used to protect files. *Exception*: an unlinked but still open file does not have a corresponding directory entry, so it will be protected via lock by the root directory's lock. Under this paradigm, one can check that there are no modification races, and although two processes can race to create/delete something, only one will succeed and there is no undefined behavior. However, it is difficult to prevent races on searching the entire directory tree, and thus calls to `lookup_directory()` must be protected by *either* the `fdtable` lock (to be implemented in pset 5--thus any calls to directory lookups in syscall functions at the moment are not yet locked), or the global root dir3ectory r/w lock.

Note: The only function which may be called without lock is `lookup_directory(inode)`. 

Grading notes
-------------

**Extra Credit**: (I got carried away with this pset...)
1. Parallel writes: TODO
2. `sys_unlink`: TODO
3. `sys_rename`: TODO
4. Subdirectory support: TODO
5. `sys_mkdir`: TODO
6. `sys_rm`: TODO
7. `sys_ftruncate`: TODO
8. Support for `/dev/null`: TODO
9. Support for `/dev/random`: TODO
10. Support for `/dev/zero`: TODO
11. Support for `/dev/full`: TODO

