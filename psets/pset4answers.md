CS 161 Problem Set 4 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset4collab.md`.

Answers to written questions
----------------------------
**IMPORTANT**: most of everything below is just documentation, useful if something in the code seems cryptic. The only parts possibly worth reading for its own sake is the synchronization plan in *Synchronization Invariants* and of course, all of the extra credit in the grading notes.

## Part A

### Eviction
Algorithm: LRU. Implementation scheme: Use a queue, `bufcache::evictq_` holding `bcentry` pointers with zero refcount. When refcount goes up in `bufcache::get_disk_entry()`, pop the entry index off of the queue, and when it goes to zero, push it onto the (back of the) queue. Thus the front of the queue will always hold the least recently used with zero refcount, and can be evicted in `O(1)` time. Popping an index can be done by `erase()` via the Chickadee list structure which it also `O(1)` time. The superblock shall never be evicted. We allow the free block bitmap to be evicted, though it is generally unlikely that it will make it to the front of the queue before another extension is needed. One additional eviction policy has been implemented; see the note after *Prefetching*. 

**N.B.** If the cache is full, and no eviction can be made, then any function discussed will return error or `nullptr`, depending on the return type of the function. As such henceforth in this document we will not explicitly discuss the case of a full cache.

### Prefetching
Design: prefetch the first `nfetch = 8` (or as many as available) blocks whenever `open(), lseek(), read(), write()` are called. For the former two, include the block containing the current offset; for the latter two, start prefetching one block ahead of the current offset, since the current block is already being fetched normally. Implementation: we added a `read_or_write_nonblocking()` function to `struct ahcistate` which mimics everything about the driver function `read_or_write`, except that (a) it simply returns error if no slot is available and (b) after `issue_ncq()` is called it returns without waiting for it to finish. We moreover added an atomic flag `bcentry::std::atomic<int> pfstatus_` initialized to `E_AGAIN`; when the SATA disk completes a fetch, it will set this status to something other than `E_AGAIN`, so we will use this flag as a signal in the algorithm below. The method of prefetching follows the below steps.

1. A new function `bufcache::prefetch()` loops through each block number to prefetch. (It locks the cache inside the loop only, since prefetching is low-priority so other processes should be able to squeeze in between iterations if they need to grab a block from the cache.) For each block, it searches for an available entry in the cache, evicting if necessary. 
2. Once a spot has been found, `bufcache::prefetch()` calls `bcentry::prefetch_block()` on the allocated entry, locking the entry before and unlocking immediately after return (see *Synchronization Invariants* for synchronization plan and lock ordering). `prefetch_block()` is a nonblocking version of `bcentry::load()`, calling the nonblocking version of `ahcistate::read()` which has an additional argument `r` that is passed by reference to the NCQ to be set to something other than `E_AGAIN` upon completion of fetch. `bcentry::prefetch_block()` resets `bcentry::pfstatus_` to `E_AGAIN` and passes it in. Importantly, `prefetch_block()`, which is called under lock, sets the `estate_` of the block to a newly added state `es_prefetching` and pushes it onto a newly added `bufcache::pfq_` prefetching queue. An invariant of the queue is that every entry on it must have state `es_prefetching`. This queue is used for eviction (see Note below).
3. Suppose at some point a process registers interest in a prefetched block with block number `b` by calling `get_disk_entry()` on `b`. If a prefetcher has already been called on `b` (if not then the function works as usual), then it will already appear in the cache. We modified `get_disk_entry()` to block, in the case that the block is already in the cache with `estate_ = es_prefetching`, until `pfstatus_ != E_AGAIN` (it may not block at all, if the fetch was already done). Once the disk completes the write, signaling via setting of `pfstatus_`, the waiting process wakes, sets `estate_ = es_clean`, and pops the entry off of the prefetch queue.
4. Suppose no process registers interest in `b` for a sufficiently long time (i.e. before eviction--see note below). Then the block will be evicted according to the prefetch block eviction policy outlined below.

#### Note: Eviction with Prefetching
Following the completion of prefetching, we added an additional eviction policy. If the eviction queue is empty, `bufcache::evict()` will instead try to evict a block that has finished prefetched but no one has reigstered interest in. This is implemented by looping through `bufcache::pfq_` and evicting the first block satisfying `e->pfstatus_ != E_AGAIN`, indicating prefetch was complete. There is an inherent race condition with the disk, but not a harmful one, since the worst that cna happen is that while looping through the queue the disk finished the fetch on a block after it has already been iterated through, so we may end up missing an evictable block, but this does not have any major problems (more on this in the comments of `evict()`).

## Part B

### `execv`
We added a derived class of `proc_loader` called `diskfile_loader`, holding a `chkfs::inode* ino_` and a `bcentry* curr_pg_`. It includes definitions of parent-overriding functions `get_page()`, which calls `get_disk_entry()` and stores the result in `curr_pg_`, and `put_page()`, which calls `curr_pg_->put()`. In `syscall_execv()`, we first validate the path and grab the associated inode, then callocate a new `diskfile_loader` that is passed into `proc::load`. The rest proceeds as usual, for now (see extra credit, current working directories).

### VFS Linking
We added a new inherited class of `vnode` called `disk_vnode`. Along with the inherited attributes of a per-vnode lock, atomic `offset_`, and `refcount_`, it has a pointer `chkfs::inode* ino_` that holds the disk inode it corresponds to. The constructor stores the inode pointer passed in, and the destructor will call `ino_->put()`. Writing is locked entirely with `ino_->lock_write()` and reading with `ino_->lock_read()`. There is a subtle problem with locking for readingl; see *Synchronization Invariants*.

### Open
Opening a file now involves first validating the path with slight modification to the `pathname_invalid()` helper function to now return with an error code if the path is longer than `chkfs::maxnamelen`. (If you look at the function, you will notice there is also an argument `pfxlen`, which is the length of the prefix. A *prefix* is a section of the full path not passed in by the user but stored in the current working directory. This allows `open()` to work with our implementation of current working directories--see extra credit). The inode is then grabbed if it exists (`disk_vnode::create_file()` is otherwise called; see *Part E*), and a new disk vnode is created to store it. Upon success, a prefetch is made to grab a chunk of the beginning of the file.

## Part C
To mark items are dirty we added a new `estate_` called `es_dirty` as well as a list of dirty entries `bufcache::dirty_list_`, enforcing a new invariant change that dirty blocks cannot be on the eviction queue. It follows that the three queues `dirty_list_, evictq_, pfq_` have empty intersectionl; that is, no entry belongs to more than one of the three lists at a time. We added `bcentry::get_write()` and `bcentry::put_write()`, that, in addition to obtaining a write reference so that only one thread can write at a time to a `bcentry`, pushes the entry onto the dirty list. Putting the write releases the reference. 

### `sync`
Calling `sync()` will lock the `bufcache`, loop through a localized swap copy of the dirty list (see problem set instructions, part C), and obtain a write reference to the entry. Each entry will then write to the SATA disk, blocking each time until the write finishes, then mark the state as clean and pop off of the dirty list. Syncing and writing are correctly synchronized since both are done under the same grabbing of write reference; hence no process may write to a syncing block, and syncing may not begin on a block that is in the midst of a write.

## Part D

### Truncate
Truncating a file is done by obtaining a write reference to a write-locked inode in `syscall_open()`, and setting the size to 0. In general, the `inode::size` attribute keeps track of the size as it appears to the user, but the actual allocated size of the file may be larger. For example truncating a file does not actually reduce its allocated size, so if a user writes to a truncated file it will initially not require a true extension from the disk.

### `sys_lseek`
Seeking is handled by locking the inode for reading, then updaing the `disk_vnode::offset_` according to the origin flag. If `origin == LSEEK_SIZE`, the offset to seek to is irrelevant and is not validated; instead, `ino_->size` is returned.

### File Extension
We implemented the `chkfsstate::allocate_extent(count)` function, which grabs the free block bitmap (FBB) from the disk and searches it for a contiguous range of `count` blocks. This is implemented efficiently using the driver code functions `find_lsb(index)` and `find_lsz(index, nsearch)`, which return the first 1 (free) and 0 (taken) bit in the FBB starting from the given index and searching at most `nsearch` blocks. A contiguous range is found successfully by the following logic:
```c++
idx = find_lsb()dex = 
end = find_lsz(index=idx, nsearch=count)
if (end - idx == count) {
    return idx;
}
// Repeat until found or end of disk is reached.
```
In essence, if the next taken block, given by `find_lsz()` is at least `count` blocks away from the free block index, then that range has been found. We added some bitset view helper functions inspired by the suggested ones in the ChickadeeFS documentation to mark the blocks as taken/free; search for "Bitset helper functions" in `k-chkfs.cc` to see them. After a contiguous range is found, `aloocate_extent()` marks the blocks as taken and returns them. These are done under a write reference to `fbb` in the buffer cache, so there are no race conditions. Similarly, our `free_extent()` function will free allocation if an error happens, or more interestingly for `sys_unlink()` (see relevant section in *Part F*).

To extend a file, we added a checker branch to the top of `disk_vnode::write()`, which walks to the end of the file's allocated size (recall that this may be larger than `inode::size`) and examines if it has sufficient room for the write. If it does not, then it will allocate an extent, using the driver code `chkfsiter` class method `insert()` in `k-chkfsiter.hh/cc` to add it to the vnode's `ino_`. It will then proceed to the write as usual. If there is not enough space, it will write as much as it can, and then return.


## Part E
To create a file, we add the following helper functions. Many of them use a helper-helper function `chkfsstate::ino_to_inum()`, which computes the `inum` of an inode using the inverse function of the `inum -> inode` function that `get_inode()` used in the driver code. (We inverted the function with a bit of algebra and modular arithmetic to build it.)
1. `chkfsstate::allocate_inode(type)`: Grabs the superblock to get the number of inodes and the starting index, then iterates through the inodes by grabbing the inode blocks and iterating thorugh them until a free inode is found. Under lock, it is set to allocated (i.e. `type_ != 0`). For this section, `type = type_regular` will always be the function argument, but this function will be reused to allocate subdirectories later (see *Part F*), during which `type = type_directory` will be used instead.
2. `chkfsstate::free_inode(type)`: Same idea, but it frees the inode if it is allocated. Used for errors, and for `syscall_rm()` later.
3. `chkfsstate::allocate_direntry(dirino)`: Walks through the directory inode given, looking for a free `direntry`; if none are found, `allocate_extent()` and `insert()` are called to get a new extent. THe new `direntry` is initialized properly. In the function a second argument, a `bcentry` pointer that is passed by reference, is also required; this allows the caller to do what they need to do with the `direntry` while holding the entry write reference for that `direntry` bufcache entry, and then put it back afterwards. (In a pythonic world we could return both, but that is hard to do in C++ so we just pass one in by reference.)
4. `chkfsstate::allocate_direntry(dirino)`: same deal, but backwards and a little more subtle. Finds the `direntry` to free, sets `name = '\0', inum = 0` so that other processes can allocate it again.

All of these together are used by a new `static disk_vnode::create_file(pathname)` function, which allocates an inode, grabs it, allocates a direntry, sets the name, and releases all locks and references before returning the new `chkfs::inode*` to the caller. `syscall_open()` then calls `create_file()` when it fails to look up an inode but `OF_CREAT` is included in the open flags.

## Part F
The below gives some basic documentation on the extra credit items we did for this problem set. To test them, see *Extra credit*.

### Unlink


### Rename


### `ftruncate`


### Subdirectories

#### Subdirectory Interface
1. Making a directory: TODO
2. Removing a directory: TODO

### Current Working Directory


### Shell Upgrade


### Special files
We added another derived vnode class `special_vnode` that holds a `type_` variable enumerated over `null, random, zero, full`. In `syscall_open()`, the `pathname` string is first checked to see if it is one of these special files, and if it is, a special vnode is allocated in place of a disk vnode, and initialized with the appropriate type. The full implementaion is in `k-vfs.hh/cc`. In total there are four special files; we added all of the special pseudo-files Linux uses. Their functions are given in *Extra credit*.


## Synchronization Invariants
Synchronization plan FIX talk about the basic inode locks, and then about serializing vnode offset changes to prevent parent/child/sibling processes from making offset ludicrous (i.e. serialize reads within a vnode) using the same per-vnode lock `vnode::open_close_lock_`. If multiple processes sharing a `vnode` (e.g. parent-child) with separate `fdtable`s read at the same time, it is up to the processes to synchronize, as while `offset_` is atomic, the entire function will not be locked, so if not synchronized the processes could be reading the same thing twice (this is not a problem with writing, since the inode is locked, but the r/w lock allows multiple readers at the same time).

Summary of invariant changes: 
1. The `bcentry::buf_` is no longer constant after writing, but may not be modified without holding the `bcentry` write reference via `bcentry::get_write()`. 
2. When a kernel task holds a reference to a bufcache entry, the state must satisfy `buf_ != nullptr` and `estate_ == es_clean || es_dirty`. 
3. Just as one may check whether the state is `es_empty` without entry lock, one may also do so with `es_prefetching`. The queue for prefetching (either completed or not, but no process has yet to ask for it) is protected by the `bufcache::lock_`, as are all the other queues/lists in `bufcache`. 
4. Reading an (initialized) inode type does not require any locking.

**Lock ordering**: every subset of locks must obey the following order invariant. Directory `inode`s are to be locked first, followed by file `inode` locking. Each layer of locks is called in order of `lock_write()/lock_read()`, `inode->entry()->get_write()` for writing (for reading, the entry has no read reference and thus there is no ambiguity), bufcache spinlocks, and then entry spinlocks.

Subdirectory locking strategy: every subdirectory will be protected by the root directory lock, and every file will be protected by its parent directory lock, with the exception that unlinked files are protected by the root directory. New allocations of inodes are protected by the root lock. Under this paradigm, one can check that there are no modification races, and although two processes can race to create/delete something, only one will succeed and there is no undefined behavior. However, it is difficult to prevent races on searching the entire directory tree, and thus calls to `lookup_directory()` must be protected by *either* the `fdtable` lock (to be implemented in pset 5--thus any calls to directory lookups in syscall functions at the moment are not yet locked), or the global root directory r/w lock.

Note: The only function which may be called without lock is `lookup_directory(inode)`. 

Grading notes
-------------
Please enjoy the following fun additions to the OS and shell. **Below we will only outline how to test it.** For full documentation on how these were implemented, see *Part F*.

## Extra Credit Work
1. Parallel writes: See `ahcistate::read_or_write()` for implementation. Searches bitwise through all the slots to find the first open one. To see that multiple slots are indeed being used, run `make cleanfs run-testwritefs2 fsck` with the flag `SLOT_AND_PREFETCH_EXAMINE` in `kernel.hh` turned on and check the logs.
2. `sys_unlink`: Run `make cleanfs run-testwritefs4 fsck`, which will unlink files, some while open, so ensure it conforms to the corresponding Linux syscall specs.
3. `sys_rename`: Run `make cleanfs run-testwritefs6 fsck`, which will attempt to rename some files.
4. Subdirectory support: Take a look at `lookup_directory()` in `k-chkfs.cc` to get an idea of how this was implemented, and see the documentation in *Part F*.
5. `sys_mkdir`: Run `make cleanfs run-testwritefs7 fsck`, which will make some directories, check some invariants, induce some errors, and remove directories; you can also try the shell stuff below.
6. `sys_rm`: The test `make cleanfs run-testwritefs4 fsck` will also test `rm`.
7. `sys_ftruncate`: An extra credit syscall in the driver code that Eddie/Mickens added. Truncates the file or extends it with zero bytes, depending on the argument `length` passed in. Run `make run-testwritefs8 fsck` to try it out.
8. Support for `/dev/null`: Reading from it will return a single null character, unless the read argument `size` is 0; writing to it will return `sz` without doing anything. See 11 for tests.
9. Support for `/dev/random`: Reading from it will return `sz` bytes of randomly-generated characters; writing to it will return 0. See 11 for tests.
10. Support for `/dev/zero`: Reading `sz` bytes from it will provide a stream of `sz` zero bytes;  writing to it will return `sz` without doing anything. See 11 for tests.
11. Support for `/dev/full`: Reading `sz` bytes from it will provide a stream of `sz` zero bytes; writing to it will always return `E_NOSPC`. Useful for testing/simulating writes to a full disk. **To test all of the special files at once, run** `make cleanfs run-testwritefs5 fsck` [We have a special suprise for you... :-D].
12. Support for a per-process *current working directory*: you can now remember where you are working in the directory tree, and do not need to specify the full path! This let's us do cool things like the last 3 extra credits below.
13. `sys_pwd`: run `make cleanfs run-testwritefs9 fsck`, as well as the shell stuff below.
14. `sys_cd`: run `make cleanfs run-testwritefs9 fsck`, as well as the shell stuff below.
15. We added a lot of extra programs that give the shell a big upgrade! We added support for shell commands `mkdir, cd, pwd` and `rm`, which will actually deduce whether the object you are trying to remove is a file or a directory, and call `syscall_unlink()/syscall_rm()` accordingly. As we know from CS 61 PSet 5, `cd, pwd` are special because we can't just spawn a child to call `sys_cd`; for those we directory modified `p-sh.cc` to allow for changing and printing the directory without spawning a child process like usual, following the lead from the driver code's implementation of the shell command `exit`. Try it out! See example below.
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

