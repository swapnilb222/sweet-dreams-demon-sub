# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

Our best board run: **0.0856 s, 57.9 MB, 578 lines, 76.65 points**, against 72.69 s and 893 MB for the reference. The full story, step by step with every board run, is in [PERF.md](PERF.md).

## Approach

The job is to answer 2,000 shortest-path queries on a road network with 1.74 million nodes and 5 million weighted, one-way edges. Reading the input is part of the timed run, so most of the work goes into building a small, well-ordered graph very quickly. The search itself is a careful but standard bidirectional Dijkstra. Everything runs on 12 threads.

**Reading.** The input file is memory-mapped, and each thread parses its own slice of it, decoding numbers eight bytes at a time with bit arithmetic. Edges are filed into page-sized chunks by the id range of their tail. Each range's rows are then built separately, in cache. Every node gets four fixed slots for its edges, which covers almost every node in a road network, so a node's edges are one memory access away. The rare busier node points to a shared overflow area instead.

**Shrinking and reordering.** About 30% of the nodes only continue a road: at most two ways out, and not a query endpoint. The program removes them. When an edge leads into such a node, it follows the chain to the first node that has to stay and points the edge straight there, with the weights summed.

At the same time, each thread grows compact breadth-first regions and hands out new ids in visiting order, so nodes that are close on the map end up close in memory. That matters more than anything else here, because the searches spend their time waiting on memory. Chain walks advance in small batches so their memory requests overlap. One more pass renames the edges and counts each node's incoming edges, and a last pass builds the reverse graph for the backward searches.

**Searching.** Each query starts with a short forward-only search, which settles nearby targets on its own. Then it searches from both ends, always growing the side that has done less work, until the two fronts prove that the best meeting point is optimal.
- The queue is a ring of distance buckets, with a small heap for rare long jumps.
- Distances live in small pages created on first use and recycled between queries.
- The inner loop has no unpredictable branches: it always writes the better distance and always writes the queue entry, counting it only when the path improved.

**Details that added up.** None of these change the algorithm, but each removed a specific kind of waiting:
- Each edge is a single 32-bit word: the head node and its weight.
- Hot loops keep their values in registers. The compiler is told the arrays do not overlap, so it does not reload them after every store.
- Rare code paths sit outside the hot loops.
- The id map reuses pages that are already in memory, instead of faulting in new ones.
- Memory is released only between parallel phases, never while other threads are working.
- The binary is fully static and built with profile-guided optimisation. The profile comes from a small input made with the official generator, on a different seed from the scoring one.

**Correctness.** Answers are exact 64-bit distances, or -1 for no path. A race in the renumbering is allowed to happen because it is cheap, and it is caught afterwards: if it ever produced too many duplicate ids, the pass reruns with exact atomic operations. Every version passed 41 small edge cases, in normal and sanitizer builds, repeated with 1 to 12 threads, plus five full-size inputs checked against the official output. Nothing depends on a particular seed: every size and limit comes from the input at run time.

## Dependencies

None. Only the C++ standard library and Linux system calls.

The track directory also contains `d_traverse.py`. It is an unchanged copy of the official generator, used only at build time to make the small training input for profile-guided optimisation.

## With another 24 hours

- **Lower the memory peak.** About 53 MiB is close to the floor of this design, because three different phases each reach it on their own. Merging the edge-copy buffer of the renumbering into the final graph, and halving the search's distance pages with 32-bit labels, could together bring it towards 45 MB. That would be worth about 0.4 points.
- **A partial contraction hierarchy.** Contracting some of the nodes with three neighbours as well, with shortcut edges, would shrink the searches further. It needs a cheap way to choose which nodes to contract, so that the preprocessing does not cost more than it saves.
- **Better tools for the target machine.** Our laptop has mostly efficiency cores and runs with turbo, so it is a weak stand-in for the scoring machine's six P-cores at a fixed 2.5 GHz. Several ideas, huge pages and spinning barriers among them, could only be judged by pushing them to the board. A machine closer to the target would make those decisions faster and more certain.
