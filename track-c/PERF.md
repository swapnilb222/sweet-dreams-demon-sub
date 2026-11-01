# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

Local numbers (in the notes) were measured on a laptop (Intel Core Ultra 7 265U, 14 threads, Docker Desktop) on full seed 1, with the data in a Docker volume. They are best-of-15 wall times and are noisy. Fill in the board's T/M/run ID after each push.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | C++ rewrite: mmap, parallel chunks synced on LF+digit/BOM, hand-written scanner, per-thread name tables + per-group bitsets merged by hash shard (local: ~0.24 s) | | | | |
| 2 | Shared lock-free name table (read-mostly, cache-resident) + per-group id streams in place of per-thread tables and bitsets; no merge phase (local: ~0.12 s, peak RSS ~52 MB) | | | | |
| 3 | Software-pipelined name lookups (prefetch slot, then arena entry, 8 records apart): single-thread parse 1.2 s → 0.43 s (local: ~0.10 s) | | | | |
| 4 | AVX2 field scanning (quote bit masks, one 32-byte compare for region+amount), SWAR number decoding, 3-stage pipeline (region row → name slot → bitset word), per-country bitsets with atomic OR giving distinct counts directly (no merge pass), regions parsed in parallel, tables pre-faulted with MADV_POPULATE_WRITE (local, 12 threads: ~55 ms) | | | | |
| 5 | Piece bounds found lazily by each worker: the up-front split faulted a 64 KB window at every cut (peak RSS 58 → 23 MB) | | | | |
| 6 | Name id = slot of its 64-bit key in a 4-wide AVX2-probed table (no separate key store: less cache pressure under 12 threads); write-hot counters padded off read-only lines; 1 MB pieces (local, 12 threads: ~30–40 ms, 23 MB) | | | | |
