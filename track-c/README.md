# Track C: Join (C++20)

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

`main.cpp` is built by the allowlisted GCC 15 image, statically, with `-O3 -march=raptorlake`, because the scoring box is an Intel i5-13500 (Raptor Lake: AVX2, AES-NI, PCLMUL, BMI2, no AVX-512). It runs one thread per CPU in the affinity mask, which is 12 on the box (6 P-cores × 2 hyperthreads; one thread per core measured slower).

1. **Start-up.** Both files are mapped, each followed by 64 KB of zero pages so wide loads may run past the end. All threads meet at a barrier before any page is faulted in, because a page fault holds up thread creation (that cost 1.5 ms).
2. **Regions.** Each thread parses a slice of `regions.csv` into a direct table `region_id -> group`, where 0 means `_UNMATCHED`. A country gets a group number the first time it is seen, through `call_once`. Meanwhile two threads fault in the two huge pages that hold the name buckets (1 MB) and the start of the bitset. No other thread touches them yet: racing huge-page faults each allocate, and are charged for, a 2 MB page.
3. **Records.** Threads claim 1 MB pieces of `records.csv`. A piece begins at its first record start: an LF followed by a digit or a BOM. By spec 2.2, an LF inside quotes is always followed by `[a-z"]`. Each piece is walked in 1 KB batches (the two hyperthreads of a core share its 48 KB L1, so each thread keeps its batch arrays small), in five passes:
   1. **Structure.** For each 64 bytes, AVX2 builds masks of quotes and of commas and LFs (one `pshufb` lookup). A carry-less multiply of the quote mask by all ones gives the inside-quotes mask (`""` toggles twice, so escapes need no handling). Each record is exactly five structural offsets. Each block's quote mask is kept, so the number of quotes before any offset is one popcount.
   2. **Numbers.** The digits before each field end are gathered and decoded four records at a time with SIMD multiply-adds. The amount's dot is squeezed out first.
   3. **Names and lengths.**
      - The raw bytes between a name's quotes are hashed with AES-NI from a masked 32-byte load. Escaping is one-to-one, so equal bytes mean equal values, and both empty forms hash as empty.
      - A quoted field of `r` raw bytes holding `q` quotes has a value of `r - q/2 - 1` bytes.
      - The name's bucket and the region row are prefetched.
   4. **Totals and name numbers.** Totals are per thread and group, and merging them is only sums and maxima (min is stored as `BIAS - amount`). The hash's top 14 bits pick a 64-byte bucket of eight slots. Each slot is a 47-bit tag above a 17-bit dense name number, claimed with one CAS, which gives 61 bits of identity. A name that spills past a full bucket is found on the slow path. The bitset word is prefetched for writing.
   5. **Distinct names.** An atomic OR sets bit `(group, name)` in a bitset laid out as `[name / 64][group]`, so its memory grows with the names actually seen (~3 MB). A bit that was clear is a new distinct name.

   The input is prefetched 4 KB ahead. A parsed piece's pages are dropped with `MADV_DONTNEED`, so exit does not unmap ~50,000 pages on one thread.
4. **Output.** Countries are printed in code order, then `_UNMATCHED`. Cents / 100 as a double prints exactly, because every total is far below 2^53 / 100.

The design and every number behind it are in [PERF.md](PERF.md).

Correctness is checked against the reference outputs on tiny/sample/full seed 1, sample seed 17 and the spec's worked example. `check.py c` passes on full seed 1.

## Known limits

- **Name identity is a 61-bit hash.** Among the spec's ~100k distinct names, the chance of two sharing a hash is about 1 in 4 × 10^8 per dataset. Comparing bytes would cost a second cache miss per record.
- **Name numbers are 17 bits**, enough for the generator's pool of 100,000 names (up to 131,072).

## Dependencies

None (`dependency_count = 0`). The C++ standard library is statically linked.

## With another 24 hours

- Tune the piece size and batch length on the real box (6 P-cores, 12 threads, no turbo).
- Shrink the ~15% of time spent in `lock bts`, perhaps by routing bit updates to the thread that owns each group's rows.
