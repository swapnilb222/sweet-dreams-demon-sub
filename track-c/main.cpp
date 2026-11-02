// Speed Demon 2026, Track C (Join). v23.
//
// Threads: one per CPU, spawned as a tree before anyone faults memory in
// (a page fault can hold up the mmap lock that creating a thread needs). They
// parse regions.csv in T slices into a direct table region_id -> group (0 =
// unmatched), meet at a barrier, then claim records.csv in 1 MiB pieces. A
// piece starts at its first record start: by spec 2.2 an LF inside quotes is
// followed by [a-z"], so an LF followed by a digit or a BOM byte starts one.
//
// A piece is walked in batches of 2 KiB, in passes:
//  1. Structure. Per 64 bytes, AVX2 masks of quotes, commas and LFs; a
//     carry-less multiply of the quote mask by all ones gives the
//     inside-quotes mask ("" toggles twice, so escapes need no care). Every
//     record has exactly five structural bytes (four commas, one LF) outside
//     quotes; their offsets go to pos[]. Each block's quote mask and the
//     quote count before it are kept, so the quotes before any offset are a
//     popcount away.
//  2. Numbers: the digits before each field end, decoded four records at a
//     time with SIMD multiply-adds.
//  3. Names: the raw bytes between the quotes are hashed with AES (escaping
//     is one to one, so equal bytes mean equal values; both empty forms hash
//     as empty). A quoted field of r raw bytes and q quotes holds a value of
//     r - q/2 - 1 bytes. The name bucket and the region row are prefetched.
//  4. Totals per thread and group, and the name's dense number: the hash's top
//     14 bits pick a 64-byte bucket of eight slots, each a 47-bit tag above a
//     17-bit number (61 bits of identity: among 100k names, a false match has
//     odds of about 1 in 4 x 10^8). The bitset word is prefetched for writing.
//  5. Distinct names: an atomic OR sets bit (group, name) of a bitset laid out
//     [name / 64][group], so it grows with the names seen (~3 MiB); after
//     parsing, the threads popcount it in shares.
// The name buckets and the bitset's head sit on two huge pages (fewer TLB
// misses), all of them used; a piece's file pages are dropped once parsed
// (else exit unmaps them all on one thread). Board-style peak memory: ~9 MB.
#include <bits/stdc++.h>
#include <fcntl.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/prctl.h>
using namespace std;
using u64 = uint64_t;
using V = __m256i;
using U = u64 __attribute__((may_alias, aligned(1))); // an unaligned word
#define LD(p) _mm256_loadu_si256((const V *)(p))
#define MASK(x) (unsigned)_mm256_movemask_epi8(x)
#define EQ(v, c) MASK(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)))
#define EQ64(a, b, c) (EQ(a, c) | (u64)EQ(b, c) << 32)
// Commas and LFs at once: a byte equals the table entry its low nibble picks
// (',' at 0xC, LF at 0xA, 0x80 elsewhere; bytes >= 0x80 pick 0).
#define SEP(v) MASK(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(sep, v), v))
#define POP __builtin_popcountll
#define RW PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
// Name buckets: 2^BB of eight slots (1 MiB).
constexpr long BB = 14, PIECE = 1 << 20, BIAS = 1l << 40;

// Totals of a group: count, id sum, amount sum, name bytes | note bytes << 32,
// BIAS + max amount, BIAS - min amount, distinct names. Zero is empty, and
// merging is sums and maxima.
using Tot = long[8];
static char *in, *rg, *tab, keep[64];
static u64 len, rlen, nreg, T, ngroups, G;
static unsigned char *group_of;
static atomic<u64> *buckets, *seen, next_piece, next_number;
static atomic<unsigned char> code_group[17576];
static mutex mu;
static barrier<> *sync_all;
static Tot all[256];

// 64 KiB of zero pages after the file: wide loads may run past its end.
static char *map_file(const char *path, u64 &n) {
  int fd = open(path, O_RDONLY);
  n = lseek(fd, 0, SEEK_END);
  return (char *)mmap(mmap(0, n + 65536, RW, -1, 0), n, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
}
// The n (1..8) digit bytes before e, as digit values in the top n bytes.
static u64 digits(const char *e, u64 n) { return (*(U *)(e - 8) ^ 0x3030303030303030) >> (64 - 8 * n) << (64 - 8 * n); }
// Words of eight digit values to numbers, four at a time.
static void decode(const u64 *w, unsigned *out, u64 n) {
  for (u64 i = 0; i < n; i += 4) {
    V v = _mm256_maddubs_epi16(LD(w + i), _mm256_set1_epi16(0x010A));
    v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x10064)), v = _mm256_packus_epi32(v, v);
    v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x12710)), v = _mm256_permute4x64_epi64(v, 8);
    _mm_storeu_si128((__m128i *)(out + i), _mm256_castsi256_si128(v));
  }
}
// AES hash of the n raw bytes at p, 32 at a time; adds their quotes to q.
static u64 hash_of(const char *p, u64 n, u64 &q) {
  __m128i k = _mm_set_epi64x(0x1D8E4E27C47D124F, n ^ 0x589965CC75374CC3), h = k;
  for (u64 i = 0; !i || i < n; i += 32) {
    V b = _mm256_and_si256(LD(p + i), LD(keep + 32 - min<u64>(n - i, 32)));
    __m128i lo = _mm256_castsi256_si128(b), hi = _mm256_extracti128_si256(b, 1);
    h = _mm_aesenc_si128(_mm_xor_si128(h, lo), hi), q += POP(EQ(b, '"'));
  }
  return _mm_cvtsi128_si64(_mm_aesenc_si128(_mm_aesenc_si128(h, k), k));
}
// The dense number of a name, given its hash.
static u64 intern(u64 h, u64 &spare) {
  atomic<u64> *b = buckets + (h >> (64 - BB)) * 8;
  u64 tag = (h & ((1ull << 47) - 1)) | 1, f = 0;
  V want = _mm256_set1_epi64x(tag);
  for (int i = 0; i < 2; ++i) { // the eight tags, four at a time
    V eq = _mm256_cmpeq_epi64(_mm256_srli_epi64(_mm256_load_si256((V *)b + i), 17), want);
    f |= _mm256_movemask_pd(_mm256_castsi256_pd(eq)) << 4 * i;
  }
  if (__builtin_expect(f != 0, 1)) return b[__builtin_ctz(f)] & 0x1FFFF;
  // New here (or spilled past its full bucket): first free slot from the
  // bucket on, claimed with a CAS. Numbers are reserved 32 at a time: spare
  // holds the next one, and the end of its block in the high half.
  if ((unsigned)spare == spare >> 32) spare = (next_number.fetch_add(32) + 32) * 0x100000001 - 32;
  // A failed CAS leaves the slot's value in cur.
  for (u64 i = b - buckets, cur, mine = tag << 17 | (unsigned)spare;; i = (i + 1) % (8 << BB))
    if ((!(cur = buckets[i]) && buckets[i].compare_exchange_strong(cur, mine)) || cur >> 17 == tag)
      return cur ? cur & 0x1FFFF : spare++ & 0x1FFFF;
}

static void work(int t, int span) {
  for (; span > 1; span -= span / 2) thread(work, t + span - span / 2, span / 2).detach();
  sync_all->arrive_and_wait(); // every thread exists
  // Fault in the two huge pages, one thread each, while the others parse
  // regions. No one else touches them yet: racing faults would each allocate
  // (and be charged) a 2 MiB page.
  if (t < 2) tab[t << 21] = 0;
  // regions.csv: slice t holds the lines whose preceding LF is in it.
  char *p = (char *)memchr(rg + rlen * t / T, '\n', rlen) + 1, *e = rg + min(rlen * (t + 1) / T, rlen - 1);
  for (u64 id = 0; p <= e; id = 0, p += 5) {
    while (*p != ',') id = id * 10 + *p++ - '0';
    unsigned code = (p[1] - 'A') * 676 + (p[2] - 'A') * 26 + p[3] - 'A', g = code_group[code];
    if (!g) // a new country: numbered under the lock
      mu.lock(), g = code_group[code] ? code_group[code].load() : (code_group[code] = ++ngroups), mu.unlock();
    group_of[id] = g;
  }
  sync_all->arrive_and_wait();
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // the bitset's tail gets 4 KB pages
  G = ngroups + 1;
  Tot tot[256] = {};
  const u64 lo = 0x8080808080808080, hi = 0x8080802C800A8080;
  const V sep = _mm256_set_epi64x(hi, lo, hi, lo);
  const __m128i ones = _mm_set1_epi8(-1);
  unsigned pos[1280], qb[64], nm[3][260];
  u64 qm[64], dg[3][260], hs[256], lens[256], spare = 0;
  long sg[256];
  for (u64 claim = next_piece, size; claim < len;) {
    // 1 MiB pieces; the last T MiB in quarters, so the threads finish together.
    if (!next_piece.compare_exchange_weak(claim, claim + (size = claim + T * PIECE < len ? PIECE : PIECE / 4))) continue;
    u64 end = min(claim + size, len), at = max<u64>(claim, 1) - 1, filled = 0, inside = 0, quotes = 0;
    while (at < len && !(in[at] == '\n' && (isdigit(in[at + 1]) || in[at + 1] == '\xEF'))) ++at;
    u64 rec = ++at, origin = at;
    // Quote bytes before offset p, from the masks of the last 64 blocks.
    auto rank = [&](u64 p) { return qb[(p - origin) / 64 % 64] + POP(_bzhi_u64(qm[(p - origin) / 64 % 64], (p - origin) % 64)); };
    while (rec < end) {
      // 1. structure
      for (int s = 0; s < 32; ++s, at += 64) {
        _mm_prefetch(in + at + 4096, _MM_HINT_T0);
        V a = LD(in + at), b = LD(in + at + 32);
        u64 q = EQ64(a, b, '"'), blk = (at - origin) / 64 % 64;
        __m128i odd = _mm_clmulepi64_si128(_mm_cvtsi64_si128(q), ones, 0);
        u64 ins = _mm_cvtsi128_si64(odd) ^ inside;
        u64 m = (SEP(a) | (u64)SEP(b) << 32) & ~ins, k = POP(m);
        inside = (long)ins >> 63, qm[blk] = q, qb[blk] = quotes, quotes += POP(q);
        // Eight stores always (those past the count are overwritten later),
        // so the usual count needs no unpredictable loop exit.
        for (u64 j = 0; j < 8 || m; ++j, m &= m - 1) pos[filled + j] = at + _tzcnt_u64(m);
        filled += k;
      }
      // 2. numbers of the records that start in this piece
      u64 n = 0, r = rank(rec);
      for (; (n + 1) * 5 <= filled && rec < end; rec = pos[n++ * 5 + 4] + 1) {
        const unsigned *f = pos + n * 5;
        u64 stop = f[4] - (in[f[4] - 1] == '\r'), neg = in[f[3] + 1] == '-', w = digits(in + stop, stop - f[3] - 1 - neg);
        dg[0][n] = digits(in + f[0], f[0] - rec - (in[rec] == '\xEF') * 3);
        // The amount, with its dot squeezed out.
        dg[1][n] = (w & 0xFFFFFFFFFF) << 8 | w >> 48 << 48;
        dg[2][n] = digits(in + f[3], f[3] - f[2] - 1);
        sg[n] = -(long)neg;
      }
      for (int k = 0; k < 3; ++k) decode(dg[k], nm[k], n);
      // 3. names, lengths and prefetches
      for (u64 i = 0; i < n; ++i) {
        const unsigned *f = pos + i * 5;
        u64 r1 = f[1] - f[0] - 1, r2 = f[2] - f[1] - 1, nq = 0, before = r;
        hs[i] = hash_of(in + f[0] + 2, r1 < 2 ? 0 : r1 - 2, nq);
        u64 tq = (r = rank(f[2])) - before - nq - (r1 ? 2 : 0);
        lens[i] = (r1 ? r1 - 2 - nq / 2 : 0) | (r2 ? r2 - 1 - tq / 2 : 0) << 32;
        nm[2][i] = nm[2][i] < nreg ? nm[2][i] : 0;
        _mm_prefetch((char *)(buckets + (hs[i] >> (64 - BB)) * 8), _MM_HINT_T0);
        _mm_prefetch((char *)group_of + nm[2][i], _MM_HINT_T0);
      }
      // 4. totals and name numbers
      for (u64 i = 0; i < n; ++i) {
        u64 g = group_of[nm[2][i]];
        long a = (nm[1][i] ^ sg[i]) - sg[i], *o = tot[g];
        o[0]++, o[1] += nm[0][i], o[2] += a, o[3] += lens[i], o[4] = max(o[4], BIAS + a), o[5] = max(o[5], BIAS - a);
        // Bitset words are [name / 64][group]: memory grows with the names
        // seen, not a guessed maximum. hs: word << 6 | bit.
        u64 name = intern(hs[i], spare);
        hs[i] = ((name / 64) * G + g) << 6 | name % 64;
        _mm_prefetch((char *)(seen + (hs[i] >> 6)), _MM_HINT_ET0);
      }
      // 5. distinct names: set the bit (counted by popcount at the end)
      for (u64 i = 0; i < n; ++i) seen[hs[i] >> 6].fetch_or(1ull << hs[i] % 64, memory_order_relaxed);
      filled -= n * 5;
      memmove(pos, pos + n * 5, filled * 4);
    }
    madvise((void *)(in + claim), end - claim, MADV_DONTNEED);
    claim = next_piece;
  }
  // Distinct names per group: popcounts of this thread's share of the words.
  sync_all->arrive_and_wait();
  u64 w = (next_number / 64 + 1) * G;
  for (u64 i = w * t / T; i < w * (t + 1) / T; ++i) tot[i % G][6] += POP(seen[i]);
  mu.lock();
  for (int i = 0; i < 256 * 8; ++i) {
    long &a = all[i / 8][i % 8], b = tot[i / 8][i % 8];
    a = i % 8 == 4 || i % 8 == 5 ? max(a, b) : a + b;
  }
  mu.unlock();
  sync_all->arrive_and_wait(); // all totals are in
}

int main() {
  cpu_set_t cs;
  sched_getaffinity(0, sizeof cs, &cs);
  T = CPU_COUNT(&cs), memset(keep, -1, 32);
  in = map_file("/data/records.csv", len), rg = map_file("/data/regions.csv", rlen);
  // Regions are listed in ascending id order: the last line has the largest.
  nreg = strtoul((char *)memrchr(rg, '\n', rlen - 1) + 1, 0, 10) + 1;
  group_of = (unsigned char *)mmap(0, nreg, RW, -1, 0);
  // Name buckets (1 MiB), then the bitset (2^17 names x 256 groups at most,
  // ~3 MiB used), 2 MiB aligned. The first 4 MiB are two huge pages, all of
  // them used, so the random lookups miss the TLB less; the bitset's tail
  // gets 4 KB pages, charged only as touched.
  tab = (char *)mmap(0, 7 << 20, RW, -1, 0), tab += -(uintptr_t)tab & ((2 << 20) - 1);
  madvise(tab, 4 << 20, MADV_HUGEPAGE);
  buckets = (atomic<u64> *)tab, seen = buckets + (8 << BB);
  barrier<> b(T);
  sync_all = &b, work(0, T);
  // Countries in code order, then _UNMATCHED (group 0). Cents / 100 as a
  // double prints exactly: every total is far below 2^53 / 100.
  FILE *out = fopen("/out/aggregates.txt", "w");
  for (int c = 0; c <= 17576; ++c) {
    unsigned g = c < 17576 ? code_group[c].load() : 0;
    long *o = all[g];
    if ((c < 17576 && !g) || !o[0]) continue;
    g ? fprintf(out, "%c%c%c", 'A' + c / 676, 'A' + c / 26 % 26, 'A' + c % 26) : fputs("_UNMATCHED", out);
    double sum = o[2] / 100., lo = (BIAS - o[5]) / 100., hi = (o[4] - BIAS) / 100.;
    fprintf(out, " %ld %ld %.2f %.2f %.2f", o[0], o[1], sum, lo, hi);
    fprintf(out, " %ld %ld %ld\n", o[6], o[3] & 0xFFFFFFFF, o[3] >> 32);
  }
  fclose(out);
  _exit(0);
}
