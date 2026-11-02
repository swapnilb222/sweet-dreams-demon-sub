# Track D: performance log

## Where we ended up

| | T (s) | M (MB) | Lines | K | Points |
|---|---:|---:|---:|---:|---:|
| Python reference | 72.69 | 893 | 43 | 1.00 | 0.00 |
| Our best board run (step 28, commit b82aadb) | **0.0849** | **56.6** | **586** | **34.33** | **76.78** |

That is about **850 times faster** than the reference, with **15 times less memory**. Most of the speed came from three ideas:
- making the graph smaller by contracting chains of pass-through nodes;
- renumbering it so that nodes close on the map are close in memory;
- then a long series of fixes that each removed one specific kind of waiting.

Single runs on the scoring machine vary by several percent: the same code has measured 0.087 s in one hour and 0.091 s in another. So every number below is a measurement, not a promise. We kept a change only if it won on our own machine and then held up on the board.

---

## Every step on the live board

Each step is one push. A push is run once per team, so most steps have two board runs, and the table shows the faster one. "Board test" marks a change we could not judge on our own machine, so we pushed it to measure it and reverted it if it did not win.

| Step | What changed | Commit | Run ID | T (s) | M (MB) | Lines | K | Points |
|---|---|---|---|---:|---:|---:|---:|---:|
| 1 | Parallel C++ version: parsing, compressed graph, bidirectional search | 06837bc | not recorded | 0.447 | 169 | 350 | 13.00 | 55.70 |
| 2 | Chain contraction, locality renumbering, bucket-queue search | bdfa5e8 | run-6bc981e03f03 | 0.109 | 65.3 | 574 | 29.56 | 73.54 |
| 3 | Memory diet: input bins freed as the graph is built, compact queues | 5223e25 | run-db3c3a2d92ae | 0.117 | 55.6 | 572 | 29.40 | 73.42 |
| 4 | Bins freed between build passes, not during them | 45e4d5b | run-157cf3e402b1 | 0.110 | 55.2 | 571 | 30.39 | 74.14 |
| 5 | Chain contraction folded into the renumbering pass | 6bbd254 | run-caef9c2ebef7 | 0.110 | 55.2 | 563 | 30.44 | 74.17 |
| 6 | Correctness fix, profile-guided static build, parallel teardown, fused reverse pass | 93ab427 | run-7e04fad34160 | 0.113 | 55.4 | 569 | 29.99 | 73.85 |
| 7 | Step 6 without the parallel teardown, with cheap claims and an exact fallback | 93a6c1c | run-b0e091cfc395 | 0.109 | 55.3 | 571 | 30.63 | 74.30 |
| 8 | Prefetched chain walks, branch-free edge relaxation | 240b78d | run-b11110b169c2 | 0.100 | 54.9 | 571 | 31.95 | 75.22 |
| 9 | Hot loops rewritten so the compiler keeps their values in registers | d88f3f8 | run-460785953528 | 0.096 | 57.3 | 578 | 32.25 | 75.43 |
| 10 | Raw graph stored as four fixed slots per node | 65cc5ce | run-4efb9302d30e | 0.089 | 57.8 | 593 | 33.25 | 76.09 |
| 11 | Raw graph's pages reused for the id map and forward offsets | 5491bc8 | run-940c564ae383 | 0.089 | 59.0 | 600 | 33.08 | 75.98 |
| 12 | Input file unmapped by the parsing threads (reverted) | 6c0a85d | run-225c1dc47a94 | 0.095 | 56.0 | 601 | 32.31 | 75.57 |
| 13 | Back to step 11 | 11cca82 | not run separately | | | | | |
| 14 | Board test: reverse graph in 2 MiB pages (reverted) | 99aef00 | run-719b42059767 | 0.089 | 57.4 | 622 | 33.15 | 76.03 |
| 15 | Back to step 11 | f09ad57 | run-4590a0e90760 | 0.087 | 58.0 | 600 | 33.63 | 76.34 |
| 16 | Board test: barrier that spins briefly before sleeping (reverted) | 67069f8 | run-8755815cfc16 | 0.087 | 58.1 | 610 | 33.57 | 76.27 |
| 17 | Back to step 11 | 89b85fd | run-98427ea1df21 | 0.089 | 58.8 | 600 | 33.31 | 76.09 |
| 18 | The same program in 11 fewer lines | 53bbed8 | run-c9f3114356b5 | 0.089 | 58.2 | 589 | 33.48 | 76.19 |
| 19 | Board test: fewer instructions in the search and the parser | 30bba3d | run-af7a78fbfbdb | 0.094 | 56.1 | 596 | 32.61 | 75.62 |
| 20 | Back to step 18, as the box was noisy during step 19 | a16b30e | run-3fc80c66055d | 0.088 | | 589 | | |
| 21 | Step 19 again, on a quiet box (kept) | 55eeb87 | run-86a463aba615 | 0.086 | 58.5 | 596 | 33.72 | 76.40 |
| 22 | The same program in 9 fewer lines | c09205a | run-0638cc93d96a | 0.086 | 58.9 | 587 | 33.76 | 76.42 |
| 23 | The same program in 9 fewer lines again | 640d57d | run-325981a15163 | **0.0856** | **57.9** | **578** | **34.12** | **76.65** |
| 24 | 16-bit distance labels, with an exact 64-bit rerun (busy box: Track A 6% and 10% slow) | ca05322 | run-c43ea24c24c4, run-f85df36b8aba | 0.0855 | 58.6 | 587 | 33.96 | 76.55 |
| 25 | Step 24 in 8 fewer lines (busy box: Track A 20% slow) | 5f88cb7 | run-0615a8689401 | 0.0854 | 58.4 | 579 | 34.08 | 76.63 |
| 26 | Step 25 rerun, PERF.md only (quiet box: Track A normal) | 6d8a4be | run-b2d9282c5a33 | 0.0960 | 56.2 | 579 | 30.69 | 75.52 |
| 27 | Back to step 23's code, minus the same 8 header lines (identical machine code; busy box: Track A 25% slow) | 35dca0f | run-a291b921cb21 | 0.0902 | 58.5 | 570 | 33.22 | 76.08 |
| 28 | 7.8% fewer instructions: a fast path for light edges, a sentinel bucket tail, digits converted with SSE | b82aadb | run-563b7cbbe53a | **0.0849** | **56.6** | **586** | **34.33** | **76.78** |

---

## How the program works

The task is to answer 2,000 shortest-path queries on a road network with 1.74 million nodes and 5 million directed, weighted edges. Reading the input is part of the timed run, so the real challenge is building a good graph quickly, not only searching it. The program runs in four stages on 12 threads.

**1. Read the graph.** The input file is memory-mapped, and each thread parses its own slice. Numbers are decoded eight bytes at a time with bit arithmetic rather than one character at a time. Each edge goes into a page-sized chunk for its part of the id range. Each part's rows are then built separately, in cache. A node gets four fixed slots for its edges, which covers 97% of nodes in this road network. The rare node with more edges points to a shared spill area instead. Finding a node's edges therefore takes one memory access instead of two. While building, the program marks the nodes that just continue a road: at most two outgoing edges, and not a query endpoint.

**2. Contract and renumber.** About 30% of the nodes are such pass-through points. Each thread grows compact breadth-first regions over the graph and hands out new ids in visiting order, so that nodes close on the map end up close in memory. When an edge leads into a pass-through node, the program follows the chain to the first node that has to stay, and the edge then points straight there with the summed weight. Chain walks run in small batches that advance together, so their memory requests overlap instead of queuing one after another. The result is the forward graph in its new numbering, with about 1.22 million nodes and 3.96 million edges. A single pass renames the edges and counts each node's incoming edges at the same time. A second pass then fills the reverse graph for the backward searches. The id map and the forward offsets are placed in the raw graph's pages, which are already in memory, instead of being allocated fresh.

**3. Answer the queries.** Each query first runs a short forward-only search, which settles nearby targets on its own. It then searches from both ends, always growing the side that has done less work, until the two fronts prove the best meeting point is optimal.
- The priority queue is a ring of distance buckets, with a small heap for the rare very long jumps.
- Distances live in small pages that are created on first use and recycled between queries, so a search only touches the memory around where it actually goes.
- The inner loop has no unpredictable branches: it always writes the shorter of the old and new distance, and always writes the queue entry, counting it only when the path improved.

**4. Write the answers** in input order, and exit without freeing anything. The operating system reclaims the memory anyway.

Every edge is one 32-bit word, holding the head node in the low bits and the weight above it. The few weights too large for the field, about 0.1% after contraction, sit in a small side table. The binary is linked fully static and built twice. The first build runs once on a small input made by the official generator with a different seed from the board's. The second build uses that profile to lay out the branches the way they actually run.

---

## What each step did

### Steps 1 and 2: a fast graph, in a good order
Step 1 replaced the Python reference with straightforward parallel C++. It was already 160 times faster, but the node ids are a random permutation, so almost every step of a search missed the cache.

Step 2 fixed that with three ideas, which took T from 0.447 s to 0.109 s:
- **Chain contraction** removes 30% of the nodes, and every search shrinks with it.
- **Breadth-first renumbering** puts neighbouring nodes next to each other in memory.
- **A bucket queue** replaces the binary heap. The weights are small integers, so distances can be grouped into fixed-width buckets, which is far cheaper than heap operations.

### Steps 3 to 5: less memory
Memory is a fifth of the score, and the reference uses 893 MB, so every megabyte counts. We tracked the program's memory over time and found three separate peaks:
- **While building:** the parsed edges and the new rows were alive together. The parsed edges now live in page-sized chunks that are released group by group as the rows are built.
- **While searching:** each bucket kept the largest size it had ever reached, about 100,000 entries per direction, although only about 3,000 are live at once. Buckets now draw from shared, recycled blocks.
- **While renumbering:** node degrees are stored in one byte instead of four, and bitmaps that were not needed are gone.

The peak went from 65 MB to 55 MB. Step 4 then moved the releases to the moments between build passes, because releasing memory briefly stalls every other running thread. Step 5 folded contraction into the renumbering pass, which removed a whole scan of the graph.

### Steps 6 and 7: a real bug, and a faster build
Running every small test case many times, with different thread counts and under the address sanitizer, turned up wrong answers in some runs of one 32-node case.

Claiming a node during renumbering uses a plain read and write, which is cheaper than an atomic operation but lets two racing threads both claim the same node. The extra copy is harmless, except that on a graph with nothing to contract, the duplicates can push the node count past the head field of an edge word. A later stress run found the same race could also overrun a thread's edge buffer on a tiny graph.

The fix keeps the cheap claims and checks the result: if duplicates would overflow the field, a thread's buffer, or the raw graph's size, the pass runs again with exact atomic claims. That never happens on full-size inputs, so it costs nothing there. On the tiny graphs it does happen, and every stress run since then has passed.

Step 6 also brought the profile-guided static build, and a rename pass that counts incoming edges as it goes, which saves one scan of the forward graph. Its third part, freeing memory in parallel just before exit, made the board slower. Step 7 removed it.

### Step 8: two hot loops waiting needlessly
- **Chain walks waited twice per hop.** Each round fetched the next node's row offset ahead of time, but not the row itself. Now each round first requests the rows it is about to read, so those misses overlap. Renumbering went from about 32 to 29 ms.
- **The search branched on a coin flip.** "Is this path shorter?" is close to random, so the processor guessed wrong about half the time. The relaxation is now branch-free, as described above. The query phase went from about 26.5 to 25 ms.

On the board, T went from 0.109 s to 0.100 s.

### Step 9: values in registers instead of memory
In C++, a write through one `u32` pointer may change any other `u32` in memory, unless the compiler can prove otherwise. Our hot loops store `u32` values while reading `u32` arrays. So after every store, the compiler reloaded the row offsets, the row's end, an edge counter, and even the array pointers.

Copying those into local variables, and marking the pointers `__restrict`, lets the compiler keep everything in registers. Renumbering went from about 29 to 22–25 ms, and the queries from about 25 to 22–23 ms. On the board, T went from 0.100 s to 0.096 s.

### Step 10: one cache miss per node
The renumbering pass visits nodes in random order. With separate offset and edge arrays, each visit cost two misses in a row. The four-slot rows described above make it one, and a prefetch can now fetch a node's edges directly from its id. Renumbering went from about 24 to 20 ms, and on the board T went from 0.096 s to 0.089 s.

To keep the empty-slot marker unambiguous, the head field is sized from n + 1, so no real edge word can be all ones. At the scored size that is the same 21 bits as before.

### Step 11: reuse pages instead of faulting in new ones
A profile showed the kernel's page-fault handling taking about 8% of all CPU time, about 37,000 faults per run. The id map and the forward offsets now take over the raw graph's pages, which are already in memory, and the rest of those pages are returned at once. The rename pass went from about 10.7 to 8.6 ms, and the memory peak did not move.

### Steps 12 to 17: three ideas the board did not reward
Each of these looked plausible but depended on the scoring machine itself, so the board had to decide:
- **Step 12** unmapped the input file during parsing, to shorten exit. It was slower on the board, probably because every unmap interrupts the other threads to flush their address caches.
- **Step 14** put the reverse graph in 2 MiB pages. The scoring CPU caches page translations for only 8 MB with small pages, so this looked promising. It measured the same speed, so the translation misses on the reverse graph were not what limited the searches.
- **Step 16** let threads spin briefly at barriers instead of sleeping, to avoid slow wake-ups from deep idle states. It was no faster.

Each was reverted. The repeat runs of step 11's code (steps 13, 15 and 17) also showed how much the board drifts: identical code measured 0.087 s in one session and 0.091 s in another.

### Steps 24 to 27: smaller distance labels, tried and reverted
A search spends much of its time reading and writing distance labels, so their size decides how many cache lines it touches. Almost every answer here is small: the median is about 1,200 and the 99th percentile about 8,500. So the searches now keep 16-bit labels, a quarter of the size of the 64-bit ones.

A 16-bit label saturates at 65,535. Any label below that is the length of a real path, so a result below it is exact. A query whose answer is 65,535 or more, or that has no path at all, is answered again with 64-bit labels. That is 16 of the 2,000 queries on the generated inputs, and they cost about 1.5% of the query time. The 64-bit search is created only for threads that need it. The query phase went from about 22.9 to 21.0 ms on our machine.

Both board runs of step 24 landed on a busy box, as Track A ran 6% and 10% slower than its usual time. Even so, one of them tied our best time. Step 25 is the same machine code with 8 header lines removed, since the other headers already include them in the pinned compiler image. Its run again came on a busy box, with Track A 20% slow, and again tied our best time.

Step 26 ran the same code on a quiet box, with Track A at its normal time, and measured 0.096 s. So the four board runs of the 16-bit version were 0.0854, 0.0855, 0.0949 and 0.0960 s, while step 23's code has measured between 0.0855 and 0.0874 s in every run. The final score is the middle three of five runs, so a version that is slow in half its runs is a bad trade, even with a faster best run. Our first wall-clock comparison had also shown it slightly behind overall, although its query phase was faster. There was no time left to find the cause before the freeze, so step 27 went back to step 23's code. It keeps only the header cleanup, which leaves the machine code byte for byte the same.

Besides the full test battery, the 16-bit version was checked on 60 new random graphs with answers around 65,535 and weights up to the largest 32-bit value, against a plain Dijkstra, at 1, 2, 5 and 12 threads.

### Steps 18 to 23: fewer instructions, fewer lines
The scoring CPU runs at a fixed 2.5 GHz, so instructions matter there. We counted them exactly with Valgrind on the sample input. The search's bucket drain executes about half of them. Three changes cut the total by 4.8%:
- **Rare paths out of line.** Growing a bucket, jumping to the far heap, and starting a new distance page are rare. Kept inline, they made the hot loop spill its own values to the stack.
- **No queue counter.** A count of queued entries, updated on every push, was only used to tell when the bucket ring was empty. The search now finds that out by scanning the ring, which it did already.
- **Simpler parser bookkeeping,** with no division per edge.

Its first board test, step 19, landed on a busy machine. Our Track A code, unchanged in that push, was also running 24–38% slower than usual. Repeated on a quiet box as step 21, it gave our best time so far, 0.0864 s.

Line count is the only part of the score that does not vary between runs, so steps 18, 22 and 23 went through the code for statements that could be written more simply without changing what runs:
- a plain sort instead of a stable one;
- a redundant edge-packing helper removed;
- merged early exits and one-statement setup code;
- a direct read of each query pair.

Instruction counts stayed the same, and together these steps removed 29 lines, from 607 down to 578 on the board's count.

---

## Things we tried that did not help

Each idea was measured against the version before it and dropped when it did not pay for itself.

**Search**
- **No contraction at all:** slower, and 16 MB more memory.
- **A binary heap instead of buckets:** about three times slower.
- **Other bucket widths (32, 128) and probe lengths (128, 512):** within noise of 64 and 256.
- **Pruning edges that cannot beat the best meeting point:** no gain.
- **Scheduling long queries first:** all threads already finish within about 1 ms of each other.
- **32-bit or 16-bit distances, with a 64-bit rerun for long queries:** no faster on an early version of the search. On the current one, 16-bit labels made the query phase 8% faster locally, but on the board they were fast in two runs and 11% slower in two others (steps 24 to 27), so we reverted them.
- **Label pages of 64, 256 or 512 entries for the 16-bit labels, and probe lengths of 64, 128 and 512:** within noise of 128 and 256.
- **Deeper prefetching in the search:** no gain, which showed the search is limited by its own instructions, not by memory.

**Renumbering**
- **An atomic operation on every claim:** correct, but 4 to 10 ms slower.
- **One byte per node for claims, or a private claim bitmap per thread:** no gain, and more memory.
- **Larger walk batches or other region sizes:** up to 2 ms faster in that phase, but not overall.
- **Letting threads take seeds from each other's range:** region sizes stayed just as uneven. They reflect thread speed, since two threads share each core, not idle time.
- **Renaming edges in equal slices of new ids:** slower, because slice boundaries no longer match the regions.
- **Three slots per node instead of four:** saves only 0.5 MB and sends 37% of the edges to a second cache miss.

**Memory and the kernel**
- **Releasing memory one page at a time:** thousands of small releases added about 8 ms.
- **Releasing the input bins in parallel instead of one unmap:** no faster.
- **Moving edge copies into the reverse graph with `mremap`:** 4,000 fewer page faults, but 4 MB more peak memory.
- **Huge pages for all of the final graph:** creating them cost more time or more memory than they saved.
- **Freeing everything in parallel before exit:** slower on the board in step 6.

**Build**
- **Training the profile on a full-size input:** no better, and a two-minute longer build.
- **Tuning for the scoring CPU's core type** (`-mtune=alderlake`): no difference.
- **A barrier that spins without limit:** much slower, because a spinning thread takes execution slots from its busy hyperthread sibling.

---

## Sizes checked against the scoring machine's caches

Our laptop has a 12 MB last-level cache, and the i5-13500 has 24 MB, so we checked whether our sizes were tuned to the wrong cache. Valgrind's cache simulator ran the full input with the target's caches (48 KB L1 data, 24 MB last level). We turned its counts into cycles with a rough model for a 2.5 GHz core: 2.5 instructions per cycle, 30 cycles for an L1 miss that hits a lower cache, and 200 cycles for a miss that goes to memory.

| Variant | Estimated cycles vs current |
|---|---:|
| Current code with a 12 MB cache, like the laptop | +6.1% |
| No chain contraction | +18.2% |
| Blob 2,048 / 8,192 nodes (current 512) | +3.1% / +4.4% |
| Bucket width 32 / 128 (current 64) | +3.0% / -0.1% |
| Chain-walk batch 4 / 16 (current 8) | about -2% / -1%, within the simulation's own noise |
| Blob 64 nodes, three runs each | -2.7%, from 6% fewer L1 misses |
| Blob 32 nodes | the same as 64 |

So the smaller laptop cache exaggerates memory costs by about 6%, but it did not mislead the big decisions: contraction is still clearly worth it on a 24 MB cache. The one promising size, 64-node blobs, lost 2 to 3 ms in two rounds of 20 interleaved wall-clock runs. The simulator counts misses but cannot see how many of them the processor overlaps, and smaller blobs restart the chain walks' prefetch pipeline more often. So we kept 512.

## Step 28: fewer instructions for a fixed-clock CPU

The board's 0.085 s for about 2.9 billion instructions means the six 2.5 GHz cores retire about two instructions per cycle each, across the whole run. So on the scoring machine the program is close to instruction-bound. On our laptop, which turbos to almost twice that clock, memory stalls looked like the bigger cost, and that view was misleading. Valgrind's per-line counts showed where the instructions go. The search's inner loop did about 50 per edge relaxed (18.7 million relaxations), and the parser about 108 per input line.

Three changes, each with the same answers and the same memory accesses:
- **A fast path for light edges.** An edge lighter than the ring's reach, which is every edge here except the 0.1% with escaped weights, cannot land outside the bucket ring when relaxed from the bucket being settled. So the ring-range check now runs only for heavy edges, out of line.
- **A sentinel bucket tail.** Every empty bucket points at a permanently full block 0. One "is the tail block full?" test then covers both the full and the empty case.
- **Digits converted with SSE.** The parser's three multiply-shift-mask steps per number became two instructions, `pmaddubsw` and `pmaddwd`, which x86-64-v3 includes.

Together they cut the whole program's instructions by 7.8%, from 2.90 to 2.67 billion, with the simulated cache misses unchanged. On the laptop the change was even, as expected when turbo makes instructions cheap. On the board, with Track A at its normal time, it ran 0.0849 s, our best yet.

Two follow-ups did not help. Copying the size globals into locals in the parser and the row builder saved only 0.1%, because the compiler had already kept them in registers. Storing both directions' labels side by side in shared pages, so that a relaxation reads the other side's label from the line it writes, cost 4% more instructions and 8% more L1 misses. The pages became twice as large, and filling them cost more than the shared line saved. Also, a missing output directory now ends with exit code 1 instead of a crash on the way out.

## How we measured

**The scoring machine.** It is an Intel i5-13500. Timed runs get its six P-cores with hyperthreading (12 threads), turbo off and a fixed clock of about 2.5 GHz. The input sits in the page cache before the clock starts, and memory is capped at 8 GB without swap.

**Our own machine.** We tested in Docker on a laptop, with the board's timer and its memory measurement (the cgroup's peak). Old and new versions always ran alternately, 20 to 30 times each, so both saw the same machine load. A profiling build printed each stage's time and memory, and a background sampler recorded which stage reached the peak. Valgrind counted instructions exactly. Perf sampled where the time went.

The laptop has only two P-cores among its twelve and runs with turbo, so it is a rough guide to the scoring machine. A change had to win locally and then hold up on the board.

**Reading the board.** Single runs vary by several percent, and the machine has busier and quieter hours. Our Track A code never changes between pushes, so its time in the same push works as a control: when Track A ran slower than usual, we did not trust the Track D result either.

## Correctness

Every version was checked before it was pushed:
- **41 small cases,** hand-made and random. They cover zero-weight cycles, parallel edges, self-loops, distances above 2^32, unreachable targets and isolated nodes. They run in a normal build and in one with address and undefined-behaviour checks. Since step 6 they also run many times in a row with 1, 2, 5 and 12 threads, because a race only shows up in some runs: about 1,500 to 2,500 runs per version.
- **Five full-size inputs** (seeds 1, 7, 17, 99 and 4242), compared with the official reference's output.
- **Nothing depends on a particular dataset.** Every size, the contraction, the fallback in the renumbering, and the thread count (up to 12) are worked out from the input and the machine at run time.
