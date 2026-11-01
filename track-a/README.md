# Track A: Ingest (C++20)

Reads `/data/requests.log` and writes exactly `/out/summary.txt`. The output follows the [official Track A specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Implementation

The Dockerfile compiles `main.cpp` with the allowlisted GCC 15 image (C++20, `-O3 -flto`, `-march=x86-64-v3`, i.e. AVX2, which the scoring i5-13500 has) into a static binary inside the allowlisted Debian runtime.

One parallel region (one thread per available CPU, at most 12), three phases separated by barriers:

1. **Parse once.** Threads claim 1 MiB chunks of the memory-mapped log. Each line becomes a 4-byte record (20-bit latency, error bit, 11-bit thread-local endpoint id) appended to a per-thread bucket for its hour; the ~1% of larger latencies go to a side list. Bucket memory is reserved for the worst case (every line in one bucket), so no input order can overflow it, and only touched pages are charged. Each parsed chunk's page-table entries are dropped right away so exit does not tear them down serially.
2. **Per hour, largest first.** After the endpoint names get global ids in byte order, each hour's buckets are merged into a cache-sized scratch array. A branch-free three-way multi-select finds the nearest-rank p50/p95/p99, and compact stats plus the hour's exact output size are kept.
3. **Write.** Hour sizes become file offsets, and the output file is sized and mapped. Every thread formats whole hours straight into it.

The hour comes from the timestamp's fixed offsets within the specification's interval (2026-10-30 18:00 UTC to 2026-11-02 17:59:59.999999 UTC). Input order is unrestricted, endpoints are discovered from the input (at most 2,048 per thread; the spec fixes 1,842), and duplicates count separately. Error rates use 64-bit intermediates. Empty input gives an empty file.

## Dependencies

No external runtime libraries: `dependency_count = 0`. Standard C++ runtime libraries are statically linked; libc is supplied by the runtime image. Team name is kept in `.speed-demon.toml`.

## Run from the workspace in PowerShell

Keep Docker Desktop running with Linux containers:

```powershell
cd C:\SWAPNIL\SpeedDemon
python .\speed-demon-starter\check.py a --data .\data\track-a\full-seed1 --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
```

The checker rebuilds the image and validates its output. Do not use `--skip-build` after source changes. The output is `speed-demon-starter/.out/track-a/summary.txt`.

See `../dev/README.md` for the repeatable correctness/sanitizer suite and five-run benchmark. The original Python starter is preserved in `../dev/reference-a/`, outside the scored track folder. `PERF.md` records measured results and their limitations.

## With another 24 hours

Measure on the board: local numbers come from a hybrid laptop CPU. Candidates still open are parsing two lines per iteration to break the line-to-line dependency, a parallel endpoint merge (~0.8 ms serial now), and a source-size pass toward 300 lines.
