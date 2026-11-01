# Track C: Join (C++20)

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

`main.cpp` is built by the allowlisted GCC 15 image (`-O3 -march=x86-64-v3`, static) into the Debian runtime image.

1. **Lookup.** `regions.csv` becomes a dense array `region_id -> group index`. Any id outside the array, or with absurdly many digits, goes to `_UNMATCHED`.
2. **Chunking.** `records.csv` is memory-mapped. It is cut into about 8 chunks per thread, and each cut is moved forward to the first LF followed by a digit or a BOM. Spec 2.2 guarantees that is a record start: names never contain an LF, every note line starts with a lowercase word or a quote, and BOMs only sit at record starts.
3. **Parsing.** Worker threads claim chunks dynamically and run a hand-written scanner that follows spec 2.1. A BOM at a record start is skipped. Quoted fields end at the first `"` not followed by `"`, using `memchr`, and each `""` counts as one byte. Unquoted fields run to the next comma. Amounts are parsed as integer cents, and CRLF or LF ends a record. Count, sums, min/max and byte counts go into per-thread, per-group accumulators. After a chunk is parsed, its file pages are dropped from RSS with `MADV_DONTNEED`.
4. **Distinct names.** Each name is interned in one shared, lock-free open-addressing table. The key is the raw bytes between the quotes, still escaped. Escaping maps one-to-one onto the unescaped value, so this is exact, and both empty forms give the empty key. Because the name pool is about 100k values, the table quickly becomes read-mostly and fits in cache. Lookups are software-pipelined: a record prefetches its slot, 8 records later the arena entry is prefetched, and 8 records after that the comparison runs from cache. Each name id is appended to a per-thread, per-group stream. If a dataset has far more distinct names than the table was sized for, the run starts again with a table 8× larger. That never happens on spec data, but it keeps arbitrary inputs exact.
5. **Merge.** Groups are processed in parallel. Each group ORs every thread's id stream into a small bitset and counts the new bits. The numeric accumulators are summed.
6. **Output.** Keys are sorted bytewise (`_UNMATCHED` sorts last on its own) and the file is written.

Correctness was checked against the Python reference on tiny/sample/full seed 1 and sample seed 17, and on the spec's worked example. A build with a tiny initial name table was also run to exercise the rerun path.

## Dependencies

None (`dependency_count = 0`). The C++ standard library is statically linked.

## With another 24 hours

- Profile-guided optimisation (as in track A), and an AVX2 scanner that finds `"`, `,` and LF for a whole record at once in place of per-field `memchr`.
- Pin the thread count to the box's 6 P-cores × 2 and tune the chunk size on the real scoring box.
