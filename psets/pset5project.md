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

1. File system tree. A great shell visualizer on Linux is the file system tree, which can be found by typing `tree` into the shell. The program prints to `stdout` a formatted tree of the file system tree, starting at the current directory. We imitate their program on Chickadee. `syscall_tree()` grabs the current working directory and passes it to `chkfsstate::tree()` along with a buffer to copy the visualization into. The general design is to DFS through the file system tree, done in `chkfsstate::tree_dfs()`, keeping track of the depth so that the proper offset in the tree can be printed. Once the buffer is filled in the DFS, the buffer is returned to `chkfsstate::tree()`, which adds final touches and passes it back up to the system call. The system call returns this to the user, which can now print the buffer out. The user-level implementation is found in `p-tree.cc`. You can run it just by `sudo make cleanfs run-tree`, but that is not very interesting since the default FS doesn't have any subdirectories! A much cooler version is given in the shell. Try, for example, the following sequence of commands in the shell:
```bash
mkdir os
cd os
pwd # should say '/os/'
mkdir lectures
cd lectures
mkdir other
echo hi > LFS.txt
echo hi > journaling.txt
cd ..
echo CASH > investing.txt
cd ..
mkdir sections
echo FLAME WAR > sections/microkernel.txt
tree # Watch the magic go
```
2. VFS visualization. 
3. (Bonus!) Buffer cache visualization (suggestion by James Conant). 
4. Further shell upgrade. 

The primary difficulty in this work is all of the string arithmetic (I resorted to ASCII conversions several times) and getting the UI to look right, with things like centering and such; this is why you will find some constants declared like `UI_CENTER` and `UI_DEEPCENTER` which are helpful to store the offsets.


## Futexes and User-Level Mutexes
=== TODO ===


## Project(?) 4: Secret
Have a burning question that you just need some advice on? Have a lot of spare time and want to find a fun way to waste it all? Run `sudo make run-oracle` to get started! Hours of fun, starring real quotes my partner, Aakash 'Big Ka$h' Mishra, has said over the past semester. (Too lazy to run it this way? Ask your question directly in the shell by `sudo make run-sh` and typing `ask <question>`--I also welcome you to try edge cases, e.g. `ask` with no arguments). Enjoy! (I certainly did.)



Grading notes
-------------
