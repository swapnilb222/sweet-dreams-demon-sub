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
