# Track D: performance log

Every step from the reference to the current version. Each step lists the live-board run that measured it.

| Step | Change | Commit | Run ID | T (s) | M (MB) | Lines | K | Points |
|---|---|---|---|---:|---:|---:|---:|---:|
| 0 | The reference, unchanged | | | 72.69 | 893 | 43 | 1.00 | 0.00 |
| 1 | C++ port: parallel parsing, CSR graph, bidirectional Dijkstra | 06837bc | not recorded | 0.447 | 169 | 350 | 13.00 | 55.70 |
| 2 | Chain contraction, locality renumbering, bucket-queue search | bdfa5e8 | run-6bc981e03f03 | 0.109 | 65.3 | 574 | 29.56 | 73.54 |
| 3 | Memory diet: bins freed as the graph is built, compact search queues | 5223e25 | run-db3c3a2d92ae | 0.117 | 55.6 | 572 | 29.40 | 73.42 |
| 4 | Bins freed between build passes instead of during them | 45e4d5b | run-157cf3e402b1 | 0.110 | 55.2 | 571 | 30.39 | 74.14 |
| 5 | Chain contraction folded into the renumbering pass | 6bbd254 | run-caef9c2ebef7 | **0.110** | **55.2** | **563** | **30.44** | **74.17** |
| 6 | Correctness fix in renumbering, profile-guided static build, parallel teardown, fused reverse pass | 93ab427 | run-7e04fad34160 | 0.113 | 55.4 | 569 | 29.99 | 73.85 |
| 7 | Step 6 without the parallel teardown; cheap racy claims again, with an exact fallback | 93a6c1c | run-b0e091cfc395 | 0.109 | 55.3 | 571 | 30.63 | 74.30 |
| 8 | Prefetched chain walks, branch-free relaxation | 240b78d | run-b11110b169c2 | 0.100 | 54.9 | 571 | 31.95 | 75.22 |
| 9 | Hot loops rewritten so the compiler can keep their values in registers | d88f3f8 | run-460785953528 | 0.096 | 57.3 | 578 | 32.25 | 75.43 |
| 10 | Raw graph stored as four fixed slots per node | 65cc5ce | run-4efb9302d30e | **0.089** | **57.8** | **593** | **33.25** | **76.09** |
| 11 | The raw graph's pages reused for the id map and forward offsets | 5491bc8 | run-940c564ae383 | 0.089 | 59.0 | 600 | 33.08 | 75.98 |
| 12 | The input file unmapped by the parsing threads, not at exit | 6c0a85d | run-225c1dc47a94 | 0.095 | 56.0 | 601 | 32.31 | 75.57 |
| 13 | Step 11 again: step 12 reverted | 11cca82 | not run separately | | | | | |
| 14 | Board test: the reverse graph in 2 MiB pages | 99aef00 | run-719b42059767 | 0.089 | 57.4 | 622 | 33.15 | 76.03 |
| 15 | Step 11 again: step 14 reverted | f09ad57 | run-4590a0e90760 | **0.087** | **58.0** | **600** | **33.63** | **76.34** |
| 16 | Board test: a barrier that spins briefly before sleeping | 67069f8 | run-8755815cfc16 | 0.087 | 58.1 | 610 | 33.57 | 76.27 |
| 17 | Step 11 again: step 16 reverted | 89b85fd | run-98427ea1df21 | 0.089 | 58.8 | 600 | 33.31 | 76.09 |
| 18 | The same program in 11 fewer lines | 53bbed8 | run-c9f3114356b5 | 0.089 | 58.2 | 589 | 33.48 | 76.19 |
| 19 | Board test: fewer instructions in the search and the parser | 30bba3d | run-af7a78fbfbdb | 0.094 | 56.1 | 596 | 32.61 | 75.62 |
| 20 | Step 18 again: step 19 reverted | a16b30e | run-3fc80c66055d | 0.088 | | 589 | | |
| 21 | Step 19 again, retested in a quieter period | 55eeb87 | run-86a463aba615 | 0.086 | 58.5 | 596 | 33.72 | 76.40 |
| 22 | Step 21 in 9 fewer lines | c09205a | run-0638cc93d96a | **0.086** | **58.9** | **587** | **33.76** | **76.42** |
| 23 | Step 22 in 9 fewer lines | pending | pending | | | | | |

Step 6 ran slower on the board than step 5 (0.113 s and 0.118 s in its two runs, against 0.110 s), even though it was faster on our machine. Step 7 removes the two parts of it that behave differently on the board's kernel. From the reference to step 5: about **660x faster**, **16x less memory**, and K from 1 to 30.4.

---

## How the solution works

The task is to answer 2,000 shortest-path queries on a road network of 1.74 million nodes and 5 million weighted, directed edges. The whole run is timed, including reading the input. So the real problem is building a fast graph quickly, not just searching it. The program runs in four stages, all on 12 threads.

**1. Read the graph.** The input file is memory-mapped, and each thread parses its own slice of it. Numbers are decoded eight bytes at a time with bit tricks instead of character by character. Each edge is filed into a bin by the id range of its tail. Then each id range's adjacency rows are built separately, small enough to stay in cache. While the rows are built, the program also notes which nodes have at most two outgoing edges and are not a query endpoint. These are the nodes the next stage can skip.

**2. Contract and renumber.** Road graphs are full of nodes that just continue a road, and about 30% of the nodes here are such pass-through points. Each thread grows compact breadth-first regions over the graph and gives new ids in visiting order. That way, nodes that are close in the road network also sit close together in memory, which matters a lot because the searches are bound by memory latency. While a node is expanded, any edge that leads into a pass-through node is followed along its chain to the first node that has to stay. The edge then points straight there, with the summed weight. Chain walks run in small batches that advance in lockstep, so their cache misses overlap instead of queuing one after another. The result is the forward graph in its new numbering, about 1.22 million nodes and 3.96 million edges. A reverse graph is then built from it for the backward searches.

**3. Answer the queries.** Each query runs a bidirectional Dijkstra search. A short forward-only search goes first, which answers nearby targets on its own. After that the search grows from both ends, always expanding the side that has done less work, until the two fronts prove that the best meeting point found is optimal. The priority queue is a ring of distance buckets, with a small heap for the rare very long jumps. Distances live in small pages that are created on first use and recycled between queries, so a search only touches memory near where it actually goes. Threads take queries from a shared counter.

**4. Write the answers** in input order and exit, leaving the cleanup to the operating system.

Each edge is packed into one 32-bit word: the head node in the low bits and the weight above it. The few weights too large for that field (about 0.1% after contraction) are kept in a small side table.

---

## What each step changed, and why

### Step 1: a C++ port
This replaced the Python reference with straightforward parallel C++. It was already 160x faster, but every query searched a large graph with random node ids, so nearly every step of a search missed the cache.

### Step 2: make the graph small and local
- **Chain contraction** removes about 30% of the nodes, and every search gets shorter by the same amount.
- **Breadth-first renumbering** puts neighbouring nodes next to each other in memory. Without it, the queries spend most of their time waiting on main memory.
- **A bucket queue** instead of a binary heap. Edge weights are small integers, so distances can be grouped into fixed-width buckets, which is much cheaper than heap operations.

Together these took T from 0.447 s to 0.109 s.

### Step 3: cut the memory peak
Memory counts toward K, and the reference uses 893 MB, so every megabyte saved here is worth almost as much as a millisecond. We measured the program's memory over time and found three separate peaks, then flattened each one:

- **While building the graph (63 MB):** the parsed edges and the new adjacency rows were both alive at once. Parsed edges now go into page-sized chunks, grouped by which part of the graph they belong to, and each group is released as soon as its rows are built.
- **While searching (61 MB):** the search queues kept the largest size they had ever reached in every bucket, about 100,000 entries per search direction, while only about 3,000 are ever live at once. The buckets now draw from shared, recycled blocks, so memory follows the live queue. This also made the queries slightly faster.
- **While renumbering (57 MB):** per-thread bookkeeping was trimmed. Node degrees are now stored in one byte instead of four, and per-thread bitmaps that were not really needed were removed.

The peak dropped from 65 MB to 55 MB.

### Step 4: free memory at the right moment
Step 3 released memory while other threads were still busy building the graph. Releasing memory briefly locks the process's memory map and forces every core to flush its address cache, which can stall the other threads on real hardware. The build now runs in two halves, and each half's memory is released between them, while the other threads are idle. Memory stayed at 55 MB, and the time loss seen on the board went away.

### Step 5: one pass instead of two
Contraction used to be its own pass over the whole graph, followed by the renumbering pass. Now the chains are walked during renumbering, at the moment a node is expanded. This removes one full scan of the graph and its code. Time and memory stayed the same, and the program got 8 lines shorter.

### Step 6: a correctness fix, then a faster build
*Board result: slower (0.113 s and 0.118 s). See step 7.*

**The fix.** A stress test found a real bug. We ran every small test case many times, with different thread counts and under the address sanitizer. On one 32-node case the answers came out wrong in some runs. The renumbering claimed nodes with a plain read and write instead of an atomic operation, which is cheaper. When two threads raced on the same word, a node could be claimed twice and end up with two new ids. We knew about that race and believed it harmless. The trouble was the count: on a graph with nothing to contract, the duplicate ids push the node count past what the head field of an edge word can hold, so an id spills into the weight bits. The full-size graphs never get near that limit, because contraction removes 30% of the nodes first. But a correct program must not depend on that. Claims are now atomic, so every node gets exactly one id. To keep that cheap, a claim first reads the word normally and uses the atomic operation only when the node still looks free. A plain atomic on every attempt cost 4 to 10 ms; this version costs about 2 ms. After the fix, 1,968 runs over all 41 cases, with 2, 5 and 12 threads, normal and sanitized, all gave the right answer.

**Profile-guided, fully static build.** The compiler now builds the program twice. The first build is instrumented and runs once on a small input made by the official generator, using seed 7 rather than the board's seed. The second build uses that profile to lay out the branchy loops (parsing, chain walks, the bucket drain) the way they actually run. The generator file is an unchanged copy of the published one, so it does not count as code. The binary is now linked fully static, so no dynamic loader runs before `main`. Locally, on the same machine and interleaved with the old version, this shaved about 2 ms off the minimum, with most of it in the renumbering pass.

**Parallel teardown.** When a program exits, the kernel frees all of its memory on a single thread, and the board's clock keeps running until that finishes. Now each thread hands its own share of the memory back while the threads are still running in parallel. It does this for the input file mapping right after parsing, and for the graph and search arrays right after the last query. The search state is also no longer freed by each thread as it finishes its queries, because freeing memory briefly stalls every thread that is still searching.

**One pass less for the reverse graph.** The pass that renames the forward edges now also counts how many edges arrive at each node, which is the first half of building the reverse graph. One full scan of the forward graph goes away. The reverse phase dropped from about 7 to about 5 ms, and the program got 8 lines shorter than it would otherwise be.

Locally (25 interleaved runs, all outputs checked), the old version measured 112 / 118 / 125 ms (minimum / first quartile / median), and this version measured 108 / 113 / 116 ms. Memory and line count are unchanged within 1 MB and 6 lines.

### Step 7: keep what helps, drop what the board did not like
Step 6 was 3 to 8 ms slower on the board, while every local measurement said it was faster. Two of its parts depend heavily on the kernel and hardware, so they are the likely cause, and step 7 removes both:

- **The parallel teardown.** When 12 threads free memory at the same moment, they compete for the kernel's page allocator lock, and each release interrupts every other core to flush its address cache. Our test machine runs a different kernel inside a virtual machine, so it could not show this.
- **Atomic claims.** Atomic operations are slow on the board's CPU at its fixed 2.5 GHz. Renumbering uses cheap racy claims again. Two checks keep that safe. If the duplicate ids a race can create would ever overflow the head field, or a thread's edge buffer, the pass runs again with atomic claims. On full-size inputs this never triggers, so it costs nothing. On the tiny test graphs it does trigger, and the stress test (2,460 runs over all cases, normal and sanitized, with 2, 5 and 12 threads) passes. A stress run also showed that the old code could overrun a thread's edge buffer on a tiny graph, and the second check covers that.

The profile-guided static build and the fused reverse pass stay. Locally, over 20 interleaved runs, step 7 measured 89 / 92 / 95 ms (minimum / first quartile / median), against 93 / 98 / 100 ms for step 5.

*Board result: 0.109 s and 0.113 s, level with step 5. The board's noise turned out to be larger than we thought: another team's identical runs went from 0.098 s to 0.114 s.*

### Step 8: two fixes in the hot loops
We compared each phase against another implementation of the same design. Two of our loops were slower, and each had a clear cause.

- **Chain walks waited on memory twice per hop.** Each round of walks fetched a node's row offset ahead of time, but not the row itself, so reading the edges still missed the cache. Now, at the start of each round, the walks first request the rows of the nodes they are about to read, and all of those misses overlap. The renumbering phase dropped from about 32 to 29 ms.
- **The search branched on a coin flip.** For every edge, the search asked "is this a shorter path?", and the answer is close to random, so the CPU guessed wrong about half the time. Now it always writes the new label as the minimum of the old and new values, and always writes the queue entry, counting it only when the path really was shorter. Nothing in the loop depends on the comparison any more. The query phase dropped from about 26.5 to 25 ms.

Locally, over 20 interleaved runs with the board's build, step 8 measured 85 / 87 / 93 ms (minimum / first quartile / median), against 94 / 97 / 101 ms for step 7. It is 2 lines longer.

*Board result: 0.100 s and 0.102 s, first place.*

### Step 9: let the compiler keep values in registers
A sampling profile showed no single hot spot, so we read the compiled loops instead. In C++, writing through one `u32` pointer may change any other `u32` in memory, unless the compiler can prove otherwise. Our hot loops store `u32` values (copied edges, queue entries) while reading `u32` arrays (row offsets, edges), and some of their counters also live in memory. So after every store, the compiler had to reload the row offsets, the end of the row, the edge counter, and even the pointers to the arrays.

The fix changes nothing about the algorithm. The loops now copy those pointers and counters into local variables, and mark the pointers `__restrict` (a promise that they do not overlap). Then the compiler can keep everything in registers. This touched four loops:

- **Renumbering** (BFS and chain walks): its edge counter was a reference to a field of the thread's region, so every edge copied meant a load and a store of it. The phase dropped from about 29 to 22–25 ms.
- **The search's bucket drain:** the end of each row and the best meeting distance were reloaded after every label and queue write. The query phase dropped from about 25 to 22–23 ms.
- **The rename pass and the parser:** smaller gains, about 0.5 ms each.

Locally, over 20 interleaved runs with the board's build, step 9 measured 81 / 84 / 88 ms (minimum / first quartile / median), against 86 / 90 / 93 ms for step 8. It is 7 lines longer.

*Board result: 0.096 s and 0.098 s. Memory read 57.3 and 58.7 MB, higher than step 8's 55 to 56 MB, although nothing in step 9 allocates anything new. Locally both versions peak at the same 53.8 MiB (56.4 MB), so we read this as variation in the board's measurement.*

### Step 10: one cache miss per node in the raw graph
The renumbering pass walks the raw graph, whose node ids are a random permutation, so almost every node it visits is a cache miss. With offsets and edges in two arrays, each visit costs two misses in a row: first the node's offset, then its edges. Chain walks pay this on every hop.

The raw graph now has no offset array. Every node owns four fixed slots, and its edges sit right there: an unused slot holds an all-ones marker, and four slots cover almost every node of a road network. A node with more than four edges puts a marker, a position and a count in its slots instead, and its edges go to a shared spill area. Visiting a node is now one miss, and the prefetches can fetch the edges directly, because a node's slot position is known from its id alone.

To keep the markers unambiguous, the head field is now sized from n + 1 instead of n − 1, so no real edge can be all ones. That costs nothing at the scored size: both give 21 bits.

The renumbering phase dropped from about 24 to 20–21 ms. Locally, over 20 interleaved runs with the board's build, step 10 measured 77 / 79 / 83 ms (minimum / first quartile / median), against 79 / 86 / 91 ms for step 9. It is 15 lines longer, and memory is unchanged.

*Board result: 0.089 s (run-4efb9302d30e) and 0.096 s (run-99192c439592). Both runs used the same commit. Their 8% gap is the board's run-to-run noise, which is why we judge every change on both runs and on local interleaved runs.*

### Step 11: reuse pages instead of faulting in new ones
A profile showed the kernel's page-fault path taking about 8% of all CPU time. Over a run the program writes about 140 MB of fresh memory: input bins, raw graph, edge copies, final graphs. Each new 4 KB page costs a fault, a zeroing of the page and a memory-accounting update, and threads faulting at once contend on kernel locks. The run took about 37,000 faults.

When the renumbering ends, the raw graph (about 28 MB, already faulted in) is freed. Right after that, the program used to allocate the id map (old id to new id) and the forward offsets, about 12 MB, and fault them in page by page. Now those two arrays are simply placed in the raw graph's old pages, which are already resident. The rest of the raw graph's pages go back to the kernel at once.

We also tried placing the forward edges there. That saved more faults, but it raised the memory peak by 3 MB. Those pages would all be resident from the start of the rename pass, while the edge copies are still alive, and normally the edges fill in as finished threads free their copies.

The new placement can only fit if the new graph is no larger than the raw one. That was already nearly guaranteed: contraction only removes nodes, and the exact fallback in the renumbering (step 7) now also covers the node and edge counts, not just the id field.

The rename phase dropped from about 10.7 to 8.6 ms, and memory is unchanged. Locally, over 20 interleaved runs with the board's build, step 11 measured 80 / 90 / 93 ms (minimum / first quartile / median), against 86 / 94 / 97 ms for step 10, on a slower stretch of the machine. It is 7 lines longer.

*Board result: 0.0893 s (run-940c564ae383) and 0.0909 s (run-2bf1b13d4cae), against 0.0894 s and 0.0964 s for step 10. Both runs were fast this time, so the average improved by about 3%, but the best run did not, and the board shows each team's best run. Memory read 58 to 59 MB, while our local peak stays at about 56 MB. The memory controller charges pages to a process in per-CPU batches, so when many threads fault at once the measured peak can read a few MB high. We cannot control that from the program.*

### Step 12: unmap the input file in parallel
The program's own phase timers cover about 5 ms less than the board's clock, which also includes process start-up and exit. Exiting has to tear down every mapping. That includes the 92 MB input file, whose 23,000 mapped pages each need their page-cache reference released, on one thread, while the clock runs.

Now each parsing thread unmaps its own slice of the file as soon as it has parsed it, so the work is done in parallel and early. The file stays in the page cache. Measured as the time from the end of `main`'s work to process exit, over 20 interleaved runs, this cut the median from 4.8 to 3.9 ms. Over 30 interleaved runs with the board's build, step 12 measured 74 / 82 / 94 ms (minimum / first quartile / median), against 77 / 84 / 96 ms for step 11. It is 1 line longer.

*Board result: 0.095 s and 0.108 s, slower than step 11 in both runs. Other teams' runs were also slower than their best at that time, so part of this was the board. But unmapping during the parse makes every thread interrupt all the others to flush their address caches while they are still parsing. On real hardware that can cost more than our virtual machine shows, and step 6, which did the same, was also slow on the board. Step 13 reverts it.*

### Step 13: back to step 11
The code is exactly that of step 11, the version with the best board average so far (0.0893 s and 0.0909 s).

### Step 14 (board test): huge pages for the reverse graph
The scoring CPU's second-level TLB holds 2,048 page translations. With 4 KB pages that covers 8 MB, while the searches hop at random over 41 MB of graph, so most hops also miss the TLB and wait for a page-table walk. With 2 MiB pages the whole graph fits in the TLB.

Huge pages are switched off for the whole process from the start (step 3), because partly used buffers would round up to 2 MiB and raise memory. They are allowed only for the reverse graph's offsets and edges, and only for the 2 MiB-aligned body of each array, so rounding wastes nothing. Each array is faulted in by all threads at once, each taking whole huge pages. The offsets are faulted in before the rename pass, which zeroes them anyway. The edges are faulted in once the edge copies are gone. That keeps the memory peak where it was. The forward graph stays on small pages: its offsets reuse the raw graph's pages (step 11), and faulting its edges early would raise the peak.

On our laptop this is about 3 ms slower, but there every new huge page also faults in the virtual machine's host, which the scoring machine does not have. Memory is unchanged and it is 22 lines longer. If both board runs are slower than step 11, we revert it.

*Board result: 0.0890 s (run-719b42059767) and 0.0907 s (run-f60f3a5c7751), against 0.0893 s and 0.0909 s for step 11. The same speed within noise, so TLB misses on the reverse graph are not what limits the searches on that CPU. With 22 more lines it scores lower, so step 15 goes back to step 11's code.*

### Step 16 (board test): spin briefly at barriers
The program has nine parallel phases, and between them the worker threads wait at a barrier while the main thread does short serial work. The standard barrier puts a waiting thread to sleep in the kernel. The scoring machine fixes its clock speed, but idle cores can still enter deep sleep states, and waking one takes tens of microseconds, up to a few hundred, about 18 times per run.

The new barrier spins for about 2,000 `pause` instructions (tens of microseconds) before it sleeps. Most waits end within that window, so the thread never sleeps. A long wait still sleeps, so a waiting thread does not take execution slots from its busy hyperthread sibling for long. Spinning without a limit was much slower in step 7's experiments for exactly that reason.

Locally it is level with step 11 over 30 interleaved runs, but our virtual machine does not show the host's sleep states, so only the board can tell. It is 10 lines longer. If both board runs are no faster than step 11, we revert it.

*Board result: 0.0870 s (run-8755815cfc16) and 0.0907 s (run-6b2fd972effa). Step 11's code ran just before it, as step 15, at 0.0870 s (run-4590a0e90760) and 0.0877 s (run-851c8ae1bb48). So the barrier did not help, and step 17 reverts it. The step 15 runs also show how much the board drifts: the same code measured 0.0893 s and 0.0909 s an hour and a half earlier.*

### Step 18: the same program in fewer lines
Line count is the one part of the score that does not vary from run to run, so we went through the code for statements that could be written more simply without changing what runs:

- The escape tables are sorted with a plain sort. Each slot appears once, so the stable sort and its custom comparison were not needed.
- The file mapping no longer skips the second `mmap` for an empty file, because that call simply fails on its own.
- The exact rerun of the renumbering resets its state with one assignment each, instead of loops.
- A few short setup statements share a line, and the output checks for a failed write once, when the file is closed.

Instruction counts on the sample input confirm the program does the same work (310.3 million instructions, against 311.0 million before). All tests pass, and it is 11 lines shorter: 582 locally.

*Board result: 0.0885 s (run-c9f3114356b5) and 0.0889 s (run-c052275c3715), with 589 lines. Six board runs of step 11's code now average 0.0888 s, so the speed is the same, as intended.*

### Step 19 (board test): fewer instructions in the hot loops
Valgrind's instruction counts show the search's bucket drain running about half of all instructions. Three changes cut the board build's total by 4.8% (295 million instructions on the sample input, against 310 million):

- **Rare paths out of line.** Growing a bucket by a block, a jump to the far heap, and a new label page are rare. Inlined, they made the compiler keep their values in registers too, so the hot loop spilled its own values to the stack. Now they are separate functions.
- **No queue counter.** Each push and each drained entry updated a count of queued entries, used only to tell when the ring of buckets is empty. The search now finds that out by scanning the ring when it looks for the next bucket. It already scanned there, so this costs at most one full pass when a search runs dry.
- **Simpler parser bookkeeping.** The parser no longer counts each part's edges with a division by the chunk size per edge. The counts follow from the chunks at the end.

The query phase got about 1.5 ms faster in the per-phase timings, but our laptop's full timings were level. Its cores and clock differ from the scoring machine's, so the board decides. It is 7 lines longer. We keep it only if both board runs beat step 18's.

*Board result: 0.0944 s (run-af7a78fbfbdb) and 0.1077 s (run-8fe3dcef2fc3), both slower than step 18. But the machine was noisy during these runs: our Track A code, unchanged in this push, ran 1.24 and 1.38 times slower than its own best in the same two runs. So this result cannot tell whether the change is slower. Step 20 goes back to step 18, the known-good version. Track A, whose code never changes between our pushes, works as a control from now on.*

*Step 20 confirmed the noise: step 18's code ran at 0.0966 s and 0.0877 s, with Track A at 1.19 and 1.42 times its best. Step 21 retests step 19's code after the board had been idle for half an hour.*

*Step 21 result: 0.0864 s (run-86a463aba615) with Track A at 1.01 times its best, so the machine was quiet. That is our best run, and faster than any run of step 18's code (best 0.0870 s). The other run, 0.0895 s (run-809644ef242e), had Track A at 1.18, so the machine was busy. We keep step 19's changes.*

### Step 22: step 21 in fewer lines
More statements written more simply, with the same instructions executed (295.4 million on the sample input, as before):

- The search's queue type gets a short name, and the far-heap jump in `lower()` is a single assignment.
- A new label page is set up in its return statement.
- The solver's two early exits, for s equal to t and for an endpoint with no edges, share one test.
- The query reader pushes each pair directly: C++ evaluates a braced list from left to right, so the two numbers are read in order.
- The edge count is a sum over each thread's part counts.

All tests pass, including the stress runs, and it is 9 lines shorter: 580 locally.

*Board result: 0.0864 s (run-0638cc93d96a, Track A at 1.09 times its best), with 587 lines: 76.42 points, our best so far. The other run, 0.0922 s (run-6266a5c0d81b), had Track A at 1.15 times its best.*

### Step 23: more of the same
- The helper that packed an edge with a heavy weight is gone. The parser already stores such an edge with an all-ones weight field, which is exactly what the helper rebuilt, so the build pass now just stores the word and records the weight in the escape table.
- The worker hand-off `par()` is a single statement.
- A claim that finds its node taken, and an exact claim, now share one test.
- The reverse fill stores an edge and checks it for an escape in one statement.

The sample input runs 296.1 million instructions, against 295.4 million: no measurable difference. All tests pass, including the stress runs, and it is 9 lines shorter: 571 locally.

---

## Things we tried that did not help

Each idea was measured against the version before it and dropped when it did not pay for itself:

- **Skipping contraction altogether.** The search got slower and memory rose by 16 MB, so contraction stays.
- **Pruning the search** so it does not queue nodes that cannot beat the best meeting point. No measurable gain.
- **Scheduling long queries first.** All threads already finish within about 2 ms of each other.
- **32-bit distances** in the search, with a 64-bit retry for the rare long queries. No speedup, and the memory peak happens elsewhere.
- **A plain binary heap** instead of the bucket queue. About three times slower.
- **Other bucket widths, larger walk batches, deeper prefetching, bigger renumbering regions.** All within measurement noise.
- **Releasing memory one page at a time.** Each release costs a little, and thousands of them added about 8 ms.
- **Training the profile on a full-size input.** No better than the small sample, and it makes the build two minutes slower.
- **An atomic operation on every claim attempt.** Correct, but 4 to 10 ms slower than reading first.
- **Huge pages (2 MiB) for the final graph.** The query phase got about 10% faster, because the searches miss the TLB less. But creating the huge pages either cost 4 to 5 ms elsewhere or, when created early, raised the memory peak by 3 to 7 MB. On our virtual machine each new huge page also triggers faults on the host, so we could not measure the real cost.
- **Prefaulting the input file's pages in one call per thread.** The kernel already maps the file 16 pages per fault, so this saved only about 1,400 faults.
- **Restrict-qualified pointers in the build and reverse-fill loops.** No measurable change, so we left those loops as they were.
- **Renaming edges in equal slices of new ids.** The renumbering regions differ in size by up to 2x, so the rename pass looked unbalanced. Equal slices were slower, though, because their boundaries no longer match the regions, and many more edges then crossed between slices.
- **Letting threads take seeds from each other's id range.** Region sizes stayed just as uneven, which showed that they reflect thread speed (two threads share each core), not idle threads.
- **16-bit search labels.** Another implementation uses them. In our tests its query phase was slower than ours.
- **Profile training on a full-size input, and tuning for the scoring CPU's core type** (`-mtune=alderlake`, same instruction set). Neither was measurably faster than the plain sample-trained build.
- **Freeing the graphs and search state in parallel before exit.** The time to exit dropped by about 1 ms in the median, but not in the first quartile. Step 6 tried the same and the board ran it slower, so we left it out.
- **Moving the edge copies' pages into the reverse graph with `mremap`,** instead of freeing them and faulting in new ones. It saved 4,000 page faults, but the copies then had to live until the whole rename pass ended, and the peak rose by 4 MB.
- **Releasing the input bins in parallel** (each thread its own, with `MADV_DONTNEED`) instead of unmapping them serially (about 2 ms between the build passes). It was no faster: the kernel's page freeing contends about as much as the serial unmap costs.
- **One byte per node for claims** instead of one bit, so threads share fewer cache lines. No gain, and 1 MB more memory.
- **Deeper prefetching, or clamped look-ahead, in the search.** No gain. The search is limited by its own instructions, not by waiting on memory.
- **Other bucket widths (32, 128) and probe lengths (128, 512).** All within noise of the current 64 and 256.
- **Copying the edge-field width and masks into locals in the hot loops,** the way step 9 did for pointers. The compiler was already keeping them in registers: the disassembly has the same number of loads.
- **Larger chain-walk batches (16, 32) and other BFS region sizes (256, 1024).** The renumbering got up to 2 ms faster with bigger batches, but the total was no better over 20 interleaved runs with the board's build.
- **Cutting instructions in the search loop.** The board's CPU runs at a fixed 2.5 GHz with turbo off, so we counted instructions exactly with Valgrind's cachegrind on the sample input. The search's bucket drain executes about half of all instructions, about 80 per edge it relaxes. Moving its rare paths (growing a bucket, a far jump, a new label page) out of line, dropping a queue counter that every push updated, and a cheaper parser bookkeeping step cut the total by 4.8%, for 11 more lines. The phase timers showed the query phase about 1.5 ms faster. But the full profile-guided build was not faster over three sets of 25 to 30 interleaved runs, so we did not ship it. The board's time is only about 1.1 times our in-program time, which points to waiting on memory, not to arithmetic, as what dominates there.
- **Three slots per node in the raw graph instead of four.** The input's out-degrees are 1 to 4 for 97% of nodes, with an average of 2.9, so four slots look wasteful. But three slots save only 0.5 MB and push 37% of the edges into the spill area, where each needs a second cache miss. Four slots (28.8 MB) are about as small as the raw graph gets: offsets plus edges would be 27 MB.
- **A private claim bitmap per thread** during renumbering. It saves a little time, but adds 2.6 MB in that phase, which would become the new memory peak.
- **A spinning barrier between phases** instead of the standard one, which sleeps. Much slower locally (median 127 ms against 101 ms), because a spinning thread takes execution slots from its busy hyperthread sibling.

---

## How we measured

The scoring machine is an Intel i5-13500. Timed runs get its six P-cores with hyperthreading (CPUs 0 to 11), turbo is off, and the governor is fixed, so the cores run at about 2.5 GHz. The input is loaded into the page cache before the clock starts, and memory is capped at 8 GB without swap. Our laptop (Core Ultra 7 265U) has only two P-cores among its twelve cores and runs with turbo, so local timings are a rough guide. We trust a change only when it wins clearly in local interleaved runs and then on both board runs. Nothing in the program depends on the dataset's seed: every size, the contraction, and the fallback in the renumbering are derived from the input at run time.

Changes were tested locally in Docker on a 12-thread laptop, with the input on a Linux volume and the file already in the page cache. We used the same timer and cgroup memory peak as the board. A profiling build prints the elapsed time and memory at each stage, and a background sampler records which stage reaches the peak. Old and new versions always ran alternately, so both saw the same machine load. We compared each stage's best time across several sessions.

Single board runs vary a lot. The same commit has measured anywhere from 0.110 s to 0.138 s. The table above lists the best run of each step. Memory is stable to within about 1 MB.

## Correctness

Every version was checked before it was pushed:

- 41 small hand-made and random cases, covering zero-weight cycles, parallel edges, self-loops, distances above 2^32, unreachable targets and isolated nodes. They pass in both the normal build and a build with address and undefined-behaviour checks. Since step 6, they are also run many times in a row with different thread counts, because a race only shows up in some runs.
- Five full-size inputs (seeds 1, 7, 17, 99 and 4242), compared against the official reference output.
- Nothing in the code depends on a particular seed. Contraction, renumbering and all sizes are derived from the input at run time.
