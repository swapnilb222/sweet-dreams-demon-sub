# Track B: performance log

## Status: 2026-11-02

The submitted version is **v16**: one scan of the corpus in phases of about 32 MB. Each phase is indexed at document level (lists of 16-bit ids, bitmaps for common terms), every query is answered on that phase into a running tally, and the phase is then dropped (see [README.md](README.md)). The previous submission, v12 (commit `4d0be41`), is the only one with a board result so far: **T 0.217 s, M 41 MB, S 802, D 1, K 25.42, speed score 70.26** (rank 29 when recorded; its run ID was not noted). v16 has no board result yet.

The v13 restructuring borrows its overall plan from a Rust Track B program we studied (`trackb_refm.rs`): phases with 16-bit postings; phrase matches written as rows of a pseudo-term, so that a phrase query becomes a one-term lookup; per-query tallies owned by one thread at a time (no atomics on results); answering phase k − 1 on the threads that have run out of scan work in phase k; row chunks per shard, sorted by claim order instead of sorting rows; a futex barrier that spins first. Unlike that program, lookups stay exact. It compares 47 bits of a hash and accepts a rare false match, whereas v13–v16 verify every hit (see v15 below). v1–v5 are the earlier posting-list design and v6–v12 the block design, both kept below for the record.

## Environment and method

- Windows host, Ubuntu under WSL2, Intel Core Ultra 7 265U, 14 visible logical CPUs. These are **not** the scoring machine's six P-cores. Local scheduling and load caused substantial variation.
- Official generator, full seed 1: 250,000 documents, 253,827,587 corpus bytes, 50,000 queries. Input files were copied into `/tmp/sd-track-b/data` on the WSL Linux filesystem before timing. Filesystem cache was reused; no cold-storage claim is made.
- `/usr/bin/time -f '%e %M'` measures complete executable startup through output and exit; container startup is excluded. Times are rounded to 0.01 s by this tool. Memory is **process peak RSS in KiB**, not the harness's cgroup `memory.peak`. No corpus mapping, input-derived build artifact, or on-disk index is used.
- Development compiler: Ubuntu GCC 15.2.0. v1 used `-O3 -march=native -pthread`; v2 onward used `-O3 -march=x86-64-v3 -pthread`, all C++20. Final measurements use the **actual statically linked GCC 15 Docker binary**, extracted from the built image, including `-DNDEBUG`.
- Every timed run checked the complete output SHA-256. All matched `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927`.
- Full raw measurements are preserved in [../dev-b/measurements.jsonl](../dev-b/measurements.jsonl). Sources `v1.cpp` through `v5.cpp`, benchmark scripts, and the static-axis report are in that development directory, outside the scored folder.

## Tested versions

Times below are all observations from the indicated batches, in run order. The memory column is the maximum RSS within that row. Early rows are exploratory single runs, not five-run aggregates.

| Version / local batch | Change | Threads | Wall seconds | Peak RSS KiB | Board run ID |
|---|---|---:|---|---:|---|
| Published reference | Python full index and candidate re-tokenization | 1 | 56.352376 on scoring box | 2,199,961,600 bytes, cgroup metric | Published calibration |
| v1 | Selective sorted uint32 postings; phrases matched during scan | 1 | 5.32 | 147604 | Not submitted |
| v1 | Same | 4 | 1.79 | 171196 | Not submitted |
| v1 | Same | 8 | 1.14 | 200728 | Not submitted |
| v1 | Same | 12 | 1.07 | 217592 | Not submitted |
| v2 initial | Adaptive bitmaps; compact Boolean-term numbering; batch result reduction | 1 | 7.98 | 108244 | Not submitted |
| v2 initial | Same | 4 | 1.92 | 121296 | Not submitted |
| v2 initial | Same | 8 | 1.06 | 135376 | Not submitted |
| v2 initial | Same | 12 | 0.84 | 147536 | Not submitted |
| v2 repeat | Repeat to assess noise | 8 | 1.47, 1.59, 1.60 | 135504 | Not submitted |
| v2 repeat | Same | 12 | 1.72, 1.39, 1.33 | 142908 | Not submitted |
| v3 initial | SIMD tokenizer and CRC-based hash; exact equality still checked | 8 | 2.44 | 130384 | Not submitted |
| v3 initial | Same | 12 | 1.35 | 149200 | Not submitted |
| v3 repeat | Repeat to assess noise | 8 | 1.15, 1.32, 1.34 | 135472 | Not submitted |
| v3 repeat | Same | 12 | 1.25, 2.07, 1.24 | 142928 | Not submitted |
| v4 | Restore scalar scan; 16-bit local postings with 32-bit fallback | 8 | 1.47, 1.08, 1.06 | 105296 | Not submitted |
| v4 | Same | 12 | 1.22, 1.12, 1.90 | 112852 | Not submitted |
| v5 | Add exact base-37 lookup for tokens of length 1–3 | 8 | 1.83, 0.86, 0.97, 0.91, 0.85 | 105496 | Not submitted |
| v5 | Same | 12 | 0.83, 1.03, 1.52, 1.24, 1.35 | 118168 | Not submitted |
| Final container binary | v5, GCC 15 static release build | 8 | 1.77, 1.00, 0.99, 0.95, 0.85 | 103028 | Not submitted |
| Final fallback check | Same, one shard larger than 65,536 documents | 1 | 6.99 | 105712 | Not submitted |

v2 reduced the eight-worker peak from 200728 to approximately 135500 KiB. v4 reduced that again to approximately 105300 KiB. These memory changes are more consistent than the local wall-time differences. Eight workers were selected because the v5 batch had both a lower trimmed time and lower peak RSS than twelve locally. Revisit this choice on the scoring box.

The SIMD experiment did not show a consistent enough benefit across batches to retain its additional code. It is preserved as v3 rather than described as an unconditional regression. No PGO was used.
## v6–v12: query-directed scan with block-local evaluation

v5 spent most of its time in `std::getline`, byte-at-a-time hashing, a `std::string` per dictionary slot and per-term posting vectors, and held ~100 MiB of postings. v6 onward never builds a global index: the corpus is scanned once and every query is answered on blocks of documents as they are scanned. Each row below is one tested step; all full seed-1 outputs matched `f86973c2…0927`.

Local measurements: same WSL2 laptop as above (Core Ultra 7 265U, 14 logical CPUs, hybrid P/E cores, 3.38 GHz TSC). Wall times are medians of interleaved runs (`dev-b/cmp.py`) unless a list is given; this machine's baseline drifts by ±15 % with thermal state, so only same-batch comparisons are meaningful. "Anon" is peak `RssAnon` polled by `dev-b/memwatch.py` (RSS would also count the mapped corpus in v6); "cgroup" is the container's `memory.peak` from `dev-b/dock.sh` (board sandbox flags, CPUs 0–11, the track's Dockerfile).

| Version | Change | Wall s (14 thr) | Memory | Notes |
|---|---|---|---|---|
| v5 (same batch) | baseline | 0.84–0.86 | 103 MiB RSS | |
| v6 | mmap, AVX2 bitmask tokenizer, exact 3-key buckets, prefetch ring; per-block (2048 docs) Boolean evaluation with small lists spilling to rows; guards and anchors from a 1/64 sample; phrases per document around anchors | 0.30, 0.30, 0.33 | 97 MiB anon | eval ≈ 400 M cycles/thread: every morsel ended a block (245 blocks × 12.5 k evals) |
| v7 | `pread` 256 KiB chunks (mmap faults cost 64 ms single-thread, 18 ms on 14 threads); blocks span a thread's morsels via a segment table; D = 8192; hot terms renumbered to rows, cold terms as branch-free pairs + counting sort; shared atomic results | 0.36, 0.20, 0.21 | 135 MiB RSS | evals/thread 300 k → 60 k |
| v8 | triggered queries evaluated in ascending order from a bitset; first list referenced, not copied; size-aware intersect/subtract | 0.17–0.19 | 124 MiB anon | list eval cycles halved |
| v9 | pairs store only term ids with a 0 per new document; staggered per-thread start of the query sweep (instead of 7 MB of per-thread result arrays); fixed 16-wide bit extraction | 0.167–0.174 | 86 MiB anon | experiment: removing shared atomics saved 30–40 M cycles/thread, staggering recovers most |
| v10 | D = 4096; cold-only dedupe array; reserved vectors; two-pass prefetched query parse | 0.163–0.177 | 59 MiB anon | D = 2048 / 4096 / 8192 at 12 thr: 37 / 46 / 64 MiB anon, same time within noise |
| v11 | parallel query parse into a per-thread-sharded dictionary; tree thread spawn; parallel query read; `mallopt(M_MMAP_THRESHOLD)` (freeing the query buffer had raised glibc's dynamic threshold: +7 MiB); capacity reserved once (growth remaps, `munmap` = TLB shootdown on all cores); `PR_SET_THP_DISABLE` | 0.176–0.19 | 52 MiB anon (D = 4096) | serial parse 10 ms → ~1–3 ms of work per thread |
| v12 = `main.cpp` | D = 2048, profiling removed, read buffer grows for over-long lines | median 0.176 | **cgroup 38–39 MiB** | container wall 182–211 ms |

Negative or neutral results kept for the record: branch-free union/difference (no change, the merges were not mispredict-bound); prefetch distance 16 vs 8 (no change); hot ratio N/64 vs N/32 (no change, kept 32); `MADV_HUGEPAGE` dictionary (dropped for THP safety, no measurable loss).

Single-thread profile of v11 (D = 4096, TSC cycles converted at 3.38 GHz): total 0.84 s; Boolean evaluation 0.23 s (1.25 M evaluations, list operands ~68 elements each); cold-pair counting sort 0.04 s; tokenizer stages measured separately (`dev-b/tokbench.cpp`): `pread` 26 ms, masks 4 ms, position extraction 50 ms, keys 38 ms for 36.5 M tokens; dictionary probes ~150 ms; per-token bookkeeping ~150 ms.

v12 on the board: T 0.217 s, M 41 MB, S 802, K 25.42 (score 70.26). Profiling v12 afterwards (phase timers, `dev-b/prof_patch.py`) showed where the rest went: 12 threads scaled poorly (1 thread 0.97 s, 12 threads 0.26 s locally); evaluation at block end was 25 % of single-thread time and grew 2.3× at 12 threads (2.17 M evaluations, shared atomic results bouncing between cores); setup before the scan took 16 ms.

## v13–v16: phases, per-query tallies, exact 64-bit tags

The corpus is cut into phases of about 32 MB (8 at full scale, ~31,000 documents each; a phase is split further if it would exceed 65,535 documents, so ids within it always fit 16 bits). Threads take 256 KB claims of the current phase from a counter. Per token the scan stores the term's action; at the end of each 8 KB batch, list hits go to 64 row shards in 32-row chunks headed by their claim number, and heavy hits are applied to the phase's bitmaps once per group of 64 documents (atomically only for the first and last group of a claim, which a neighbouring claim may share). After a phase, each shard's chunks are sorted by claim, counted and written as lists (duplicates within a document dropped against the previous entry), and the other bitmap set is zeroed. Then the threads answer every query on that phase, 64 queries per job, into a tally (count, 64-bit sum, ten smallest ids) that only one thread touches at a time. Phases are answered in order, so the first ten ids come out sorted with no merge. The answers of phase k − 1 run on threads that have no claims of phase k left.

Local measurements below are on the same laptop as above. It throttles under 12 threads and drifted by up to 2× during this session, so only interleaved comparisons in one batch are used. `dockcmp.sh` runs static binaries in the board's sandbox flags (CPUs 0–11, read-only, `/data` mounted) and reports the cgroup `memory.peak`. The Rust program is timed the same way as a yardstick, not as a submission. Single-thread cycle counts are TSC cycles (3.38 GHz) for the scan claims alone, taking the minimum of 7 interleaved runs (`dev-b/minrun.sh`).

| Version | Change | Result |
|---|---|---|
| v13 | phases, row shards, per-query tallies, pseudo-terms for phrases, futex barrier; dictionary of 7 32-bit tags per line with exact keys in a separate array; 64 × 64 bit transposition of heavy rows per group | correct first time (1 and 12 threads); 12 thr 0.26 s, no faster than v12 |
| knobs on v13 (single thread) | removing the per-token heavy-bit read-modify-write saved 20 % of scan cycles: non-heavy tokens all wrote one dummy word, a serial store-forwarding chain; skipping the separate key check saved 13 % (a second cache line per hit) | led to v14 |
| v14 | heavy hits buffered as (bitmap, doc) and applied per 64-document group, straight into the bitmaps (no transposition, no per-token RMW); three exact keys inline per line | single thread: scan 60 cycles/token (tokenize + key + hash 22, lookup 19, bookkeeping 19); container min 207 ms vs Rust program 165 ms, v12 233 ms; **47 MiB cgroup peak, over the 44 MB cap** |
| prefetch tests on v14 | no prefetch +30 % scan cycles; prefetch 16 ahead −12 %; 16 KB batches with prefetch 16 −15 % | lookup latency matters |
| v15 | the dictionary was 8 MB: shards were sized by term *occurrences*, and frequent words repeat thousands of times in the queries. Shards are now sized by distinct hashes (counted by each shard's owner after parsing), giving a 2 MB table at load ≤ 5/8. Four 64-bit tags per line; the hash of a key is a bijection of its first 8 bytes when the rest is empty, so for tokens up to 8 bytes an equal tag is an equal token; longer terms keep their second word beside the table and a bit in the line says to compare it; 17–32-byte terms also compare their tail. Parse buffers freed after interning; 16-bit sample counters; prefetch 16, 16 KB batches | 43.3 MiB cgroup peak |
| **v16** = `main.cpp` | 8 KB batches (the per-thread token arrays scale with them), 32-row chunks (less half-empty space per claim), unpadded tallies | **37.5 MiB cgroup peak**; container min / median 173 / 179 ms vs v14 180 / 186 and Rust program 131 / 148 in the same batch; natively at 12 threads v14 / v15 / v16 medians 0.169 / 0.171 / 0.176 s, within noise |

Row volume at full scale: 1.63 M rows and 1.42 M postings per phase (cold-term occurrences, duplicates included). Setup at 12 threads takes 15–25 ms locally: query parse 1–4 ms, interning 3–8 ms (page faults under WSL2 are slow; a longer barrier spin did not help), sample 2–3 ms, plan 1–2 ms. The Rust program's setup measured 10–15 ms on the same machine.

## Current result and K estimate (v16)

| Measure | Value |
|---|---|
| Container wall (local, 12 CPUs) | min 173 ms, median 179 ms (v12 in an earlier batch: 233 / 340) |
| Container `memory.peak` | 37.5 MiB (≈ 39 MB) |
| S (official `static_axes.py`, cached tools image) | **905** (898 C++ + 7 Dockerfile) |
| D | 1 |

If the board's time falls as much as the local container time did from v12 (about 25 %), T would be about 0.16 s and K ≈ (56.352 / 0.16)^0.5 × 50^0.2 × (91 / 905)^0.15 ≈ 18.8 × 2.19 × 0.71 ≈ **29** (score ≈ 73). This is an estimate, not a result; the board decides.

## Correctness and packaging checks (v16)

- `dev-b/verify.py`, `dev-b/boundaries.py` (65,536 / 65,537 / 131,073 documents, which also forces phases to split below 65,536 documents) and `dev-b/extra.py`: all pass.
- ASan + UBSan (`-fno-sanitize-recover`) on `extra.py`, `boundaries.py` and full seed 1; TSan on `extra.py`: all pass, no reports.
- Fresh full-scale seed 2026: output identical to the reference (`bbd7e637…`).

## Correctness and packaging checks (v12)

- `dev-b/verify.py`: the 8 official hand-written tests, 24 randomized corpora × 200 mixed queries, generated tiny seeds 1, 2, 17 and sample seeds 1, 7 at 1, 3, 8 and 12 threads — 51 comparisons, all identical to the reference.
- `dev-b/boundaries.py`: 65,536 / 65,537 / 131,073 documents at 1, 3, 8 threads (many blocks, sums beyond 32 bits).
- `dev-b/extra.py`: 17–32-byte terms and corpus tokens of 33–64 bytes, a 400 KB line (buffer growth), an empty corpus, one document on 64 threads, 9,000 documents with results longer than ten, duplicate lines.
- Fresh full-scale seed 2026 (generated locally, 609,815 distinct tokens): output identical to the reference (`bbd7e637…`).
- The same suites under ASan + UBSan (`-fno-sanitize-recover`) and under TSan: all pass, no reports.
- Five consecutive full seed-1 runs: identical hashes.
- Offline Docker build from the pinned allowlisted images; runs read-only as uid 65534 with no network; only `results.txt` is written. The `mkdir -p /data /out` line was removed: the mounts work without it.

## Reproduction

From `speed-demon-starter`, under Linux/WSL with the sibling `speed-demon-2026` checkout and the seed-1 data copied to `/tmp/sd-track-b/data`:

```sh
cd dev-b
bash mk.sh v16.cpp v16 && bash mk.sh v16.cpp v16s -static
SD_THREADS=12 python3 cmp.py 9 bin/v14 bin/v16      # interleaved, hash-checked
bash dockcmp.sh 5 bin/v16s                            # board sandbox flags: wall + memory.peak
python3 prof13.py v16.cpp bin/v16p.cpp && bash mk.sh bin/v16p.cpp v16p && SD_THREADS=12 bin/v16p /tmp/sd-track-b/data /tmp/sd-track-b/out   # phase timers
python3 verify.py bin/v16 && python3 boundaries.py "$(realpath bin/v16)" && python3 extra.py bin/v16
# v12 and earlier:
bash mk.sh v12.cpp v12                 # release flags of the Dockerfile
python3 cmp.py 9 bin/v12               # interleaved median, hash-checked
python3 memwatch.py bin/v12 /tmp/sd-track-b/data /tmp/sd-track-b/out
bash dock.sh v12.cpp 3                 # container wall + memory.peak
python3 verify.py bin/v12 && python3 boundaries.py "$(realpath bin/v12)" && python3 extra.py bin/v12
bash mk.sh v11.cpp v11p -DPROF && SD_THREADS=1 bin/v11p /tmp/sd-track-b/data /tmp/sd-track-b/out   # phase profile
```

## Official submission results

| Run ID | Commit | T (s) | Cgroup peak M | Official S / D | K | Gates |
|---|---|---|---|---|---|---|
| not noted (v12) | `4d0be41` | 0.217 | 41 MB | 802 / 1 | 25.42 (score 70.26) | passed |
| pending (v16) | — | — | — | — | — | — |
