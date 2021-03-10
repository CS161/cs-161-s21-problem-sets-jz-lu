CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

Answers to written questions
----------------------------
### Part C
The bounded buffer implementation follows closely the implementation from CS 61 linked on the problem set statement; as such, we refrain from restating all of the logic for the bounded buffer. We built the pipe `vnode` system in our design document early on, so any updates are below.
1. In the document it was decreed that the writer pipe `vnode` should allocate and free the buffer. It turns out the natural way is slightly more complicated. We added two additional boolean metadata to the bounded buffer: `read_closed` and `write_closed`. The constructor of the `write` class shall as defined in the design document allocate the bounded buffer, but the destructor shall only free the buffer if both of the above booleans are true. The destructor for the write end shall, all under the shared bounded-buffer lock, set `write_closed` to true, then check if the dual `read_closed` is true, freeing `bbuf` if so. Otherwise, it will just exit. The read end destructor does the same, switching the bools above.

Grading notes
-------------
