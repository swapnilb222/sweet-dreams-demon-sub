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
| 9 | Threads spawned as a tree before any page is faulted (a page fault was holding up thread creation for ~1.5 ms); shared tables on 3 transparent huge pages; input prefetched 4 KB ahead (1 thread: 91 → 85 ms); `-march=raptorlake` for the scoring box's i5-13500; code reorganised and compacted, S = 462 → 207 lines (local, 12 threads: ~20–23 ms, board-style M 11.2 MB) | | | | |

## How the numbers were taken

Tools live in `speed-demon-starter/dev-c` (every version `v4.cpp` … `v18.cpp` is kept there).

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
- 4 KB pages for the tables, populated in parallel: slower than huge pages.
- 256 KB pieces: more system time with no tail gain.
- `MADV_POPULATE_READ` per piece: no gain once the input is prefetched.
- Prefetch distances of 1, 2 and 8 KB: 4 KB was best.
- PGO, step 6: small and noisy.
