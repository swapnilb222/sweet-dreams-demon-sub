# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | C++ port: parallel parse, CSR, bidirectional Dijkstra (commit 06837bc, 350 lines) | | 0.447 | 169 | 55.70 |
| 2 | Chain contraction, BFS-blob renumbering, bucket-ring bidirectional search (commit bdfa5e8, 574 lines) | | 0.109 | 65 | ~73.5 (K 29.56) |
| 3 | Memory diet: freed bin arenas, block-chained buckets, byte degrees (this version, 565 lines) | *pending board run* | | | |

Fill in the Run ID column from the board; T and M for step 3 come from that run.

## Step 3 in detail (local measurements, 2026-11-01)

Local protocol: Docker Desktop on an Intel Core Ultra 7 265U, 12 threads, input on a Linux named volume with a warm page cache, `/bench-timer` wall time and cgroup `memory.peak`. Earlier board runs came in at about 0.75 to 0.8x local time and about +2 MB of memory. These are **not** board numbers.

Final A/B: 25 interleaved runs of step 2 vs step 3 on full seed 1.

| | min T (ms) | median T (ms) | median peak (MiB) | max peak (MiB) | S (scc after clang-format) |
|---|---:|---:|---:|---:|---:|
| step 2 | 103.5 | 154.5 | 62.7 | 63.2 | 567 |
| step 3 | 99.1 | 153.0 | 53.0 | 54.7 | 565 |

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
