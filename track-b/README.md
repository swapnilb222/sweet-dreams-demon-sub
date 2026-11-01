# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Dependency-free C++20. The spec explicitly allows "indexing only the terms the queries use" (section 3). Read queries first, intern their terms, and build a token trie for their phrases. Eight workers stream disjoint, document-aligned sections of the corpus. Nothing derived from the corpus is stored in the image.

Boolean terms get sorted, deduplicated document postings. IDs are relative to each worker's document range: 16 bits when the range fits, otherwise 32 bits. A posting list becomes a bitmap when that representation uses less space. Boolean operations use sorted merges, bitmap operations, or bitmap membership filtering, always in the input's left-to-right order.

Phrase matching happens during tokenization. Active trie paths represent suffixes of the current document's tokens; an unknown token or a document boundary breaks the appropriate paths. Exact token comparison resolves hash collisions. Repeated occurrences in a document count once, including overlapping phrases. ASCII case folding, digit strings, and non-ASCII separator bytes follow the frozen spec exactly.

Workers summarize results in small batches. A locked merge combines counts, 64-bit sums, and the smallest ten IDs, independent of thread completion order. No raw corpus or full positional index is retained. Short tokens use a collision-free base-37 direct lookup.

## Build and run

```sh
docker build --network=none -t sd-track-b track-b
docker run --rm --network=none --read-only --user 65534:65534 \
  -e TRACK=b -v "$PWD/data:/data:ro" -v "$PWD/out:/out" sd-track-b
```

Create a writable `out` directory first. The binary defaults to `/data` and `/out`; for local runs it also accepts `solution DATA_DIRECTORY OUTPUT_DIRECTORY`. `SD_THREADS` overrides the default of eight workers for profiling. The Dockerfile pins allowlisted GCC and Debian images, builds offline, and statically links the program with the portable x86-64-v3 instruction baseline.

## Dependencies

Only the C++ standard library and platform threading support; no external runtime packages. `dependency_count = 0`, so D = 1.

## Validation and measurements

See [PERF.md](PERF.md) for measured versions, exact hashes, reproducible commands, limitations of local K estimates, and pending board results. The scripts and preserved experimental sources in [../dev-b](../dev-b) are outside the submitted track directory. The canonical reference remains in `../../speed-demon-2026/reference/b_index.py`.

## With another 24 hours

Measure the eight-versus-twelve-worker tradeoff on the scoring machine, then investigate allocation costs in the streaming scan. Keep a change only when its time and memory improvement outweighs its formatted-source cost in K. SIMD tokenization was tested, but was not consistently faster enough locally to justify the extra code.
