# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

Local numbers (in the notes) were measured on a laptop (Intel Core Ultra 7 265U, 14 threads, Docker Desktop) on full seed 1, with the data in a Docker volume, pinned to 12 CPUs (`--cpuset-cpus=0-11`). They are min/median wall times over rounds of 10 runs and are noisy (the host is shared). Fill in the board's T/M/run ID after each push.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C++ rewrite: mmap, parallel chunks synced on LF+digit/BOM, hand-written scanner, per-thread name tables + per-group bitsets merged by hash shard (local: ~0.24 s) | | | | |
| 2 | Shared lock-free name table (read-mostly, cache-resident) + per-group id streams in place of per-thread tables and bitsets; no merge phase (local: ~0.12 s, peak RSS ~52 MB) | | | | |
| 3 | Software-pipelined name lookups (prefetch slot, then arena entry, 8 records apart): single-thread parse 1.2 s → 0.43 s (local: ~0.10 s) | | | | |
| 4 | AVX2 field scanning (quote bit masks, one 32-byte compare for region+amount), SWAR number decoding, 3-stage pipeline (region row → name slot → bitset word), per-country bitsets with atomic OR giving distinct counts directly (no merge pass), regions parsed in parallel, tables pre-faulted with MADV_POPULATE_WRITE (local, 12 threads: ~55 ms) | | | | |
| 5 | Piece bounds found lazily by each worker: the up-front split faulted a 64 KB window at every cut (peak RSS 58 → 23 MB) | | | | |
| 6 | Name id = slot of its 64-bit key in a 4-wide AVX2-probed table (no separate key store: less cache pressure under 12 threads); write-hot counters padded off read-only lines; 1 MB pieces (local, 12 threads: ~25–30 ms, 1 thread: ~110 ms, S = 462 lines) | | | | |
| 7 | Batched structural scan: carry-less multiply turns the quote mask into an inside-quotes mask, so every record is five structural offsets with no branch on field shape; AES name hash over a masked 32-byte load (no byte loop); 8-slot cache-line buckets holding tag + dense 17-bit number (local, 1 thread: 127 ms; slower than step 6 until steps 8–9) | | | | |
| 8 | Always-8 offset stores (no mispredicted loop exit per 64-byte block); hot pointers copied out of the lambda's captures into registers; per-block quote rank instead of a quote count per offset; digits decoded four records at a time with `maddubs`/`madd` (local, 1 thread: 127 → 91 ms) | | | | |
| 9 | Threads spawned as a tree before any page is faulted (a page fault was holding up thread creation for ~1.5 ms); shared tables on 3 transparent huge pages; input prefetched 4 KB ahead (1 thread: 91 → 85 ms); `-march=raptorlake` for the scoring box's i5-13500; code reorganised and compacted, S = 462 → 207 lines (local, 12 threads: ~20–23 ms, board-style M 11.2 MB) | (commit 0a20695) | 0.023 | 11 | 82.87 (K 45.43, S 213) |
| 10 | Memory 11 → 8.9 MB (board-style `memory.peak`): bucket table halved to 1 MiB (8-slot buckets at 76% load; spills take the slow path); bitset laid out `[name / 64][group]` with one column per country present, so it grows with the names seen (~3 MiB) instead of a fixed 4 MiB; the two huge pages hold only buckets + bitset head (all used), and nobody else touches them while they fault (racing THP faults each charged a 2 MiB page: +4–8 MB peak); commas and LFs found with one `pshufb` lookup; distinct names by popcount after parsing (no stores between the locked ORs); smaller per-thread arrays; the last T MiB claimed in 256 KiB pieces; fixed a latent bug (a fresh block of name numbers looked used up, so spilled lookups wasted numbers). Local, 12 threads: ~21 ms, unchanged | (commit f984f5b) | 0.024 | 9 | 83.17 (K 46.06, S 220) |
| 11 | Batches of 16 blocks (1 KiB) instead of 32: per-thread batch arrays shrink from ~37 to ~20 KiB, because the box runs two threads per core on a shared 48 KiB L1; input pages dropped once per 4 MiB region by the thread finishing its last piece (a quarter of the `MADV_DONTNEED` calls, each a TLB-shootdown IPI to every core running the process). Local, 12 threads: CPU 279 → 256 ms (median of 5 interleaved rounds), board-style M 8.7 MB, S 221 | (commit 2975292) | 0.024 | 9 | 83.31 (K 46.36, S 227) |
| 12 | Same engine in fewer lines. On the board the source counts with the same weight as wall time (exponent 0.15 vs 0.5): at M ≈ 9 MB, cutting S from 227 to 150 is worth as much as a 1.7 ms faster run. Dropped the features that cost lines without a measurable gain: popcount after parsing (back to counting from `fetch_or`), last-T-MiB quarter pieces, per-region DONTNEED. Country numbering by `call_once`; threads in a `jthread` pool behind a start barrier; output via one buffer per line. Fixed during the rewrite: a GCC vector `==` on `__m256i` compares 64-bit lanes, not bytes; a `long < 2u` comparison sent sums to `max`. Local, 12 threads: CPU 222 ms (v25: 237), median wall equal; M 9.1 MB; S 184 + 6 (Dockerfile) | (commit 4a45427) | 0.024 | 9 | 83.75 (K 47.32, S 190) |

## How the numbers were taken

Tools live in `speed-demon-starter/dev-c` (every version `v4.cpp` … `v28.cpp` is kept there).

- `bench.sh rounds img...`: interleaved rounds of 10 runs of each image, pinned to 12 CPUs (`CPUS=2` for one thread). `runner.c` reports min/median wall, CPU, system time and peak RSS.
- `mem.sh img...`: the board's M. It warms `/data` into the page cache from another container first, as the harness's `prewarm` does, then reads the run's cgroup `memory.peak`. Page-cache pages of the input are charged elsewhere, so M is anonymous memory plus the container's own ~5.4 MB floor.
- `prof.sh img [cpus] [min%]`: `perf record` inside the container plus `perf annotate` of the worker, listing its hot instructions.
- `tools/sloc.sh file`: S as the board counts it, i.e. code lines after `clang-format --style=LLVM` (clang-format 23.1.3, as in the harness tools image).

## What the profiles showed

Single thread, v11 → v18, full seed 1:

| hot spot (share of samples) | v11 | v18 | fix |
|---|---|---|---|
| per-offset emission loop (loop exit mispredicts, quote count per offset) | ~15% | ~8% | always 8 stores; one rank query per record |
| captured variables reloaded from the closure in the hot loops | ~6% | 0 | locals |
| input loads (DRAM stream, one page walk per 4 KB) | ~16% | ~10% | prefetch 4 KB ahead |
| `lock bts` on the bitset word (distinct names) | ~12% | ~15% | none: it is a full fence and the floor of this design; private bitsets would cost 4 MB of M per thread |
| prefetch stalls on bucket / region row / bitset word | ~12% | ~12% | none: three random L3 lines per record, bound by fill buffers |

Phase timeline at 12 threads (instrumented build, `dbg13`/`dbg14`): thread creation was 2–5 ms before the tree spawn and the early barrier, and 0.3–0.8 ms after. The region phase is bounded by the three huge-page faults, about 0.2 ms on bare metal but up to ~2 ms in the Docker Desktop VM. Parsing takes ~17 ms, the tail ~1 ms, and output ~0.3 ms.

Tried and dropped:
- `pread` into per-thread buffers instead of `mmap`, to avoid faults and TLB shootdowns: system time doubled because of the kernel copy.
- 4 KB pages for every table: ~10% more CPU than huge pages.
- Pre-faulting the bitset for 100k names: no gain, +0.9 MB.
- 4 KB pages for the tables, populated in parallel: slower than huge pages.
- 256 KB pieces: more system time with no tail gain.
- `MADV_POPULATE_READ` per piece: no gain once the input is prefetched.
- Prefetch distances of 1, 2 and 8 KB: 4 KB was best.
- PGO, step 6: small and noisy.

## Where the time goes now (single thread, v22 profile)

- Input stream loads: 22% of samples. The structure pass is ~25 instructions per 64 bytes; the stall is the DRAM stream sharing fill buffers with the table prefetches.
- `lock or` into the bitset: 14%. It is a full fence.
- Name interning plus the bitset footprint: 20–25%, measured by knocking each stage out (the output is then wrong).

Board arithmetic, with K = (T_ref/T)^0.5 × (M_ref/M)^0.2 × (S_ref/S)^0.15: at M = 9 MB and S ≈ 220, 86 points needs T ≈ 19 ms.

## Research-guided experiment: partitioned distinct-name bitset (v24, dropped)

The literature on parallel aggregation says contended atomics on shared random data scale badly. The fix it gives is to partition by key, so that each partition's owner updates privately:
- **Privatization and partitioning:** Cieslewicz & Ross, *Adaptive Aggregation on Chip Multiprocessors*, VLDB 2007; Ye et al., *Scalable aggregation on multicore processors* (PLAT), DaMoN 2011; Kirk & Hwu on privatization of atomics.
- **Owner caches and batched messages:** Metreveli et al., *CPHash: a cache-partitioned hash table*, PPoPP 2012. Each core owns a slice of the table, and lookups reach it as batched messages.
- **Partitioning through cache-sized buffers:** Polychroniou & Ross, SIGMOD 2014; Balkesen et al., VLDB 2013.
- **Shared tables:** Xue et al., *Global Hash Tables Strike Back!*, 2025, find that a purpose-built shared table can match partitioning.

How v24 applied this:
- Each cache line of the bitset gets an owning thread, chosen by hash.
- Senders append bit indices to a 1024-entry ring per (sender, owner) pair, published every 16 batches.
- Owners apply them with plain ORs into their ~256 KiB share, which fits in L2. There are no atomics on the bitset.

Result:
- **Correct** on every dataset.
- **Single thread:** within noise of v23.
- **12 threads:** worse, with a median of 70–97 ms. Whenever a thread fell ~1.6 ms behind (descheduled, which happens on a shared host), every sender's ring to it filled and the senders stalled (2–5 × 10^5 full-ring polls per run).
- **Clean run:** ~21 ms, the same as v23.
- **Memory:** about +1 MB of rings.

Knocking out the atomic OR showed why: it is only ~5% of 12-thread time. Most of the cost is the three random table lines per record and the input stream, which partitioning does not remove. Xue et al. reach the same conclusion: for a single shared structure tuned to the task, partitioning does not pay. Dropped.

Also measured, all within noise locally: batches of 8, 16 and 32 blocks; ring publish intervals.

## Score arithmetic behind step 12

points = 50 log10(K), with K = (14.444 / T)^0.5 × (367.4 MB / M)^0.2 × min(108 / S, 1)^0.15.

At M ≈ 9 MB, the wall time T that gives 86 points:

| S (code lines) | T for 86 |
|---|---|
| 227 | 18.8 ms |
| 190 | 19.8 ms |
| 150 | 21.3 ms |
| 120 | 22.7 ms |

The board has measured 23–24 ms for every version since step 9.

## After step 12: experiments with no measurable gain

- **PGO at zero source cost.** Byte-identical copies of the published generator do not count toward S, so the build can generate a training set itself (`c_join.py --seed 7 --scale sample`), run an instrumented binary on it, and rebuild with `-fprofile-use` (`dev-c/Dockerfile.pgo2`). Correct on every dataset; locally within noise of the plain build, at 1 thread and at 12.
- **Explicitly unrolled offset emission:** 8 unrolled stores, then a loop only for further offsets, instead of "at least 8" iterations. Fewer instructions per block should matter when two hyperthreads share a core, but locally it is within noise.

Local timings on this laptop now swing 30–40% between runs (1 thread: 100–145 ms; 12 threads: median 28–30 ms). The 12 threads also run mostly on E-cores, unlike the box's 6 P-cores × 2. So remaining 5–10% effects can only be measured on the board itself.
