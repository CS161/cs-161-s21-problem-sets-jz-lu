CS 161 Final Project
============================

Documentation
----------------------------
Due to some minor problems relating to indecisiveness, my final project combines 3 of the topics suggested in the instructions. If you have time and would like it to be wasted in the most humorous way possible, I also have an extra secret project that in no way has anything to do with systems (see below, but don't tell my partner!*).

*Just kidding. Of course I got their permission.

**Important**: because we used Docker to support networking, running Chickadee now must be done inside Docker. Doing it outside of Docker will cause a compilation error. As such, anytime this document says "do `make run-...` it really means do `sudo ./run-docker` and then inside Docker, do `sudo make run-...`.

## Networking Support
=== TODO ===


## More Visualizations
In Problem Set 4, I added a number of extra features to the kernel, including suppoprt for a full-fledged file system tree (subdirectories, unlinking, special files, etc.) and a shell upgrade (working directories, `cd`, `ls`, `pwd`, `mkdir`, `rm`, etc.). Because of this, it seemed fitting to add some visualizations. The documentation in `pset4answers.md` details the upgraded file system design, so in this writeup we will proceed taking the FS as a given. The twofold purpose of this part is to (1) provide dynamic visualizations, similar to the memviewer, of the file system and VFS in as many ways as possible, and (2) make a version of those visualizations easily accessible in the shell.

1. **File system tree.** A great shell visualizer on Linux is the file system tree, which can be found by typing `tree` into the shell. The program prints to `stdout` a formatted tree of the file system tree, starting at the current directory. We imitate their program on Chickadee. `syscall_tree()` grabs the current working directory and passes it to `chkfsstate::tree()` along with a buffer to copy the visualization into. The general design is to DFS through the file system tree, done in `chkfsstate::tree_dfs()`, keeping track of the depth so that the proper offset in the tree can be printed. Once the buffer is filled in the DFS, the buffer is returned to `chkfsstate::tree()`, which adds final touches and passes it back up to the system call. The system call returns this to the user, which can now print the buffer out. The user-level implementation is found in `p-tree.cc`. You can run it just by `sudo make cleanfs run-tree`, but that is not very interesting since the default FS doesn't have any subdirectories! (We don't have a dynamic randomized visualizer with this since this particular visualizer is really meant just for the shell.) A much cooler version is given in the shell. Try, for example, the following sequence of commands in the shell:
```bash
mkdir os
cd os
pwd # should say '/os/'
mkdir lectures
cd lectures
mkdir other
echo garbage > LFS.txt
echo redo > journaling.txt
cd ..
echo CASH > investing.txt
cd ..
mkdir sections
echo hail linus > sections/microkernel.txt
tree # Watch the magic go
```
2. **VFS visualization.** Goal: print a formatted file descriptor table. This is accomplished in `proc::show_fdtable_(buf)` which prints the file descriptor table in a formatted fashion into the buffer. An additional system call `sys_fdshow()` allows users to provide a buffer and ask the kernel to give them the state of the file descriptor table. The table shows the permissions of open file descriptors as well as the type (pipe, disk, console, etc.). Printing the type required adding an additional attribute to `struct vnode`, `signature_`, which stores an enumeration of the type. Visualizing the file descriptor table can be useful in general at the shell level (see below), but it is even cooler with a dynamic visualization to see things allocated and freed automatically. As such, we took inspiration from the memviewer, adding `KDISPLAY_FDVIEWER` as a possible value to the atomic `kdisplay`. We updated `tick()` to check for that, and built a new function `fdtableshow()` that behaves like memshow, except switches between runnable processes occurs less often and it calls `proc::show_fdtable_()` as well as a new function `console_fdviewer()` in `k-memviewer.cc`. This function performs some string arithmetic and prints out a view similar to that of the memviewer. Finally, we add a process `p-fview.cc` that, similar to `p-allocexit.cc`, forks some processes and then randomly opens/closes randomly chosen types of files forever. This creates a dynamic randomized view of the file descriptor table. Run `sudo make run-fview` to check it out.
3. (Bonus!) **Buffer cache visualization** (suggestion by James Conant). Goal: dynamic visualization of the buffer cache. This one has no shell equivalent because it would be strange and (foolish, security-wise) to let user processes inspect the buffer cache (on the other hand, user processes are certainly allowed to inspect their own file descriptor tables). The design is at a high level the same as the VFS visualization, except the relevant functions are `bcshow()` calling `console_bcviewer()` for the dynamic viewer and the constant is `KDISPLAY_BUFCACHE`. `console_bcviewer()` uses new member functions `bufcache::show_line()`, `bufcache::show_evictq()`, and `bufcache::show_dirtyq()`; the former prints a *line*, defined to be an 8-block chunk of the cache, into a buffer; and the latter two dump their resepective list of buffer cache indices into a buffer. The state of the cache is then shown as the eviction queue state, dirty queue state, and a visual of the cache as a table, showing the type of node (superblock, bitmap, or data/inode). The total number of used entries is also given using a `bufcache::count()` function. (Printing the cache is not locked, since the state is the only dynamic thing and it is atomic; the visual being slightly behind or ahead the true state is not a big deal.) The `allocator`-like dynamic process is in `p-bcview.cc`, where we run in an infinite loop a bunch of file opens/reads/writes/seeks while the console updates the visual. Run `sudo make run-bcview` to see it.
4. Further shell upgrade. The first is the `tree` command discussed above. You can also type `fdshow` to get a visual of the file descriptor table for the shell process, associated with the VFS visualizer.

The primary difficulty in this work is all of the string arithmetic (I resorted to ASCII conversions several times) and getting the UI to look right, with things like centering and such; this is why you will find some constants declared like `UI_CENTER` and `UI_DEEPCENTER` which are helpful to store the offsets.


## Futexes and User-Level Mutexes
=== TODO ===


## Project(?) 4: Secret
Have a burning question that you just need some advice on? Have a lot of spare time and want to find a fun way to waste it all? Run `sudo make run-oracle` to get started! Hours of fun, starring real quotes my partner, Aakash 'Big Ka$h' Mishra, has said over the past semester. (Too lazy to run it this way? Ask your question directly in the shell by `sudo make run-sh` and typing `ask <question>`--I also welcome you to try edge cases, e.g. `ask` with no arguments). Enjoy! (I certainly did.)



Grading notes
-------------
