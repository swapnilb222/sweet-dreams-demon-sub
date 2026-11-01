# Track C: Join (C++20)

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

`main.cpp` is built by the allowlisted GCC 15 image (`-O3 -march=x86-64-v3`, i.e. AVX2, static) into the Debian runtime image. One pool of threads (one per CPU in the affinity mask) runs three phases. Each phase is cut into pieces that any thread claims, and the next phase starts once every piece of the previous one is done.

1. **Regions, pass 1.** `regions.csv` is split by lines. The threads flag which country codes are present and track the largest region id, while also faulting in the shared tables with `MADV_POPULATE_WRITE`. One populate call per thread avoids page-fault lock contention. Countries are then numbered in code order, so the output needs no sort.
2. **Regions, pass 2.** The threads fill a direct table `region_id -> country number`, where 0 means "no such region" and maps to `_UNMATCHED`.
3. **Records.** `records.csv` is processed in ~1 MB pieces.
   - **Boundaries.** Each piece's thread finds its own boundaries: the first record start at or after its share of the file, meaning an LF followed by a digit or a BOM. By spec 2.2, an LF inside quotes is always followed by `[a-z"]`. Finding the cuts lazily matters: computing ~200 cuts up front faulted in a 64 KB window at each cut, for ~49 MB of RSS.
   - **Fields.** Field ends are found with AVX2 byte masks. Quoted fields are scanned by stepping through the bit mask of their quotes, where a `""` pair counts as an escape, so commas and LFs inside quotes cost nothing. Region and amount are found in one 32-byte compare. Numbers are decoded eight digits at a time with SWAR.
   - **Names.** A name is keyed by a 64-bit hash of its raw bytes between the quotes, together with its length. Escaping is one-to-one, so equal raw bytes mean equal values, and both empty forms give the empty key. The key's slot in a lock-free open-addressed table, probed four slots at a time with AVX2 and claimed with a CAS, is the name's id. With ~100k distinct names, the chance of a 64-bit key collision is about 3 in 10^10.
   - **Distinct names.** Each country has a bitset over name ids. An atomic OR that sets a new bit is a new distinct name, so there is no merge pass.
   - **Prefetching.** A 3-stage pipeline (16 records apart) prefetches the region row, the table slot and the bitset word.
   - **Memory.** Each piece is mapped with `MADV_POPULATE_READ` before parsing and released with `MADV_DONTNEED` after.
4. **Output.** Per-thread totals are summed and the lines printed.

If a dataset had more distinct names than the 1 MB table holds (114k), the run is redone with an 8× larger table. That never happens on spec data, but it keeps the answer right.

Checked against the Python reference on tiny/sample/full seed 1, sample seed 17, and the spec's worked example. A build with a 256-slot table was also run to exercise the rerun path.

## Dependencies

None (`dependency_count = 0`). The C++ standard library is statically linked.

## With another 24 hours

- Profile-guided optimisation, trained on a non-board seed.
- Tune piece size and pipeline depth on the real scoring box (6 P-cores, 12 threads).
