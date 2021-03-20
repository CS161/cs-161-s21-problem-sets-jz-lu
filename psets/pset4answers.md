CS 161 Problem Set 4 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset4collab.md`.

Answers to written questions
----------------------------
### Part A
Eviction strategy: LRU. Use a queue of cache entry indices with zero refcount. When refcount goes up in `bufcache::get_disk_entry()`, pop the entry index off of the queue, and when it goes to zero, push it onto the (back of the) queue. Thus the front of the queue will always hold the least recently used with zero refcount, and can be evicted in `O(1)` time. Popping an index can be done by `erase()` which it also `O(1)` time. 

Grading notes
-------------
