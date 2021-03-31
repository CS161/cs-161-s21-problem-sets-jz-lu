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

Invariant changes: `bcentry` lock will also protect the `is_linked` boolean for unlinking. The `bcentry::buf_` is no longer constant after writing, but may not be modified without holding the `bcentry` write reference via `bcentry::get_write()`. When a kernel task holds a reference to a bufcache entry, the state must satisfy `buf_ != nullptr` and `estate_ == es_clean || es_dirty`. In addition to protecting `ref_` and `estate_`, the `bcentry::lock_` will also protect a metadatum `linked`, which flags if at least one file has the `inode` open in memory. Just as one may check whether the state is `es_empty` without entry lock, one may also do so with `es_prefetching`. The queue for prefetching (either completed or not, but no process has yet to ask for it) is protected by the `bufcache::lock_`, as are all the other queues/lists in `bufcache`. Reading an (initialized) inode type does not require any locking.

Lock ordering: directory `inode`s are to be locked first, followed by file `inode` locking. Each layer of locks is called in order of `lock_write()/lock_read()`, bufcache spinlocks, entry spinlocks, and then `inode->entry()->get_write()` for writing (for reading, the entry has no read reference and thus there is no ambiguity).

Subdirectory locking strategy: `lock_write()` on the parent of the new subdirectory when making a new subdirectory. Lock the subdirectory of a new file when creating. Lock the parent when deleting a subdirectory.

Note: The only function which may be called without lock is `lookup_directory()`, which traverses the directory. It is left to the user to not delete directories in one thread while another traverses the directories. (Racing here will not cause undefined behavior, but may not return the expected behavior depending on the order of the traversals/deletes/creates, which is why we leave it to the user to understand what they want and lock accordingly.)

Grading notes
-------------
