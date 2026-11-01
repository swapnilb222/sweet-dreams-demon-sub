# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

The submission repo pushes to two board teams (SweetDreams and SweetDreamsHalloween), so most commits have two board samples. Every board run of track D is listed below. Run IDs, T, M, S and points come from the board's published `board.json`; S is the board's own count. Step 1's run ID is no longer published (only each team's best and latest runs are), and its numbers come from the leaderboard row at the time.

| Step | Change | Commit | Run ID | T (s) | M (MB) | S | K | Points |
|---|---|---|---|---:|---:|---:|---:|---:|
| 0 | The reference, unchanged | | | 72.69 | 893 | 43 | 1 | 0 |
| 1 | C++ port: parallel parse, CSR, bidirectional Dijkstra | 06837bc | (not published) | 0.447 | 169 | 350 | 13.00 | 55.70 |
| 2 | Chain contraction, BFS-blob renumbering, bucket-ring bidirectional search | bdfa5e8 | run-6bc981e03f03 | 0.1088 | 65.3 | 574 | 29.56 | 73.54 |
| 3 | Memory diet: bin arenas freed by part group, block-chained buckets, byte degrees | 5223e25 | run-aa3192aa6e1c | 0.1246 | 54.8 | 572 | 28.62 | 72.83 |
| 3 | (same commit, second team) | 5223e25 | run-db3c3a2d92ae | 0.1174 | 55.6 | 572 | 29.40 | 73.42 |
| 4 | Bin arenas unmapped between two fill passes, not while other threads work | 45e4d5b | run-157cf3e402b1 | 0.1103 | 55.2 | 571 | **30.39** | **74.14** |
| 4 | (same commit, second team) | 45e4d5b | run-6d55da2e3292 | 0.1378 | 55.1 | 571 | 27.21 | 71.73 |
| 5 | Chain contraction folded into the BFS renumbering pass (from trackd_ref3) | *to push* | *pending* | | | 556 local scc | | |

**Board noise.** The two runs of commit 45e4d5b differ by 25% in T (0.110 vs 0.138 s), and the two runs of 5223e25 by 6%. A single board run cannot tell apart changes smaller than that. Memory is stable to within 1 MB. Step 3 looked like a time regression on its two samples, but step 4 changed only the fill scheduling (locally neutral) and got both the best and the worst board time so far. So the honest reading of steps 2 to 4 is: memory -16% (65.3 to 55.2 MB, x1.036 on K), time within board noise. Final scoring takes five runs on a quiet box, which will settle T.

**Step 4 rationale.** Step 3 unmapped bin arenas while the other 11 threads were still filling the CSR. `munmap` takes the process's mmap lock for writing and sends TLB-shootdown IPIs to every core running the process, which stalls the other threads' page faults on fresh CSR pages. Docker Desktop's VM hides most of this, so it was the prime suspect for the step 3 samples. Step 4 fills the CSR in two passes, one per half of the parts, and unmaps each half's arenas between passes, while the workers are parked at the barrier. Local build peak: 47 to 49 MB, still below the BFS-phase peak (51 to 53 MB), so M is unchanged. Local time is unchanged as well (lap A/B, 10 interleaved runs: min total 102 for step 2, 103 for step 3, 98 for step 4).

## Step 3 in detail (local measurements, 2026-11-01)

Local protocol: Docker Desktop on an Intel Core Ultra 7 265U, 12 threads, input on a Linux named volume with a warm page cache, `/bench-timer` wall time and cgroup `memory.peak`. Earlier board runs came in at about 0.75 to 0.8x local time and about +2 MB of memory. These are **not** board numbers.

Final A/B: 25 interleaved runs of step 2 vs step 3 on full seed 1.

| | min T (ms) | median T (ms) | median peak (MiB) | max peak (MiB) | S (scc after clang-format) |
|---|---:|---:|---:|---:|---:|
| step 2 | 103.5 | 154.5 | 62.7 | 63.2 | 567 (board scc: 574) |
| step 3 | 99.1 | 153.0 | 53.0 | 54.7 | 565 (board scc: 572) |

The paired median time ratio is 0.979, so time is unchanged within noise. The peak falls by about 10 MB, which is worth about x1.035 on K from M alone.

### How the peak was found

The lap build (`dev-d/kit/lap.py`) now also prints cgroup `memory.current` and `memory.peak` at each phase boundary. A 0.3 ms sampler thread records which phase set the running maximum. RSS was misleading, because it includes the 92 MB file mapping, which the cgroup does not charge.

| Phase | step 2 peak | step 3 peak | What held it |
|---|---:|---:|---|
| build (bins + CSR) | 63 | 42-50 | bins 33 MB + CSR 27 MB |
| BFS renumbering | 57 | 51 | old CSR 27 + edge copies 16 + ord/ends 10 + bitmaps 3 |
| queries | 61* | 52 | graphs 42 + bucket vectors 10 + distance pages 7 |

\*Only after the build peak was fixed.

### Changes kept

1. **Bins in page chunks, freed by part group.** Each thread files its edges into 4 KiB chunks of 682 records (u32 edge word plus u16 tail offset), with tail parts of 2^14 nodes. Each eighth of the parts has its own mmap arena per thread. Once all parts in an eighth have their CSR rows written, the last thread to finish unmaps those 12 arenas. A first version freed every chunk page with `madvise`, which cost about 8 ms in TLB shootdowns (7.3K calls x 11 IPIs). Groups bring that down to 96 `munmap` calls. Build peak: 63 to about 45 MB.
2. **Block-chained buckets.** The queue holds at most about 3K live entries, but the 1,024 per-bucket vectors each kept their own largest capacity, about 100K entries per side. Entries now live in 256-byte blocks chained per bucket. A drained bucket's chain goes back to a free list in O(1). Entries are just the node id: a stale entry is skipped when the node's distance no longer falls in this bucket. Query-phase growth went from 16 MB to about 7 MB, and queries also got slightly faster.
3. **BFS bookkeeping.** The per-thread private claim bitmaps (2.6 MB) are gone. A claim bit lost to a racing store only creates an unreachable duplicate, which the code already tolerated. Per-node copied degrees are stored as bytes, with a side list for any degree of 255 or more, instead of u32 end offsets (saves 3.7 MB). `ord` and the degrees are freed as soon as they are used.
4. **Line trims.** The `SD_DATA`/`SD_OUT` environment fallback is gone (argv, else /data and /out). The contraction's escape-table dedupe is now one stable merge where the fresher entries win. `fprintf` output replaces the `to_chars` buffer. Unused includes and helpers are removed.

### Tried and dropped (measured)

| Idea (suggestion id) | Result |
|---|---|
| Pruning: do not queue v when `nd + other.lower() >= best` (D4) | queries 31.5 vs 31.3 ms: no gain, dropped to save lines |
| LPT / cost-ordered query scheduling (D4) | all 12 threads finish within about 2 ms of each other, so there is no tail to fix |
| 16-bit weights / escape-free layout (D3) | only 4,750 of 3.96M query-graph edges (0.12%) use the escape table |
| Binary heap instead of the bucket ring (line saving) | queries about 3x slower |
| Smaller rings (NB 128/256) to save bucket memory | no memory change; the cost was per-bucket capacity, fixed by item 2 |
| Freeing renumbering edge copies page by page | +2 ms, and that phase was not the peak |
| Drop the forward-only probe (PROBE = 0) | queries 39.7 vs 38.0 ms: probe kept |
| Chunk record count stored inside each 4 KiB chunk | parse 15 to 35 ms: 107 hot counters at page-aligned addresses fight over the same L1 sets. Reverted to a separate cursor array |

### Correctness

- All 41 `dev-d/data` cases pass (`dev-d/verify.py`), in both the release build and an `-fsanitize=address,undefined` build. The cases cover zero cycles, distances above 2^32, epoch rollover, isolated nodes and 30 random graphs.
- The full seeds match the Python reference: seed 1 `a23dbdf0...277e`, seed 17, and freshly generated seeds 7 (`66d63e1c...`), 99 (`9f1fa4f6...`) and 4242 (`c0b35c81...`). Inputs and oracles are made by `dev-d/kit/genseeds.py`.
- No warnings with the Dockerfile flags plus `-Wall -Wextra`.

## After step 4: experiments (local, 2026-11-01 evening)

Same protocol as above. Lap builds, 8 interleaved rounds per comparison, per-phase minimum deltas in ms. The baseline is step 4 (`dev-d/kit/w9l.cpp`).

| Variant | parse | fill | contract | BFS | reorder | reverse | queries | total min | total median | peak MiB | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| step 4 | 16.5 | 8.1 | 14.4 | 18.9 | 8.2 | 9.5 | 30.2 | 116 | 131 | 53 | baseline |
| no chain contraction | 15.4 | 9.0 | 4.3 | 24.5 | 9.7 | 12.6 | 36.2 | 118 | 144 | 69 | contraction pays: -13 ms median, -16 MB |
| contraction walks in lockstep batches of 96 (was 32) | 14.7 | 10.0 | 17.5 | 18.9 | 7.9 | 7.7 | 27.0 | 113 | 120 | 53 | within noise; kept 32 |
| BFS edge prefetch 12 nodes ahead (was 4) | 16.6 | 10.3 | 16.9 | 19.4 | 7.7 | 7.1 | 25.6 | 116 | 124 | 53 | no gain |
| BFS blobs of 4,096 nodes (was 512) | 15.3 | 10.5 | 17.3 | 18.7 | 8.6 | 7.7 | 26.7 | 109 | 128 | 53 | no gain |

These two comparisons were separate sessions, so compare rows only within the same comparison. The step 4 row of the parameter comparison was parse 17.1, fill 10.8, contract 15.8, BFS 18.1, reorder 7.7, reverse 7.8, queries 25.5, total min 109 / median 124.

Query workload facts (seed 1): 2,000 queries, 1,979 distinct sources, 1,980 distinct pairs. Batch sharing of searches from a common source (Orionet, below) has almost nothing to share here.

### Literature checked (Consensus search)

- Preprocessing indexes: contraction hierarchies (SPoCH, ICS 2025), hub and 2-hop labels (H2H, VLDB J. 2023; pruned landmark labeling, SIGMOD 2013), arc flags (Möhring et al., JEA 2007), transit nodes (Bast et al., 2007). They all trade minutes of preprocessing for microsecond queries. Inside one timed run of about 100 ms, their build cost cannot be repaid by 2,000 queries. Not pursued.
- Bidirectional search pruning and batch PPSP: Dong et al., "Parallel Point-to-Point Shortest Paths and Batch Queries" (Orionet), SPAA 2025, doi:10.1145/3694906.3743311. Its pruning rule was tried (D4 above) with no gain, and the batch sharing does not apply (no shared sources).
- Lightweight reordering: Drescher et al., "BOBA", arXiv 2306.10410. It derives locality from the order of the edge list. The generator shuffles edge lines and relabels ids with a random permutation, so the file order carries no locality. The BFS-blob order stays.
- Latency hiding: Zhi et al., "CoroGraph", PVLDB 2023, doi:10.14778/3636218.3636240. Interleaving independent traversals to overlap cache misses is already done for the contraction walks (32 in lockstep) and by software prefetch in BFS and drain. Deeper prefetch and larger batches were measured above with no gain.
- Cache-segmented CSR: Zhang et al., "Making caches work for graph analytics" (Cagra), 2017. The per-part CSR build in L2 is already this idea, applied to graph construction.

## Step 5: combining with `trackd_ref3.cpp` (local, 2026-11-01 night)

`trackd_ref3.cpp` is another full solution with the same overall design. As a whole it measured worse than step 4 on all three K axes. Interleaved plain release builds, 12 rounds, full seed 1:

| | min T (ms) | median T (ms) | median peak (MiB) | S (local scc) |
|---|---:|---:|---:|---:|
| step 4 (w9) | 118.0 | 128.5 | 53.0 | 564 |
| ref3 unchanged | 125.3 | 132.9 | 59.0 | 586 |

The paired ratio of ref3 to step 4 is 1.058. A per-phase lap profile (8 rounds, minimum deltas in ms) shows where each one wins:

| Phase | step 4 | ref3 | Winner |
|---|---:|---:|---|
| parse (SWAR vs SSE separator masks) | 13.3 | 16.3 | step 4 |
| CSR fill | 9.3 | 8.2 | ~ |
| contraction + BFS renumbering | 17.2 + 17.6 = 34.8 | 28.6 (fused) | **ref3** |
| rename into the final ids | 8.2 | 13.8 | step 4 |
| reverse graph (inbox transpose vs re-binning) | 7.2 | 13.4 | step 4 |
| queries (64-bit vs 32-bit labels, branch-free relax) | 25.1 | 23.2 | ref3? |

The two ref3 winners were grafted onto step 4 one at a time.

| Graft | Variant | Result | Kept? |
|---|---|---|---|
| Fused contraction: chain walks run while the BFS expands a node, batched 8 nodes at a time with the walks in lockstep. The separate `contract()` pass and its full graph scan are gone. | w10 | Lap A/B, four sessions, phase "contract + bfs", step 4 vs w10: 35.9 vs 28.5; 32.0 vs 33.8; 31.5 vs 32.6 (sweep session); on 6 cores 45.2 vs 45.0. Total min, step 4 vs w10: 112 vs 98, 113 vs 117, 148 vs 141 (6 cores). Time is neutral within noise. Peak unchanged at 52 to 53 MiB. **S 564 to 556.** | **yes** (S gain, no cost) |
| 32-bit distance labels (`Side<u32>`), with the query rerun on `Side<u64>` if any label would reach 2^32 - 1 | w11 | queries 27.2 vs 28.8 and 35.2 vs 34.8 ms (two sessions): no gain. Peak unchanged, because BFS renumbering sets M, not the query phase. S +15. | no |
| ref3's 128-unit buckets (SH 7), and 32 (SH 5) or 256 (SH 8) for comparison | w10 + SH | queries: SH6 30.7 / SH7 27.1 / SH5 38.2 in one session, then SH6 29.4 / SH7 29.9 / SH8 30.6 in the next | no (SH 6 kept) |

Measurement note. The host was 57% busy during this session (other desktop apps), and wall-clock medians swung by more than 20% between sessions. Cgroup CPU time (`cpu.stat usage_usec`) was just as noisy (medians 1,334 to 1,442 ms against minima of 910 to 1,008). Decisions therefore rest on per-phase minima repeated across sessions, plus one session pinned to 6 cores, which is less disturbed by the host.

Correctness of step 5 (w10): all 41 `dev-d/data` cases pass in the release build and in the ASan+UBSan build. Full seeds 1, 7, 17, 99 and 4242 all match the reference hashes. No warnings with the Dockerfile's `-Wall -Wextra -Wpedantic`.
