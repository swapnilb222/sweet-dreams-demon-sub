# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Dependency-free C++20, one pass over the corpus, and **no full index at any time**. The spec allows indexing only the terms the queries use (section 3). So the queries are read first, the corpus is scanned in phases of about 32 MB, and each phase is indexed, answered and dropped before the next one is built.

1. **Exact term table, built in parallel.** Every thread parses a line-aligned slice of `queries.txt` by position: in `BOOL` the even words are terms, so `and`/`or`/`not` are ordinary terms. A term's key is its bytes 0–7 and 8–15, zero-padded, and the table has 64 shards (the top 6 hash bits). Each shard's owner counts its distinct hashes, the table is sized from those counts (about 2 MB), and the owner inserts its terms. A line holds four 64-bit tags and their actions. The hash is a bijection of the first 8 bytes when the rest is empty, so an equal tag means an equal token. Longer terms keep their second word beside the table, and a bit in the line says to compare it; 17–32-byte terms also compare the rest. No hit is ever taken on trust.
2. **A 1/64 sample of `docs.txt`** counts each term's occurrences. These counts affect speed, never answers: a Boolean term in at least 1/32 of the documents gets a bitmap, the others lists; each phrase is anchored on its rarest word.
3. **The scan.** Threads take 256 KB claims of the current phase from a counter and read them with `pread`. Tokens come from a 64-byte AVX2 bitmask; bytes ≥ 0x80 fail both the letter and digit tests, so UTF-8 splits words exactly as the spec requires, and a token followed by TAB is a document id. Lookups are prefetched 16 tokens ahead. A token's action says what to do, and every store is unconditional, with only the counts moving:
   - a heavy term buffers (bitmap, document), applied once per 64 documents;
   - a list term buffers (term, document), sent to one of 64 row shards in chunks headed by the claim's number;
   - an anchor checks its phrases against the document's sequence of term actions, and a match is written as a row of the phrase's pseudo-term.
4. **Each phase** has at most 65,535 documents, so ids within it are 16 bits. When it is scanned, every shard sorts its chunks by claim (which puts the rows in document order without sorting them), counts them and writes one list per term; a repeat within a document is dropped against the previous entry. Then every query is answered on the phase alone, strictly left to right:
   - lists against lists: binary search when one side is 16 times shorter, otherwise marking one side in a bitmap;
   - lists against bitmaps: bit tests;
   - bitmap against bitmap: word operations.

   Results go into a tally per query (count, 64-bit sum, ten smallest ids). Phrase queries are one-term lookups. While the slower threads still scan phase k, the faster ones answer phase k − 1.
5. **Output.** Phases are answered in order and each query's tally is updated by one thread at a time, so there are no atomics on results and the first ten ids need no merge. Threads format disjoint query ranges, written with one `write` each.

Memory, 37.5 MiB cgroup peak at full scale:
- one phase of rows and lists, plus two sets of bitmaps (one filled while the other is answered);
- the table and the tallies;
- small per-thread buffers;
- `PR_SET_THP_DISABLE`, because a partly used huge page is charged in full;
- a fixed glibc mmap threshold, so that freed parse buffers return to the kernel.

## Build and run

```sh
docker build --network=none -t sd-track-b track-b
docker run --rm --network=none --read-only --user 65534:65534 \
  -e TRACK=b -v "$PWD/data:/data:ro" -v "$PWD/out:/out" sd-track-b
```

The binary defaults to `/data` and `/out`; locally it also accepts `solution DATA_DIRECTORY OUTPUT_DIRECTORY`. The thread count is the process's CPU affinity count (12 on the scoring box); `SD_THREADS` overrides it for testing. The Dockerfile pins allowlisted GCC and Debian images, builds offline and links statically for the portable x86-64-v3 baseline (`-mtune=alderlake`, no `-march=native`).

## Dependencies

Only the C++ standard library and Linux system calls. `dependency_count = 0`, so D = 1.

## Validation and measurements

See [PERF.md](PERF.md). Development sources (v1–v16), benchmark and test scripts live in [../dev-b](../dev-b), outside the scored folder. The canonical reference is `../../speed-demon-2026/reference/b_index.py`.

## With another 24 hours

- The scan is about 60 cycles per token on one thread, and three parts cost about the same: tokenizing and hashing, the lookup, and the bookkeeping. Fusing the tokenize, hash and lookup passes would remove the intermediate arrays.
- A small filter in front of the table would stop the ~30 % of tokens that are no query term before they touch it.
- Setup (15–25 ms locally, much of it page faults) could fault its arrays in bulk.
- The phase size and the heavy threshold should be tuned on the scoring box itself.
