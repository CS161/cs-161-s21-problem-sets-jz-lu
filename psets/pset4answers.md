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
2. Once a spot has been found, `bufcache::prefetch()` calls `bcentry::prefetch_block()` on the allocated entry, locking the entry before and unlocking immediately after return (see *Synchronization Invariants* for synchronization plan and lock ordering). `prefetch_block()` is a nonblocking version of `bcentry::load()`, calling the nonblocking version of `ahcistate::read()` which has an additional argument `r` that is passed by reference to the NCQ to be set to something other than `E_AGAIN` upon completion of fetch. `bcentry::prefetch_block()` resets `bcentry::pfstatus_` to `E_AGAIN` and passes it in. Importantly, `prefetch_block()`, which is called under lock, sets the `estate_` of the block to a newly added state `es_prefetching` and pushes it onto a newly added `bufcache::pfq_` prefetching queue. An invariant of the queue is that every entry on it must have state `es_prefetching`, but the `pfstatus_` can be either `E_AGAIN` or something else. This queue is also used for eviction (see *Note* below), and in fact the only way for a block to leave the queue is for a process to register interest in it (see below) or by eviction.
3. Suppose at some point a process registers interest in a prefetched block with block number `b` by calling `get_disk_entry()` on `b`. If a prefetcher has already been called on `b` (if not then the function works as usual), then it will already appear in the cache. We modified `get_disk_entry()` to block, in the case that the block is already in the cache with `estate_ = es_prefetching`, until `pfstatus_ != E_AGAIN` (it may not block at all, if the fetch was already done). Once the disk completes the write, signaling via setting of `pfstatus_`, the waiting process wakes, sets `estate_ = es_clean`, and pops the entry off of the prefetch queue.
4. Suppose no process registers interest in `b` for a sufficiently long time (i.e. before eviction--see note below). Then the block will be evicted according to the prefetch block eviction policy outlined below.
5. Upon syncing with `drop > 0` in tests, the prefetch queue is also emptied, after each entry has been fetched successfully.

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
The below gives some basic documentation on the extra credit items we did for this problem set. To test them, see *Extra Credit*.

### Unlink
There are two primary specs of `syscall_unlink()` that are to be implemented.
1. A file to be unlinked should have all of its data freed, i.e. the inode and all its extents, when the last process holding the file open closes it. Note that this means a file may be unlinked while it is still open. After an unlink, processes that already had the file open may still perform I/O with it, but no process can open the file.
2. Regardless of whether at least one process had the file open during unlink is called, immediately after `syscall_unlink()` returns (and it must not block, aside from calls to `get_disk_entry()`), the name of the file just unlinked must be available for reuse, via creating a new file.

The following algorithm satisfies both specs. The key is that `chkfs::inode` currently has an unused `uintr32_t flags` variable. Use the first (least significant) bit as an indicator bit for whether the file is linked or not, and the remaining bits to store a per-inode `refcount`. Each `vnode` linked to that `inode` will hold one `refcount`; in particular, getting an inode does not affect the `refcount`, since many functions not associated with a user use `get_inode()`.
1. Upon entry of `syscall_unlink(name)`, lookup the inode of the file with `name` and immediately free the `direntry` associated with the inode being freed. Mark the `inode` flag as `unlinked`.
2. In the constructor of `disk_vnode`, increment the per-inode refcount using some bitwise arithmetic; in the destructor, `ino_->put()` is called.
3. `chkfs::inode::put(bool user)` should decrement the per-inode refcount if the `put` is being called by the destructor of a disk vnode. If the per-inode refcount has dropped to 0, and the file is marked as unlinked, it will free the inode via `chkfsstate::free_inode()` discussed earlier (walks through the extents and frees it all, then marks the inode as free by setting `type_ = 0`).


### Rename
Renaming a file does not require the inode at all, since the name is stored in the corresponding `direntry`. As such, our `chkfsstate::rename_direntry()` function just walks the directory until the given name is found, and changes the name to the new name, given that the `direntry` exists and the new name is valid memory- and size-wise.


### `ftruncate`
`syscall_ftruncate(len)` may be called with `len` greater than or less than the inode size. 
1. If `len < ino->size`, the inode size is simply updated to the new size (see discussion in *Part D* about true allocation size versus inode size).
2. If `len > ino->size`, the file is extended with `len - ino->size` zero bytes.
3. (If `len == ino->size` then nothing happens.)

The first case is implemented the same way as supporting `OF_TRUNC` in `syscall_open()`. The second follows the algorithm below.
```c++
// ftruncate() in disk vnode...
ftruncate() CALLED: args-> len // assume len > ino->size
current_offset = offset_ // save current off
buf = array of len-ino->size zero-bytes
write(buf, len - ino->size) // updates offset_
lseek(current_offset) // puts offset_ back
```
The synchronization plan is generally discussed in *Synchronization Invariants*, but this part requires a specific note. Before now `lseek()` and `write()` in the disk vnode each lock the inode associated with the vnode. However, the above algorithm requires them to be called in sequence. If the lock were to be released in between each call, it could cause a myriad of races; as an example, after the first `lseek()` completes, a child/parent/sibling process sharing the same vnode pointers calls `syscall_lseek(0, LSEEK_SET)`. Then when the `write` is made above, it corrupts part of the file! To circummvent this problem, it is key that the lock be acquired at the beginning of the algorithm's execution and released only at the end. To that end we added `write-nolock()` and `lseek_nolock()` in `disk_vnode` and modified `write()` and `lseek()` to just lock and call their nonlocking counterparts. In the above algorithm, we instead use the nonlocking versions, explcitly handling locking at the beginning and end of `ftruncate()`.


### Subdirectories
We added support for subdirectories of arbitrary depth, so long as the total path does not exceed `chkfs::maxnamelen` in length. Below we outline the functions used to make this possible, as well as changes to functions already implemented at this point.

#### New Functions
All of the below functions, except `chkfsstate::path_find_last(char* s)`, require holding of `fstlock` (see *Synchronization Invariants*).
1. `chkfsstate::lookup_directory(pathname)`: splits the pathname string, separated by deliminating characters `'/'` in a similar fashion to how the c-string function `strtok()` would except we implemented our own version, into "chunks". Starting at the root directory inode, `for` loop over the `direntry`s until either the current chunk is found, or the end of the directory is reached, in which case an error is returned. Has specifications `access_last`, which decides whether to skip the last chunk (usually because it is a filename and not a directory), and `is_file`, which is useful for error checking
2. `contains(dirino, inum)`: walks through the `direntry`s of `dirino` and checks whether it contains the inode indexed by `inum`.
3. `contains(dirino, name)`: same as `contains(inum)`, but searches `direntry`s by name rather than inode number.
4. `chkfsstate::lookup_directory(start_dirino, inode)`: overloaded version of (1). However, when searching by inode, we no longer have the luxury of knowing where we are going the next time--we cannot simply search for the next chunk since no information is given except the final destination. This function implements the search by DFS, with base case `contains(dirino, inum)` and recursing over each subdirectory until the inode has been found, or the entire tree has been searched. This function is necessary for freeing inodes.
5. `chkfsstate::path_find_last(char* s)`: returns the last chunk of a path. For example, if the path is `/brother/edward/says/how/dare/you/sleep/holding/a/spinlock.txt`, it would return a `char*` pointer to `spinlock.txt`.
6. `chkfsstate::directory_empty(inode* dirino)`: checks if a particular directory has any non-empty `direntry`s. Logic is essentially the same as `contains()`, but with a different stopping condition.
7. `free_direntry(inode* ino)`: frees the directory entry associated with a given inode. Utilizes `chkfsstate::lookup_directory(start_dirino, inode)` to find the directory, then calls `chkfsstate::free_direntry(inode* dirino, inode* ino)` which behaves similarly to `contains()`, but frees the directory entry once found rather than just returning.
8. `chkfsstate::mkdir(char* path)`: See *New System Calls* below as well.
9. `chkfsstate::rm(char* path)`: See *New System Calls* below as well.
10. `chkfsstate::ls(char* path)`: See *New System Calls* below as well.

#### Changes to Existing Functions
Every `chkfsstate` member function that looked up the root directory will be replaced by a `lookup_directory(filename)` call, which grabs the parent subdirectory of the file. For example, the below is the meat of the `chkfsstate::lookup_inode(filename)` function.
```c++
auto dirino = lookup_directory(filename);
if (dirino) {
    auto ino = fs.lookup_inode(dirino, path_find_last((char*) filename)); // bottom level
    dirino->put();
    return ino;
}
```
Most function calls have a **2-level sequence**, where the top level declared like `function(name/inode)` will be the only level ever called by other functions (with a few exceptions, such as if the parent directory is already known in some helper functions), and the bottom level declared like `function(dirino, name/inode)` is called by the top level. The top level always just calls `lookup_directory()` to find the right `dirino`, and then passes it to the bottom level, which does the work. This layering allows us to reuse all of the lofic from the code developed in the single-layer directory phase, by abstracting the subdirectory system to a different level of work. The bottom level function searches through the given `dirino` as if it were the only directory in the file system. As an example, the code block above is the top level of `lookup_inode()` and the line with an affixed comment `bottom level` is the call to the bottom level function.

Similarly, for system calls like `open(), unlink()`, etc. `lookup_directory()` is used to validate the path before any actions are made.

#### Directory Locking
In thd driver code, the primary invariant is that any access to a directory required the root directory inode to be locked for writing. For example, the `lookup_inode()` top-level function from the code block in the above section would have locked the root directory before calling the bottom-level function. In our case, we shall replace this with a file system tree lock. We implement this lock as a readers-writers lock identical to the locking mechanisms for inodes, except that we declared a struct globally.
```c++
struct rwlock {
    std::atomic<chkfs::mlock_t> mlock;
    void lock_read();
    void unlock_read();
    void lock_write();
    void unlock_write();
    bool has_write_lock() const;
} fstlock;
```
The file system tree lock `fstlock` is a readers-writers lock that locks the entire file directory tree (we define a **file system directory tree** to be the set of all directory inodes and their `direntry`s). We locked all syscalls that read (read lock) or modify (write lock) the file system tree in `kernel.cc` which completes subdirectory synchronization. These syscalls are `open(), rm(), unlink(), cd(), rename(), execv()`. N.B.: there are two aspects of locking to file systems, one in directories and one on file inodes. The latter has already been discussed as part of the problem set instructions, and will not be repeated here; the key observation is that file inodes can be modified independently of the directory, so the two locking mechanisms are independent of each other. Locking order still must be enforced; see *Synchrnization Invariants*.

#### Subdirectory Execution Interface
1. Making a directory: call `syscall_mkdir(path)`.
2. Removing a directory: call `syscall_rm(path)`. Note that `rm()` as a syscall is only use to remove directories, while `rm` the shell function (see *Shell Upgrade*) is used to remove both directories and files, depending on the format of the string passed in.

### Current Working Directory
We added support for a per-thread tracking of a current working directory, which will allow processes to store and work within a particular subdirectory. This is implemented by a `struct cwd:rwlock` which derives from a parent class `rwlock` (see *Synchronization Invariants*). 
```c++
struct cwd:rwlock {
    char name[chkfs::maxnamelen+1] = '/'; // initialized to root directory
    int write(char* path); // change cwd to path
    int read(char* buf); // read cwd into buffer
    int cat(char* path, char* buf); // add cwd as prefix onto path, place into buf
    bool subset(char* path); // is 'path' contained in the cwd?
    size_t len(); // length of cwd path
};
```
It is critical that `cwd` inherits `rwlock`, since I/O to `cwd::name` must be properly synchronized. As such, `cat(), read(), subset()` all use the read lock, and `write()` uses the write lock. `write()` also calls `chkfsstate::lookup_directory()` to ensure that the directory the user is changing to is valid. `struct proc` holds a new pointer `cwd* pwd_`, which holds a pointer to a `cwd` struct. The constructor `proc::proc()` takes in an argument `pwd`, and sets `pwd_ = pwd`, and the destructor deletes non-null `pwd_` pointers. `boot_process_start()` and `sys_fork()` allocate a new `struct cwd` and pass it to the newly allocated `struct proc`. All processes, except `idle_task, k_proc_init` have a CWD; those that do not have `pwd_ = nullptr`. As an aside, the shell function `pwd` stands for `print working directory`, whereas in this context `pwd` stands for `present working directory`; these should not be confused.

To allow for compatibility with our previous work, we do not change `lookup_directory()` or any of the `struct chkfsstate` member functions. Instead, whenever a pathname is specified at a syscall level, we affix a prefix (the part of the full path not fully specified by the user, but stored instead in `proc::pwd_`) length to the path total length while validating, if the path does not begin with a `'/'` (beginning with a slash indicates starting from the root, i.e. the user is specifying the whole path already, so no changes are necessary in that case), and before passing the path name into any `chkfsstate` interface functions, the partial path given by the user is concatenated with the prefix in `cwd` using `cwd::cat()`. As such, whenever a path is passed into `chkfsstate` member functions, it appears as if the user specified the entire path.

#### New System Calls
The documentation of `fstlock`, the global file system tree lock, is given in *Directory Locking* and *Synchronization Invariants*.
1. `syscall_mkdir()`: write-locks `fstlock`, and calls `chkfsstate::mkdir()` which ensures that the path (a) does not already exist, (b) does not have the same name as a file, and (c) has a valid parent directory to be stored in (will always be true unless some corruption has occurred or the parent has no more room for `direntry`'s). If valid, it calls `allocate_inode(type=chkfs::type_directory)` to get a new directory inode, `allocate_direntry()` to add the new directory to the parent directory's entries, and stores the new directory inode information in the new direntry.
2. `syscall_rm()`: write-locks `fstlock` and calls `chkfsstate::rm()`, which ensures that the path (a) exists as a directory (b) refers to an empty directory, and (c) has a valid parent directory to be stored in. If valid, it looks up the directory's parent inode, uses it to free the direntry corresponding to the directory being removed, then looks up hte inode of the directory itself, freeing it.
3. `syscall_pwd()`: does not require traversing the file system tree, so it does not lock `fstlock`. It simply calls `pwd_->read()`, which has its own locking system, and returns the buffer.
4. `syscall_cd()`: read-locks `fstlock` (because `pwd_->write()` uses `chkfsstate::lookup_directory()`) and calls `pwd_->write()`. If necessary, it first concatenates with the current path via `pwd_->cat()`.
5. `syscall_ls()`: read-locks `fstlock`, `proc::pwd_->read()`s the current directory into a path buffer, and and calls `chkfsstate::ls(pathbuf, buf)`, which looks up the directory associated with the path in the buffer and traverses it, storing each nonempty `direntry` name into `buf`, separating with a newline.

### Shell Upgrade
We equipped the process shell `p-sh.cc` with the tools to interact with the subdirectory execution interface as well as 
1. `p-mkdir.cc`: Calls `sys_mkdir()`, and includes a comprehensive error-handling suite.
2. `p-rm.cc`: Examines the argument string and determines whether it was intended to be a directory or a file. Calls `sys_rm()` or `sys_unlink()` accordingly. A comprehensive error-handling suite included.
3. `p-ls.cc`: Calls `sys_ls()`. Slightly less powerful than the bash equivalent, as our version will only allow listing of the current working directory (bash allows listing of an arbitrary directory as well).
4. `p-pwd.cc`: Calls `sys_pwd()`.
5. `cd`: As we know from CS 61, Problem Set 5, Part 9, `cd` is special because it cannot be executed by a child calling `sys_cd`, since that would just change the directory of the child, which rthen immediately exits. The behavior we seek is a change in directory on the shell process. As such we directly modified `p-sh.cc` to allow for changing and printing the directory without spawning a child process like usual, following the lead from the driver code's implementation of the shell command `exit`. In essence, the shell function `run_list()` checks if the command is `cd`, and if it is, directly calls `sys_cd()`; the spawned child process does nothing and exits immediately.

### Special files
We added another derived vnode class `special_vnode` that holds a `type_` variable enumerated over `null, random, zero, full`. In `syscall_open()`, the `pathname` string is first checked to see if it is one of these special files, and if it is, a special vnode is allocated in place of a disk vnode, and initialized with the appropriate type. The full implementaion is in `k-vfs.hh/cc`. In total there are four special files; we added all of the special pseudo-files Linux uses. Their functions are given in *Extra Credit*.


## Synchronization Invariants

### General Synchronization Plan
There are a number of components in the Chickadee file system, each of which has its own locking mechanism.
1. File system tree: Includes every inode of `type_ = chkfs::type_directory` and their associated `direntry`s stored in the extents. Locked with a single global readers-writers lock `fstlock` described in *Directory Locking*, so that no more than one process may modify the tree at a time, although many processes may read from it at once.
2. File inodes: Includes every inode of `type_ = chkfs::type_regular`. Locked with the driver code's inode readers-writers lock.
3. Buffer cache: Includes the lists/queues `pfq_, dirty_list_, evictq_, read_wq_` and the actual cache array itself. More info given in the synchronization documentation given at the bottom of [this page](https://read.seas.harvard.edu/cs161/2021/doc/synchronization-invariants/). Accesses locked by `sinlock bufcache::lock_`, often passed into a conditional sleep `block_until()`.
4. Cache entries: Includes everything in a `bcentry`; see `k-chkfs.hh`. `estate_, ref_` are protected by a per-entry spinlock, while modifications to the buffers (data from the disk held in the entry) are protected by a write reference obtained via `bcentry::get_write()`, which simultaneously pushes the entry onto `bufcache::dirty_list_` (see *Part C*).
5. Current working directory: See *Current Working Directory* for most of the discussion. One additional note: we define a **passing protocol** of current working directories by giving the CWD of the parent to the child in `syscall_fork()` and *not* resetting the CWD in `syscall_execv()`. These two properties combined allow for the shell to efficiently and easily execute processes like `pwd, ls, mkdir, rm`, integrating with the CWD. Passing is implemented with `cwd::pass(cwd* childcwd)`. We require that `pass()` may not be called once the child has begun execution, as `struct cwd` is not equipped to interact with other `struct cwd`s belonging to a runnable process. Thus `pass()` may not be called outside of `syscall_fork()`.
6. VFS vnodes: Includes the `refcount_` and `offset_`. Changes to `refcount_` are protected by `vnode::open_close_lock_`, except in the constructor, and changes to `offset_` are atomic. **Important**: `disk_vnode::offset_` is atomic, but the inode associated with a `disk_vnode` can be read simultaneously under a readers-writers lock. Thus if multiple processes point to the same vnode and read at the same time without synchronizing on their own, the same chunk of the file may be read twice, into each of the respective processes. We define this to be the unsynchronized behavior, as without this method two processes could not simultaneously read at all.


### Changes to Invariants
Summary of invariants changed with respect to those given in the Chickadee Buffer Cache invariant list [here](https://read.seas.harvard.edu/cs161/2021/doc/synchronization-invariants/) .
1. The `bcentry::buf_` is no longer constant after writing, but may not be modified without holding the `bcentry` write reference via `bcentry::get_write()`. 
2. When a kernel task holds a reference to a bufcache entry, the state must satisfy `buf_ != nullptr` and `estate_ == es_clean || es_dirty`. 
3. Just as one may check whether the state is `es_empty` without entry lock, one may also do so with `es_prefetching`. The queue for prefetching (either completed or not, but no process has yet to ask for it) is protected by the `bufcache::lock_`, as are all the other queues/lists in `bufcache`. 
4. Reading an (initialized) inode type does not require any locking.
5. Accessing the file system directory tree requires `fstlock` to be locked, not the root directory inode lock. Accessing an inode still requires that the inode r/w lock be held.
6. The lists `pfq_, evictq_, dirty_list_` store blocks with `estate_ == es_prefetching`, `ref_ == 0 && estate_ == es_clean`, and `estate_ == es_dirty`, respectively. As such at any timems these lists must have empty intersection.
7. Eviction of prefetched blocks have an inherent race condition with the SATA disk that does not affect the correctness of the code. See the commment in `bufcache::evict()` for details.
8. The free block bitmap must be locked with the bufcache write reference when allocating an extent.
9. Accessing the CWD of a thread does not require holding any lock, except when calling `cwd::write()`, which requires the `fstlock` as it calls `chkfsstate::lookup_directory()`.
10. `vnode::refcount_` is now incremented in the constructor, so does not require the allocating function to explicitly lock and increment it. `vnode::offset_` has been made atomic (see (6) from above section about VFS vnodes.)

**Lock ordering hierarchy**: every subset of locks must obey the following order invariant. At the very top the `fstlock` must always be called first, and only in system calls (`chkfsstate` member functions shall always assume that `fstlock` is held, if required). Next, directory `inode`s are to be locked, followed by file `inode` locking. Then any bufcache entry write references can be grabbed: `inode->entry()->get_write()`. Finally, bufcache spinlocks, and then bufcache entry spinlocks can be grabbed.

Note that strictly speaking, the directory inode lock is obsolete. It is kept in the lock hierarchy above for compatiblibility with the driver `chkfsiter` code, the directory inode needs to be locked when allocating and freeing a `direntry`, though strictly speaking it is not necessary since `fstlock` is required for both such functions so the caller will be guaranteed to instantly obtain the directory lock.


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
15. We added a lot of extra programs that give the shell a big upgrade! We added support for shell commands `mkdir, cd, pwd` and `rm`; the latter will actually deduce whether the object you are trying to remove is a file or a directory, and call `syscall_unlink()/syscall_rm()` accordingly. Try it out! See example below (test sponsored by [this video](https://youtu.be/D5xh0ZIEUOE)).
```bash
mkdir mickens
cd mickens
pwd
cat /thoreau.txt > javascript_sux.txt
cat javascript_sux.txt
rm javascript_sux.txt
cd /
pwd
rm mickens
```
**Important Note**: there is a key implementation detail unique to Chickadee, in that all shell processes are always in the cleaned file system image, and they all live in root. Thus `syscall_execv`
()` cannot add on a prefix, or else the shell functions will fail to execute. As such it is important to note that if any executables are ever placed in a subdirectory, the entire path must be specified to run it, not just the name, regardless of the CWD.
