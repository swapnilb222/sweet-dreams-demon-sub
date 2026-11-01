# Track B: performance log

## Status: local profiling, 2026-11-01

Implemented and tested locally. **No live-board submission was made, and no official run ID, K, or points are claimed.** Add the actual run IDs and cgroup measurements below when this directory is submitted. The published reference is the baseline; it was not timed locally on the full corpus.

The selected implementation is v5: query-directed streaming C++20, adaptive sparse/bitmap Boolean postings, 16-bit local IDs with a 32-bit fallback, online token-trie phrase matching, short-token direct lookup, and eight workers. It has no external dependencies.

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

## Final five-run result and K estimate

| Run | Wall seconds | Peak RSS KiB | Correct hash |
|---:|---:|---:|---|
| 1 | 1.77 | 102552 | Yes |
| 2 | 1.00 | 102976 | Yes |
| 3 | 0.99 | 102976 | Yes |
| 4 | 0.95 | 102972 | Yes |
| 5 | 0.85 | 103028 | Yes |

Dropping the fastest and slowest gives **T = 0.980 s**. The maximum RSS is **105,500,672 bytes = 100.61 MiB**. These are local results; the coarse timing precision is insufficient to certify the harness's spread gate.

The official `static_axes.py` measured the working directory using the cached `speeddemon-static-cpp:latest` tools image: **S = 418** (410 C++ code lines after LLVM formatting, 8 Dockerfile lines), **D = 1**. Tools report scc 4.1.0 and clang-format 23.1.3. D is marked "declared, unverified" because this standard-library-only C++ build has no package manifest. The report is saved as [../dev-b/static-axes.json](../dev-b/static-axes.json); its commit field is the base checkout, and its notes correctly identify the uncommitted changes measured.

Using the published anchors `T_ref=56.352376`, `M_ref=2199961600`, `S_ref=91`, `D_ref=1`, an **illustrative mixed-environment estimate** is:

```text
K = (56.352376 / 0.980)^0.50
  * min(2199961600 / 105500672, 50)^0.20
  * min(91 / 418, 1)^0.15
  * (1 / 1)^0.15
  = 11.0753
```

**This is not a measured official K.** It combines a different CPU's wall time and process RSS with published scoring-box references. The board must supply the comparable T, cgroup M, official S, and run ID. No official points are reported.

## Profiling observations

An instrumented development copy of v2 at eight workers measured 1.138–1.698 s for each worker's scan/index/phrase phase, versus 0.057–0.065 s for Boolean queries and reduction. Each worker held about 9.16–9.20 MB of posting-vector capacity; 365–374 lists had promoted to bitmaps. The v3 instrumented batch measured 0.891–0.947 s scanning and 0.092–0.101 s querying. These batches were not isolated enough to infer a precise scanner speedup.

The useful next target is the scan and its allocations. A full positional index adds memory and construction work without being needed for the supplied query set. Narrow local IDs reduce sparse-posting storage while the bitmap switch prevents frequent terms from consuming long ID arrays. The query phase was already small, so further query machinery must justify its source-size penalty.

## Correctness and packaging checks

- All eight official hand-written test methods passed (seven execute the solution; one checks the spec example text).
- 24 randomized corpora, each with 200 mixed Boolean/phrase queries, matched the frozen reference. Cases include 32-byte tokens, absent terms, uppercase, Unicode separator bytes, repeated terms, and mixed operators.
- Generated tiny seeds 1, 2, 17 and sample seeds 1, 7 matched the reference at 1, 3, 8, and 12 requested workers: 20 executable comparisons.
- The above 51 executable comparisons also passed under AddressSanitizer and UndefinedBehaviorSanitizer, built with `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie`.
- Additional 65,536-, 65,537-, and 131,073-document cases passed at 1, 3, and 8 workers with both the final release binary and the sanitizer binary. These nine cases exercise the representation boundary, partial bitmap words, and sums exceeding uint32. Thus the final algorithm has 60 differential cases, with the same coverage checked under sanitizers.
- Full public seed 1 matched the published hash for every benchmark, including the exact container binary and its single-thread 32-bit fallback.
- Offline Docker build succeeded from the digest-pinned allowlisted images. Final image size: 121,523,814 bytes.
- Full-data container run succeeded with uid/gid 65534, `--network none`, `--read-only`, `--cap-drop ALL`, `no-new-privileges`, a 256-process limit, CPUs 0–11, and 4 GB memory with swap disabled. Only `results.txt` was produced in the output directory.

Final `main.cpp` SHA-256: `b3433575de7836b84a43ba90098982b2fdbfb0a49220f3edc06dce1c9c930f95`.

## Reproduction

From `speed-demon-starter`, under Linux/WSL with the sibling `speed-demon-2026` checkout and generated `../data/track-b/full-seed1` present:

```sh
mkdir -p dev-b/bin dev-b/results
g++ -std=c++20 -O3 -march=x86-64-v3 -pthread track-b/main.cpp -o dev-b/bin/local
REPEATS=5 python3 dev-b/bench.py dev-b/bin/local 8 12
python3 dev-b/verify.py dev-b/bin/local
python3 dev-b/boundaries.py "$(realpath dev-b/bin/local)"
docker build --network=none -t sd-track-b track-b
python3 ../speed-demon-2026/harness/static_axes.py --track-dir track-b \
  --tools-image speeddemon-static-cpp:latest --json dev-b/results/static.json
```

`bench.py` copies the dataset to the Linux temporary directory before measuring and verifies every output hash. Its generated `results/bench.jsonl` is local scratch; the completed session's measurements are preserved separately in `measurements.jsonl`. Use the official pinned static-tools image when available for the definitive S count. `profile.py dev-b/v2.cpp` or `profile.py dev-b/v3.cpp` regenerates the instrumented experimental copy; profiling code is not part of the submission.

## Official submission results

| Run ID | Commit | T (s) | Cgroup peak M | Official S / D | K | Gates |
|---|---|---|---|---|---|---|
| Pending submission | — | — | — | — | — | — |
