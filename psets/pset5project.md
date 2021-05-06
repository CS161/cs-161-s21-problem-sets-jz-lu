CS 161 Final Project
============================

Documentation
----------------------------
Due to some minor problems relating to indecisiveness, my final project combines 3 of the topics suggested in the instructions. If you have time and would like it to be wasted in the most humorous way possible, I also have an extra secret project that in no way has anything to do with systems (see below, but don't tell my partner!*).

*Just kidding. Of course I got their permission.

**Important**: because we used Docker to support networking, running Chickadee now must be done inside Docker. Doing it outside of Docker will cause a compilation error. As such, anytime this document says "do `make run-...` with regards to networking-related things, it really means do `sudo ./run-docker` and then inside Docker, do `sudo make run-...`.

**Even more important**: running networking on Docker has some downsides. Some machines appear to refuse to build the Docker image with the networking utils. I myself was unable to get Docker to build on my computer, but thanks to David Chen's generosity I was able to ssh into his server to work on networking. As such, in case the Docker file won't build (instructions to try building are below), I have pre-recorded a video showcasing networking and some other parts of the project, accssible [here](https://harvard.zoom.us/rec/play/mrROkMFUCVBTa_5aQBjsYnemktkciN-zmoGMFehThgUKSIjqKVTcVtsmpTgOPw9vFnfr5pkqvuBP7_yI.7RTZVIMKVSj3Y7_f?autoplay=true&startTime=1620269516000) until June 4, 2021. In the video I showcase the results of networking and more visualizations, but leave out futexes/mutexes since there nost much demo-wise to see there other than seeing in the console that the tests did succeed. (Video submission approved by Prof. Mickens.)

## Part 1: Networking Support
We aimed to build networking support by first implementing device driver support and then building a simplified version of network stack support, including our own ethernet interface and also building a simplified version of the IP stack from xv6. The end product should allow us to send/receive packets, with a new method of pinging allowing us to ping other servers from Chickadee and allowing Chickadee to respond to ping requests as well.

The design borrows heavily from the MIT [xv6 implementation](https://pdos.csail.mit.edu/6.828/2017/labs/lab6/) (more links are given in the comments of the code) and Chickadee's `struct ahcistate`. The architecture from the bottom-up is given below. Sending a packet generally first passes through ARP to get an address from the table, then follows ICMP to send the packet through the layers of the simplified network stack. Note that we have updated the makefile and Dockerfile for networking support (credit goes to my partner Aakash, who did a lot of reading to figure out what flags to add).
1. We define `struct e1000state` which encodes the state of the NIC as emulated by QEMU. A combination of writing to ports via methods of `struct pcistate` in `e1000state::find()` and writing via MMIO using mapped registers. We then allocate/initialize/enable the transmit (Tx) and receive (Rx) ring buffers and configure interrupts. To synchronize, we provide `e1000state` with a lock, so that accesses to the ring buffers and other data would not race. System calls and the interrupt handler (which runs with interrupts disabled) must run under this lock (this is a new synchronization invariant).
2. The ethernet layer runs atop this low-level device interaction, using `struct eth_header` which tracks packet MAC addresses. Atop that we run a simplified version of the xv6 IP layer methods.
3. Finally, ICMP allows us to implement pinging in combination with the ARP protocol.

All of our code sits in `k-netdriver.hh`, `k-ip.hh`, `k-arp.hh`, and `k-icmp.hh` as well as additional syscalls and interrupt handler in `proc::exception()` as well as their respective `.cc` files. The `ping` test can be found in `p-ping.cc`.

Some major challenges of the project was first to figure out how to actually configure the NIC. Although we had some hints from `ahcistate` and the device interactions lecture, it was not entirely clear as to what constants/registers/etc. to use, and these required a fair amount of research. It was also a challenge to work through the many layers of indirection, including Docker--it was particularly hard to communicate from Docker and the outside world.

To test the code, one must use Docker. Try the following, and if the Docker image fails to build, see the (**Even more important**) note above. 
1. Build the Docker image using `sudo docker build -t cs161:latest -f docker/Dockerfile .` from the home directory `cs-161-...`.
2. If the build succeeds, run `sudo ./run-docker`. This should open a Docker container.
3. Open a new terminal window. Run `sudo docker ps` and copy the Container ID. Pass it into the command `sudo docker exec -it <ID> bash`. This should open a second window of the same Docker instance.
4. On one of the windows, run `sudo ./dockernetsetup.sh` and verify that it went through. This sets up the different IPs and interfaces. Then run `ping google.com` and ensure it works. If it does, then networking is set up. Otherwise, see the note from the beginning.
5. Run `sudo tcpdump -XX -n -i 8.8.8.8 tap0` on one window to start listening for the IP 8.8.8.8 (Google). On the other window, launch Chickadee with `sudo make run-sh`.
6. Type `ping 8.8.8.8` in the Chickadee shell. You should see momentarily 4 responses, recorded in sequence in the corresponding tcpdump. It worked!
7. Feel free to try other pings, such as CloudFlare (1.1.1.1) or even Chickadee itself, which will show off Chickadee's response protocol to ping requests from other (or in this case, itself) servers.


## Part 2: More Visualizations
In Problem Set 4, I added a number of extra features to the kernel, including suppoprt for a full-fledged file system tree (subdirectories, unlinking, special files, etc.) and a shell upgrade (working directories, `cd`, `ls`, `pwd`, `mkdir`, `rm`, etc.). Because of this, it seemed fitting to add some visualizations. The documentation in `pset4answers.md` details the upgraded file system design, so in this writeup we will proceed taking the FS as a given. The twofold purpose of this part is to (1) provide dynamic visualizations, similar to the memviewer, of the file system and VFS in as many ways as possible, and (2) make a version of those visualizations easily accessible in the shell. A description and the tests are given below.

1. **File system tree.** A great shell visualizer on Linux is the file system tree, which can be found by typing `tree` into the shell. The program prints to `stdout` a formatted tree of the file system tree, starting at the current directory. We imitate their program on Chickadee. `syscall_tree()` grabs the current working directory and passes it to `chkfsstate::tree()` along with a buffer to copy the visualization into. The general design is to DFS through the file system tree, done in `chkfsstate::tree_dfs()`, keeping track of the depth so that the proper offset in the tree can be printed. Once the buffer is filled in the DFS, the buffer is returned to `chkfsstate::tree()`, which adds final touches and passes it back up to the system call. The system call returns this to the user, which can now print the buffer out. The user-level implementation is found in `p-tree.cc`. You can run it just by `sudo make cleanfs run-tree`, but that is not very interesting since the default FS doesn't have any subdirectories! (We don't have a dynamic randomized visualizer with this since this particular visualizer is really meant just for the shell.) A much cooler version is given in the shell. Try, for example, the following sequence of commands in the shell:
```bash
mkdir cs161
cd cs161
pwd # should say '/cs161/'
mkdir lectures
cd lectures
mkdir other
echo garbage > LFS.txt
echo redo > journaling.txt
cd ..
echo CASH > investing.txt
cd ..
mkdir sections
echo all hail linus > sections/microker.txt
tree
```
2. **VFS visualization.** Goal: print a formatted file descriptor table. This is accomplished in `proc::show_fdtable_(buf)` which prints the file descriptor table in a formatted fashion into the buffer. An additional system call `sys_fdshow()` allows users to provide a buffer and ask the kernel to give them the state of the file descriptor table. The table shows the permissions of open file descriptors as well as the type (pipe, disk, console, etc.). Printing the type required adding an additional attribute to `struct vnode`, `signature_`, which stores an enumeration of the type. Visualizing the file descriptor table can be useful in general at the shell level (see below), but it is even cooler with a dynamic visualization to see things allocated and freed automatically. As such, we took inspiration from the memviewer, adding `KDISPLAY_FDVIEWER` as a possible value to the atomic `kdisplay`. We updated `tick()` to check for that, and built a new function `fdtableshow()` that behaves like memshow, except switches between runnable processes occurs less often and it calls `proc::show_fdtable_()` as well as a new function `console_fdviewer()` in `k-memviewer.cc`. This function performs some string arithmetic and prints out a view similar to that of the memviewer. Finally, we add a process `p-fview.cc` that, similar to `p-allocexit.cc`, forks some processes and then randomly opens/closes randomly chosen types of files forever. This creates a dynamic randomized view of the file descriptor table. Run `sudo make run-fview` to check it out.
3. (Bonus!) **Buffer cache visualization** (suggestion by James Conant). Goal: dynamic visualization of the buffer cache. This one has no shell equivalent because it would be strange and (foolish, security-wise) to let user processes inspect the buffer cache (on the other hand, user processes are certainly allowed to inspect their own file descriptor tables). The design is at a high level the same as the VFS visualization, except the relevant functions are `bcshow()` calling `console_bcviewer()` for the dynamic viewer and the constant is `KDISPLAY_BUFCACHE`. `console_bcviewer()` uses new member functions `bufcache::show_line()`, `bufcache::show_evictq()`, and `bufcache::show_dirtyq()`; the former prints a *line*, defined to be an 8-block chunk of the cache, into a buffer; and the latter two dump their resepective list of buffer cache indices into a buffer. The state of the cache is then shown as the eviction queue state, dirty queue state, and a visual of the cache as a table, showing the type of node (superblock, bitmap, or data/inode). The total number of used entries is also given using a `bufcache::count()` function. (Printing the cache is not locked, since the state is the only dynamic thing and it is atomic; the visual being slightly behind or ahead the true state is not a big deal.) The `allocator`-like dynamic process is in `p-bcview.cc`, where we run in an infinite loop a bunch of file opens/reads/writes/seeks while the console updates the visual. Run `sudo make run-bcview` to see it.
4. Further shell upgrade. The first is the `tree` command discussed above. You can also type `fdshow` to get a visual of the file descriptor table for the shell process, associated with the VFS visualizer.

The primary difficulty in this work is all of the string arithmetic (I resorted to ASCII conversions several times) and getting the UI to look right, with things like centering and such; this is why you will find some constants declared like `UI_CENTER` and `UI_DEEPCENTER` which are helpful to store the offsets.


## Part 3: Futexes and User-Level Mutexes
The goal is to (a) imitate the salient features of Linux's `futex()` system call, and then use it to build an efficient user-space mutex. We added `k-futex.hh/cc` to the kernel and built a data structure for futexes. Since each futex waits on the value of a futex word (i.e. 32-bit chunk of memory) to change from an expected current value `val`, we design `struct futexwaiters` similar to the buffer cache (i.e. one global structure accessible via `futexwaiters::get()`), but with a list of states indexed by a pointer to the word. Each state is equipped with a wait queue for threads to sleep on. 

When a process asks to wait on a word, the system call `syscall_futex()` asks `struct futexwaiters` to add a waiter, which is done by locking the list, searching it, and allocating a new `struct futexstate` if one is not found. Each `struct futexstate` has a refcount, so if a state with the given word pointer already exists then the refcount is just incremented. `struct futexwaiters` returns the state to the system call, which calls a `waiter().block_until(...)` on the state's wait queue. (In Linux, the system call first checks if at the beginning the word was already not equal to `val`, in which case `E_AGAIN` is immediately returned, so we do this too.) In addition, we implement a timeout protocol that wakes up waiters after a user-specified timeout; the timer interrupt handler calls `futexwaiters::check_timeout()` which iterates through the list of states and wakes everyone up to check for timeouts. 

When a thread changes the word, it must call `sys_futex(word, FUTEX_WAKE, <nwake>, ...)` which wakes up at most `nwake` processes sleeping on the word. To implement this we added an attribute `wait_queue::wake_some(nwake)` which is exactly like `wake_all()` except it has a counter and wakes at most `nwake` threads. 

*Test*: We created `p-testfutex.cc` (run `sudo make run-testfutex`) which tests waiting, the `E_AGAIN` method, and timeouts by spawning threads and having them wait on the same word. 

The futex is most useful when implementing a user-space mutex. In `u-lib.hh/cc` we added a new `struct mutex` with a word being 0, 1, or 2 respectively if the lock is locked, taken with no waiters, and taken with waiters. To balance high and low contention locking situations, `mutex` first spins for `40` rounds on the lock. If it fails to acquire it, it moves to phase 2, calling `sys_futex(WAIT)` and going to sleep. Upon unlock, `sys_futex(WAKE)` is called only if the word is 2, as that indicates other threads are sleeping on the lock. Finally, similar to `spinlock_guard`, we added a `mutex_guard` scoped lock structure. 

*Test*: Run `sudo make run-testmutex`. This spawns threads that repeated take a (non atomic) value `val` and increment it by 6, one at a time in a loop. To avoid the compiler optimizing away this sub-optimal code, we add a write to `/dev/null` of the value between every increment (we added suppoort for special files in Problem Set 4) so that each increment is properly used. We maintain the invariant that at the beginning and end of every execution of the loop, `val % 6 == 0`, so that if threads were racing, then it is highly likely that at some point the invariant will be broken since the lock is called right before the loop and unlocked right after. In other words, the whole loop (in `change_val()`) must finish serially. We first let the threads run for `NSPINLOOP = 100000` times, which is almost certain to catch any races if there were any, and then to encourage heavy use of the actual futex we sleep until the next timer interrupt while holding the lock before changing the value. Normally this would be absurd and immoral to sleep holding the mutex, but for the purposes of the test it is important to stress the futex structure out. Because sleeping causes a lot of latency, we only run for `NSLEEPLOOP = 500` times, but as it turns out in the spin loop the sleep is also used a lot, so this just stresses it a little bit more and the combination still suffices for catching race conditions. Finally, we rerun the spin test but using the scoped lock. This method was efficient in catching races: the first time we made a small error in the scoped lock and it was caught immediately by the test. 

Since there was fairly good documentation on futexes, the primary difficulty was actually in designing the test (and ensuring that the data structure was as space/time efficient as possible). We eventually used the increment-write method since it is simple but very effective in catching races. We also encountered some difficulties working with making threads, since our implementation of `sys_clone()` requires the function that the thread should execute on to have specific arguments and return types, but reading through `p-testthread.cc` thoroughly was helpful.

Note that the combination of this, threading, and networking actually set up the framework to be able to (not that we did--that would take even more time) should be sufficient in terms of framework to recreate most if not all of the driver code for Network Pong used in CS 61 PSet 6.


## Part(?) 4: Secret
Have a burning question that you just need some advice on? Have a lot of spare time and want to find a fun way to waste it all? Run `sudo make run-oracle` to get started! Hours of fun, starring real quotes my partner, Aakash 'Big Ka$h' Mishra, has said over the past semester. (Too lazy to run it this way? Ask your question directly in the shell by `sudo make run-sh` and typing `ask <question>`--I also welcome you to try edge cases, e.g. `ask` with no arguments). Enjoy! (I certainly did.)



Grading notes
-------------
Please see the **Important** and **Even more important** notes at the top of the page.
