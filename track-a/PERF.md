# Track A: performance log

## Local conversion baseline (2026-10-31)

C++20 conversion of the unchanged Python reference. Team: sweetdreams. No board run has been submitted; all numbers below are local Docker Desktop measurements, not official scoring results.

| Step | Change | Board run ID | Process time T (s) | Peak process RSS (MiB) | Local cgroup peak (MiB) |
|---|---|---|---:|---:|---:|
| 0 | Original Python 3.13 reference, preserved in dev/reference-a | Not submitted | 35.0851 | 212.0352 | 208.5000 |
| 1 | C++20; direct parsing, endpoint IDs, two-pass exact-sized uint32 storage, partial selection, buffered output | Not submitted | 4.0794 | 17.0000 | 17.5430 |

Approximately **8.60x faster**, **12.47x lower peak process RSS** (91.98% reduction), and **11.89x lower local cgroup peak** (91.59% reduction). No external runtime dependencies in either implementation. Source size/static-axis scores have not been measured, so no K or competition points are claimed.

## Parallel versions, Linux-native volume protocol (2026-10-31)

Steps 2 and 3 were measured with a different, lower-overhead protocol than steps 0-1 (input on a Docker named volume instead of a Windows bind mount), so their times are **not comparable with the table above**, only with each other. No board run has been submitted; these are local results, not official scores.

| Step | Change | Board run ID | Median T (ms) | Trimmed mean T (ms) | Max cgroup peak (MiB) | Code lines (main.cpp + Dockerfile) |
|---|---|---|---:|---:|---:|---:|
| 2 | Previous `main.cpp` (preserved as [dev/previous_main.cpp](../dev/previous_main.cpp)): 6 workers, per-worker two-pass count/scatter over contiguous chunks, global latency arena, per-hour merge, `nth_element`, whole output buffered | Not submitted | 168.17 | 174.42 | 36.95 | 431 |
| 3 | Hour-streaming rewrite (below) | Not submitted | 74.32 | 75.41 | 14.24 | 364 |

Step 3 vs step 2: **2.26x faster** by median, **2.59x lower** cgroup peak, 67 fewer code lines.

Step 3 design, each change kept only if it measured better (interleaved A/B, 15 runs each):

- Pass 1 indexes 32 KiB blocks in parallel: newline bitmasks from 64-byte AVX2 compares, recording which of the 72 hours each block holds. The hour is recomputed only when the 8-byte `MM-DDTHH` key changes. No endpoint parsing.
- Pass 2 hands out hours in order. A thread rescans only the blocks that hold its hour (4.7% overscan on seed 1), keeps (endpoint id, latency) for that one hour in an L2-sized buffer, bucket-sorts it in place and formats the lines. Memory scales with one hour per thread, not the whole file.
- Whichever thread completes the oldest unwritten hour writes every finished hour in order under a mutex, so no thread waits for its predecessor. Replacing an in-order offset chain removed most of emit's thread-time inflation.
- Output goes out with `O_DIRECT` in 4 KiB-aligned 512 KiB chunks; the sub-block tail and any filesystem without O_DIRECT fall back to buffered writes. On ext4 the 10 MB output otherwise stays charged to the container as dirty page cache: this cut the peak from 26.8 to 18.8 MiB.
- In-place cycle bucket sort instead of a separate scatter array: 18.8 to ~14.5 MiB, within noise on time.
- Endpoint names are copied into a compact per-thread arena and hash slots shrink to 8 bytes (L1-resident). Comparing against the first occurrence in the 239 MB mapping was the top sampled hotspot; fixing it gave -7.6% median time.
- Branch-free three-way multi-select finds p50/p95/p99 together (+3% slower with three `nth_element` calls). Page faults are left to run in parallel: serial `MAP_POPULATE` cost 14 ms and `MADV_POPULATE_READ` was slower still.
- Tried and rejected: `memcmp` for endpoint verification (+6.6%), fixed-stride name arena (no gain), SIMD latency parsing (no gain over a scalar loop, so the shorter code was kept).

Protocol for steps 2-3:

- Full seed-1 input copied into the `speeddemon-track-a-native` Docker volume and read once into page cache before each batch. Binaries built from the track's Dockerfile flags in the pinned `gcc:15` image.
- Time: the `dev/bench_timer.c` timer (CLOCK_MONOTONIC around fork/exec/wait, solution run as uid 65534), 21 strictly alternating old/new runs in one container, `--cpuset-cpus 0-11`, 8 GiB memory without swap, read-only root, no network, tmpfs `/out`. Every output was checked against SHA-256 `495ca096...1cd3`.
- Memory: 5 alternating runs per binary, each in a fresh container, with `/out` on an ext4 Docker volume so the output's page cache is charged as it would be on a real disk. Same timer, reading the container's `memory.peak` after the child exits. That includes ~5.3 MiB of container plus timer floor (measured with a no-op binary). Peak process RSS (~240-254 MiB for both) is dominated by the read-only file mapping, which is charged to whoever first cached the file, so it is not reported as memory.
- Code lines: `clang-format --style=LLVM` 23.1.3 (the harness's formatter), then non-blank, non-comment lines, an approximation of `scc`.
- Host: Intel Core Ultra 7 265U laptop (2 P-cores + 10 E-cores, 14 logical CPUs, power-limited), Docker Desktop WSL2 VM. The scoring box is a different CPU (6 P-cores with HT, turbo off), so absolute times and thread scaling will differ there.
- Raw runs: `dev/results/native/hour-streaming-vs-previous-time.txt` and `...-memory.txt`. Binary SHA-256: step 2 `6ee0f635...f616`, step 3 `2c3bf111...3436`. Source SHA-256: step 3 `main.cpp` `0c8714df...f95f`, step 2 `previous_main.cpp` `089fe5e3...6615`.

Verification for step 3: byte-identical output against the Python reference for full/sample/tiny seed 1, full/sample seed 17, tiny seeds 2, 17 and 2026, and the seven `dev/test-data` fixtures (worked example, official edges, empty, late endpoint, >16 MiB output, page-aligned EOF, randomized boundaries). AddressSanitizer + UBSan are clean on all of them except the two full-size inputs. `check.py` builds with the real Dockerfile and passes on full seed 1.

## Step 4: one-pass rewrite (2026-11-01)

Board context when this started: the top two Track A entries showed 0.016 s / 17 MB / 346 lines (K 56.9) and 0.018 s / 16 MB / 294 lines (K 55.7). Step 3's board numbers are not recorded here yet. Same native-volume protocol as steps 2-3; all numbers are local, and **no board run has been submitted for this step**.

| Step | Change | Board run ID | Median T, `/out` ext4 (ms) | Median T, `/out` tmpfs (ms) | Max cgroup peak (MiB) | Code lines |
|---|---|---|---:|---:|---:|---:|
| 3 | Hour-streaming two-pass design ([dev/hourstream_candidate.cpp](../dev/hourstream_candidate.cpp)) | Not submitted | 40.50 | 31.41 | 20.00 | 364 |
| 4 | One-pass design (below) | Not submitted | 20.61 | 19.08 | 25.03 | 358 |

21 alternating runs per binary for time; 5 fresh-container runs per binary for memory. Step 3's O_DIRECT output is much slower on a real ext4 output directory than on tmpfs, which the earlier tmpfs-only timing did not show. Step 4 trades about 5 MiB of memory for 1.65-1.97x less time, roughly +23% on K by the published weights before the board confirms it.

Design (one barrier-separated parallel region, a single read of the input):

- **Phase 1:** threads claim 1 MiB chunks and parse each line once. A 64-byte AVX2 space mask finds method and endpoint (one pass for 94% of traffic). Endpoints go through a per-thread table whose 16-byte slots point straight at a compact name copy. Each line becomes a 4-byte record (20-bit latency, error bit, 11-bit local endpoint id) in a per-(thread, hour) bucket. The ~1% of latencies of 2^20 - 1 and above go to a side list. Buckets grow in blocks carved from one `MAP_NORESERVE` pool per thread, sized by an order-independent worst case (every line in one bucket), with `MADV_NOHUGEPAGE`. Each parsed chunk's page-table entries are dropped with `MADV_DONTNEED` (clamped to EOF), in parallel, instead of serially at exit.
- **Merge** (barrier completion): local ids map to global ids in byte order, which is the output order.
- **Phase 2:** hours largest first (LPT; hours range 14-73k lines). Each hour's buckets are merged into an L2-sized scratch, the branch-free multi-select runs, and compact per-group stats plus the hour's exact output size are kept.
- **Phase 3:** `ftruncate` plus `MAP_SHARED`; every thread formats whole hours straight into the file at their offsets. No output buffer, no mutex, no O_DIRECT.

Measured along the way (local A/B, 15-21 runs; kept unless noted):

| Change | Effect |
|---|---|
| One pass with 4-byte records instead of two passes | 36.3 to 26.8 ms; memory 16 to 30 MiB |
| Bucket blocks from one pool per thread instead of 864 widely spaced regions (page tables) | memory 32.5 to 27.9 MiB |
| Single parallel region, LPT phase 2, mmap'd output instead of ordered O_DIRECT publishing | phase 2 ~10 to ~4.3 ms; 25.3 to 24.0 ms |
| Per-chunk `MADV_DONTNEED` of the input | -5%; time outside `main` ~4 to ~1.4 ms |
| Records released in parallel after the barrier instead of serial `munmap` in it | -4% |
| 64-byte space mask | -5.6% |
| Slot stores the name pointer (one fewer dependent load) | -3.6%, +0.5 MiB |
| Rejected: `pread` into L2 buffers instead of mmap | +5.6%, +3 MiB |
| Rejected: spinning barrier | +13% (spinners steal the hyperthread sibling of threads still parsing) |
| Rejected: 64-byte slots with a 56-byte inline name prefix | +6.7%, +3 MiB |
| Rejected: releasing records hour by hour in phase 2 | -4 MiB but +7.8% (one TLB shootdown per madvise) |
| Inconclusive, not adopted: SIMD latency parse, LUT digit formatting, `MADV_POPULATE_WRITE` on output, first-wave-only record release | within noise |
| Static link, 6-line Dockerfile | neutral on time, -5 lines |

Phase-1 floor: replacing the parser with a bare 32-byte load sweep over the same chunks takes ~6 ms against ~12.5 ms for the real parse (laptop), so parsing is still CPU-bound there. Memory at phase boundaries (`memory.current`): ~2 MiB at start, ~18 MiB after phase 1 (records), peak ~24 MiB late in phase 2 (+ scratch and stats), ~21 MiB at exit (output page cache replaces the records).

Verification for step 4: 17 datasets byte-identical to the Python reference. That is the 15 from step 3, plus a freshly generated full seed 99 and full seed 1 with every line shuffled (no time locality). AddressSanitizer + UBSan and ThreadSanitizer are clean on the small and medium inputs. `check.py` passes with the real Dockerfile. Raw timings: `dev/results/native/onepass-vs-hourstream-time-*.txt`. Every intermediate candidate is kept as `dev/onepass*_candidate.cpp`.

## Step 4 on the board, and step 5: removing branch mispredictions (2026-11-01)

**Board result for step 4** (as reported from the leaderboard; run ID not recorded): **0.026 s, 38 MB, 358 lines, K 37.87, 78.9 points, rank 32.** Leaders at the time: K 56.90 (0.016 s / 17 MB / 346 lines) and K 55.68 (0.018 s / 16 MB / 294 lines).

Two things the board showed that local runs had hidden:

- **Memory 38 MB versus 25 MiB locally.** The local Docker VM runs transparent huge pages in `madvise` mode; Debian kernels default to `always`. Under `always`, per-thread malloc arenas, thread stacks and the `calloc`'d stats table can each be charged whole 2 MiB pages. Step 5 calls `prctl(PR_SET_THP_DISABLE)` at startup. This is a hypothesis until the next board run: changing the VM's system-wide THP setting to reproduce it was not permitted, and locally the call changes nothing (25.0 MiB before and after).
- **The board is compute-bound.** It runs at a fixed 2.5 GHz without turbo, while the laptop boosts its cores. So cost is now measured as instruction count and simulated branch mispredictions: valgrind cachegrind, one thread, full seed 1, deterministic run to run. A simple model, wall ≈ (Ir / 2.5 + 17 × mispredicts) cycles / (2.5 GHz × ~7.5 effective cores) + ~3 ms, reproduces step 4's 26 ms (Ir 755M, 7.58M mispredicts).

| Change (cumulative) | Ir (M) | Mispredicts (M) | Notes |
|---|---:|---:|---|
| Step 4 as submitted (+ THP disable) | 755.6 | 7.58 | latency digit loop alone: 38% of mispredicts; `same()` length branch: 27% |
| SIMD latency parse; 64-byte `same()` with a `bzhi` tail mask; `clz`-based digit count; pair-table formatting; `&` instead of `&&` in the slot check | 693.4 | 3.49 | |
| Leaves of the multi-select (≤16 values, and whole small groups) by vectorised rank counting: 8 candidates per register, all values streamed past, the rank picked by mask compare plus `tzcnt` | 698.4 | 1.99 | selection had been 64% of all mispredicts (measured by removing it) |
| One endpoint table shared by all threads (CAS-claimed slots, release-published keys, 64-bit packed hash/len/id key), replacing per-thread tables and the serial 22k-lookup merge | 682.6 | 2.03 | -2.2 MiB, -10 lines; ThreadSanitizer clean |
| Single-pass partition: split off the pivot's copies only when nothing was smaller | 645.3 | 1.99 | the all-equal 383k-value group in `late-endpoint` still passes |
| 16,384-slot shared table (was 4,096) | 641.1 | 1.66 | fewer probe collisions; +96 KiB |
| Current hour's bucket cursor kept in registers | 627.6 | 1.66 | |

Total: Ir -17%, mispredicts -78%. The model predicts ~18 ms on the board, down from 26 ms. Laptop wall time (21 alternating runs, `/out` on ext4): step 4 + THP disable 22.2 ms, step 5 **18.2 ms**; total CPU 213 to 164 ms. Local peak memory 23.6 MiB (was 25.0). Code lines 416 (was 358).

Rejected in step 5:

- Scalar O(n²) rank counting for small groups (+37M Ir: the inner loop's varying trip count mispredicts).
- Histogram (bucketed) selection for groups of 512 or more (-0.6% Ir only).
- Inline 56-byte name prefixes in 64-byte slots (slower, +3 MiB).
- Branch-free fixed 10-digit formatting via a line buffer (+54M Ir).
- 96-byte endpoint-end window (no change).
- Releasing the first wave of hours' records early (-2.4 MiB but +9% wall, net negative K).

Verification for step 5: 17 datasets byte-identical, 3 runs each; ThreadSanitizer and ASan/UBSan clean on 13 small and medium inputs; `check.py` passes with the real Dockerfile. Candidates kept in `dev/` (`branchless_`, `vpick16_`, `shared_dict_`, `lomuto_`, `slots16384_`, `cursor_candidate.cpp`).

## Board results for step 5, and step 6 (2026-11-01)

| Board row (as reported; run IDs not recorded) | T | M | Lines | K |
|---|---:|---:|---:|---:|
| Step 4 + THP disable only | 0.026 s | 26 MB | 358 | 41.05 |
| **Step 5** (branch-free parse/select, shared table) | **0.020 s** | **24 MB** | 416 | **46.15** (rank 11) |

The THP hypothesis was right: 38 to 26 MB from one `prctl` line. Step 5's 26 to 20 ms came from 17% fewer instructions and 78% fewer mispredicts; the cost model predicted ~18 ms.

**What the two board points imply.** Fitting T = a + b · cycles to steps 4 and 5 gives a fixed, non-scaling part of roughly 5-9 ms, depending on the assumed IPC and mispredict penalty. Locally, `perf stat` puts 30-40% of CPU in the kernel: 11.3k page faults, mmap-lock contention, TLB-flush IPIs and page clearing. This share is the largest remaining unknown, and the VM (Hyper-V paravirtual locks) cannot reproduce the board's kernel behaviour, so only board runs can settle it.

Step 6 changes (instructions and mispredicts by cachegrind on full seed 1, one thread):

| Change | Ir (M) | Mispredicts (M) | Kept? |
|---|---:|---:|---|
| Step 5 baseline | 627.6 | 1.66 | |
| Rare paths marked `[[unlikely]]` (insert, hour change, large latency, block refill) | 617.0 | 1.73 | yes |
| Rare paths moved out of line (`probe`, `refill`, `keep_big`): the parse loop no longer contains calls, so GCC stops rematerialising its AVX constants every iteration | 598.0 | 1.66 | yes; phase 1 364 to 333M (107 Ir/line) |
| 512-record blocks (was 1024) | 598.7 | 1.67 | yes, -0.2 MiB |
| Scratch carved from each pool mapping, stats a static zero-page array: no `mmap` (address-space lock) inside the merge barrier | 599.6 | 1.70 | yes, -9 lines |
| R5 sample-then-filter selection for large groups (64-value sample, ±4 brackets) | 641.1 | 1.92 | no: 29% of brackets missed (±4 of 64 is ~1σ at the median) and the scalar count/copy loops did not vectorise |
| R2 two interleaved cursors per chunk | 684.0 | 1.69 | no: -3.7% laptop wall but +9% Ir; laptop E-cores run alone, while board threads share cores with a hyperthread, so the gain is not expected to carry over |
| Right-to-left branch-free number formatting | 667.2 | 1.50 | no: +40M Ir for -0.17M mispredicts |
| Fixed-shape small selection (8/16 steps, masked) | 654.2 | 1.35 | no: +27M Ir |
| `std::to_chars` instead of the pair-table `put` (-15 lines) | 604.3 | 1.81 | no: about +1% cycles for about +0.5% S |
| R1 `pread` into 256 KiB / 512 KiB buffers | | | not locally: faults 11.1k to 8.1k, but system time not lower and wall time slower. Kept as a board experiment. |

Measured split for step 6: phase 1 333M Ir, 0.45M mispredicts; phases 2-3 267M Ir, 1.25M mispredicts. Selection takes ~137M: groups over 256 values 61M (36 per value), groups of 16 or fewer 36M plus 0.44M mispredicts. Aggregation's two record passes take ~59M and formatting ~44M. Local peak memory 23.4 MiB (step 5 board version: 23.7 measured the same way). Lines 426.

Research mapping (RESEARCH_IDEAS_track1.md): R4 shared table done in step 5; R1, R2, R5 measured above; R6 superseded by the vectorised rank counter; R7 formatting is about 7% of Ir; R9's spin barrier was measured slower earlier (+13%).

Board experiments prepared and validated (17/17), because they hinge on kernel behaviour the VM cannot show:

- `dev/board_exp_nomadvise_candidate.cpp`: no per-chunk `MADV_DONTNEED`, so no TLB-shootdown IPIs, with teardown left to exit.
- `dev/board_exp_pread_candidate.cpp`: R1, no input mapping at all.

Verification for step 6: 17 datasets byte-identical, 3 runs each; ThreadSanitizer and ASan/UBSan clean on 13 small and medium inputs; `check.py` passes.

## Step 6 on the board, and step 7 (2026-11-01)

**Board run `run-d74d8f6c4bd8`** (commit `c07a262b`, pushed 2026-11-01 05:17:53Z): **T 19.544 ms, M 23.38 MB, S 426, K 47.10, 83.65 points.** Taken from the board's public feed (`teams[].tracks.a.latest`). The step 6 cost model had predicted ~19.4 ms.

The public feed keeps only each team's best and latest run, so the IDs for steps 4 and 5 cannot be recovered from it. They are on the board's per-commit results and will be added here. Every push from now on cites its ID.

Step 7 (cachegrind, full seed 1, one thread):

| Change | Ir (M) | Mispredicts (M) | Lines | Board run |
|---|---:|---:|---:|---|
| Step 6 (`run-d74d8f6c4bd8`) | 599.2 | 1.69 | 426 | 19.544 ms / 23.38 MB |
| Phase 2 counts and scatters by raw endpoint id and walks `sorted_ids` only when emitting stats; `rank_of[]` removed (one dependent load fewer on each of 6.2M record visits) | 594.1 | 1.67 | 422 | pending |

Ideas reviewed but not built:

- **Stats stored in an hour's dead record blocks.** A `Stat` (20-24 B) needs 5-6x a record's 4 B, so an hour with few lines but many endpoints would not fit, and an overflow path is needed. Stats inside the pool would also stop the pool being released before phase 3, putting records (12.4 MB) alongside the 10 MB of output page cache, a higher peak than now. Avoiding that needs a copy after freeing scratch, plus another barrier. Net estimate about +0.5% K for about 35 lines.
- **Vectorised name copy** that overruns into the next line: about 1.3M Ir. It needs a safe path at every hour's last line, because a different thread writes the neighbouring hour concurrently.
- **Division-free itoa:** about 7-15M Ir (1-2%) at real correctness risk. Not built yet.

Board experiments, regenerated on step 7 and validated 17/17: `dev/board_exp_nomadvise_candidate.cpp` (no per-chunk `MADV_DONTNEED`) and `dev/board_exp_pread_candidate.cpp` (no input mapping).

Verification for step 7: 17 datasets, 3 runs; ThreadSanitizer and ASan/UBSan clean on 10 small and medium inputs; `check.py` passes.

## Step 8: streaming-finalize architecture (2026-11-01)

Three alternative implementations from the team's notes (`tracka_ref.cpp`, `tracka_ref2.cpp`, `tracka_ref3.cpp`) were built with the track's flags and measured against step 7 with the same harnesses. All pass the 17 datasets (ref1 identifies endpoints by hash equality alone, which is not exact in general).

| Version | Ir (M) | Mispredicts (M) | Peak memory (MiB, `/out` ext4) | CPU (ms) | Lines |
|---|---:|---:|---:|---:|---:|
| Step 7 (two-phase, ours) | 593.6 | 1.67 | 22.8 | 242 | 422 |
| ref1 (per-hour block chains, full sort) | 823.8 | 23.65 | | | 338 |
| **ref2 (streaming finalize)** | **528.5** | **0.85** | **14.0-19.4** | **203** | 576 |
| ref3 (two-phase, chunks released in batches) | 746.7 | 3.52 | 21.8 | 264 | 327 |

Why ref2 is cheaper: the log is nearly time-ordered, so it finalises each hour once the finished file prefix is two hours past it, overlapping aggregation with parsing. If a line turns up after its hour was finalised, the whole run repeats with finalisation deferred to the end, so the result is always exact (the shuffled-input fixture exercises that path). Only a few hours of records are resident at once. Output is appended in hour order and its page cache is dropped after `sync_file_range` writeback, so the 10 MB output never counts in full. Its parser runs four interleaved streams per block with prefetch, SWAR latency decoding and per-thread mirrors of an append-only dictionary. Selection uses a vector compress-partition and a 16-lane bitonic network.

Local wall time: level with step 7 when `/out` is tmpfs (17.9 vs 17.2 ms), slower with `/out` on the VM's ext4 volume (36.3 vs 27.5 ms), where per-hour writeback goes to a slow virtual disk. It is also slower when the laptop is heavily loaded, because the pipeline waits on parse progress. The board's NVMe-backed output directory should be much kinder to the writeback, but only a board run can say.

**Step 8 `main.cpp`** = ref2 + `prctl(PR_SET_THP_DISABLE)` (ref2 mallocs per-hour run buffers from per-thread arenas, which is exactly the THP exposure that cost step 4 12 MB on the board) + argument paths (`/data`, `/out` by default). Ir and mispredicts are unchanged from ref2. Projected board K at about 15 MB and 582 lines: about 49 at 19.5 ms, about 52.6 at 17 ms, and about 46 if writeback costs it 22 ms. Step 7 is kept as `dev/onepass_step7_candidate.cpp`; revert to it if the board run is worse.

Verification for step 8: 17 datasets byte-identical, 3 runs; ThreadSanitizer and ASan/UBSan clean on 11 inputs including the rerun path; `check.py` passes with the real Dockerfile.

## Protocol and evidence

- Full seed-1 input: 3,100,000 records, 238,836,234 bytes.
- Input SHA-256: `7ec80ba45bceef51ac7f79d3a14521fc6187b2dcb87b5e5542d08063fe5d962a`.
- Every timed output matched SHA-256 `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.
- Docker Desktop Linux VM, 14 exposed virtual CPUs; both images pinned to VM CPUs 0-11. This does not identify physical P-cores. No governor/turbo changes.
- Same Windows bind-mounted input/output arrangement, memory limit 8 GiB, no swap/network, read-only root/input, unprivileged solution, 1 GiB /tmp.
- One correctness-checked warmup per image, then five alternating serialized runs. Initial timing batches exceeded the stability threshold and were rerun once for each image. Both final batches passed the middle-three spread threshold of max(5%, 50 ms).
- Time is CLOCK_MONOTONIC around fork/exec/wait, excluding image build and container startup/teardown. T is the mean of the middle three times. Memory is the maximum across the selected five-run batch.
- Process peak RSS uses Linux wait4/rusage. Local cgroup peak reads the container's memory.peak after the child exits; it includes the timer and charged cache/runtime memory. It is not the official parent-cgroup measurement after container removal.
- The earlier 36.73-second Python and initial 5.23-second C++ checker results included container startup and were single runs; they are not used for the ratios above.

Selected Python times (seconds): 35.0219, 34.5877, 34.7963, 36.5130, 35.4372.

Selected C++ times (seconds): 4.0896, 4.1680, 4.0071, 4.0221, 4.1266.

[baseline.json](../dev/baseline.json) retains exact times/memory, warmups, all initial and rerun batches, image IDs and source/input hashes. [validation.json](../dev/validation.json) records the 22 passing correctness/sanitizer checks. Full local output artifacts are under `dev/results/20261031T190620850154Z/` (git-ignored).

## Verification

- Published input/output hashes passed for tiny, sample and full seed 1.
- Differential checks passed for tiny seeds 2, 17 and 2026 and sample/full seed 17, against the original Python reference.
- Official worked example, official edge fixture, empty input and randomized cases passed.
- Randomized cases cover all 72 hours, all status values 100-599, maximum-length input lines, duplicate records, zero/uint32-max latency, small/large/equal percentile ranks and 64-bit error-rate multiplication.
- AddressSanitizer and UndefinedBehaviorSanitizer passed all cases except the two full-size datasets, which were checked with the release executable.
- Release and sanitizer builds completed with -Wall -Wextra -Wpedantic and no warnings after cleanup.

## Reproduce and next work

Follow [dev/README.md](../dev/README.md) to rebuild the images and rerun validation/benchmarks. Use the official `check.py` for normal edit/build/hash checks. Rebuild after every source change. Add actual board run IDs here when submitted; local results do not establish official T, M, source size, K or points.

The two-pass design deliberately limits latency storage to 12.4 MB at full scale. Next experiments should measure whether one-pass ingestion or parallel selection improves the time/memory tradeoff on the scoring machine. Preserve all correctness checks for each experiment.
