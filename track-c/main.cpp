// Speed Demon 2026, Track C (Join). v28.
//
// Threads: one per CPU in the affinity mask. They parse regions.csv in T
// slices into a direct table region_id -> group (0 = unmatched), meet at a
// barrier, then claim records.csv in 1 MiB pieces. A piece starts at its first
// record start: by spec 2.2 an LF inside quotes is followed by [a-z"], so an
// LF followed by a digit or a BOM byte starts a record.
//
// A piece is walked in batches of 1 KiB, in passes:
//  1. Structure. Per 64 bytes, AVX2 masks of quotes and of commas and LFs; a
//     carry-less multiply of the quote mask by all ones gives the inside-
//     quotes mask ("" toggles twice, so escapes need no care). Each record is
//     exactly five structural offsets (four commas, one LF). Each block's
//     quote mask is kept, so the quotes before any offset are a popcount away.
//  2. Numbers: the digit bytes before each field end, decoded four records at
//     a time with SIMD multiply-adds (the amount's dot squeezed out first).
//  3. Names: the raw bytes between the quotes are hashed with AES (escaping
//     is one to one, so equal bytes mean equal values; both empty forms hash
//     as empty). A quoted field of r raw bytes and q quotes holds a value of
//     r - q/2 - 1 bytes. The name bucket and the region row are prefetched.
//  4. Totals per thread and group (merging them is sums and maxima: the
//     minimum is kept as BIAS - amount), and the name's dense number: the
//     hash's top 14 bits pick a 64-byte bucket of eight slots, each a 47-bit
//     tag above a 17-bit number (61 bits of identity: among 100k names a false
//     match has odds of about 1 in 4 x 10^8). The bitset word is prefetched.
//  5. Distinct names: an atomic OR sets bit (group, name) of a bitset laid out
//     [name / 64][group] (~3 MiB, growing with the names seen); a bit that was
//     clear is a new distinct name.
// The name buckets and the bitset's head sit on two huge pages (fewer TLB
// misses); a piece's file pages are dropped once parsed (else exit unmaps
// them all on one thread).
#include <bits/stdc++.h>
#include <immintrin.h>
#include <sys/mman.h>
using namespace std;
using u64 = uint64_t;
using V = __m256i;
using U = u64 __attribute__((may_alias, aligned(1))); // an unaligned word
#define LD(p) _mm256_loadu_si256((V *)(p))
#define MASK(x) (u64)(unsigned)_mm256_movemask_epi8(x)
#define EQ(v, c) MASK(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)))
#define SEP(v) MASK(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(sep, v), v))
#define RW PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
#define POP __builtin_popcountll
constexpr long PIECE = 1 << 20, B = 1l << 40;

static char *in, *rg, *tab, keep[64];
static u64 len, rlen, nreg, T, G;
static unsigned char *group_of;
static atomic<u64> *buckets, *seen, next_piece, next_number, ngroups;
static atomic<unsigned char> code_group[17576];
static once_flag numbered[17576];
static barrier<> *sync_all;
static mutex mu;
// Per group: count, id sum, amount sum, name bytes | note bytes << 32,
// B + max amount, B - min amount, distinct names.
static long all[256][8];

// The file at /data/name, with 64 KiB of zero pages after it.
static char *open_map(const char *name, u64 &n) {
  FILE *f = fopen(name, "r");
  fseek(f, 0, SEEK_END), n = ftell(f);
  return (char *)mmap(mmap(0, n + 65536, RW), n, PROT_READ, MAP_PRIVATE | MAP_FIXED, fileno(f), 0);
}
// The n (1..8) digit bytes before e, as digit values in the top n bytes.
static u64 digits(const char *e, u64 n) { return (*(U *)(e - 8) ^ 0x3030303030303030) >> (64 - 8 * n) << (64 - 8 * n); }
// AES hash of the n raw bytes at p, 32 at a time; adds their quotes to q.
static u64 hash_of(const char *p, u64 n, u64 &q) {
  __m128i k = _mm_set1_epi64x(n ^ 0x589965CC75374CC3), h = k;
  for (u64 i = 0; !i || i < n; i += 32) {
    V b = LD(p + i) & LD(keep + 32 - min<u64>(n - i, 32));
    __m128i lo = _mm256_castsi256_si128(b), hi = _mm256_extracti128_si256(b, 1);
    h = _mm_aesenc_si128(h ^ lo, hi), q += POP(EQ(b, '"'));
  }
  return _mm_cvtsi128_si64(_mm_aesenc_si128(_mm_aesenc_si128(h, k), k));
}
// The dense number of a name, given its hash.
static u64 intern(u64 h, u64 &spare) {
  atomic<u64> *b = buckets + (h >> 50) * 8;
  u64 tag = (h & ((1ull << 47) - 1)) | 1, f = 0;
  V want = _mm256_set1_epi64x(tag), *s = (V *)b; // the eight tags, four at a time
  for (int i = 0; i < 2; ++i) f |= _mm256_movemask_pd((__m256d)(_mm256_srli_epi64(s[i], 17) == want)) << 4 * i;
  if (f) return b[__builtin_ctz(f)] & 0x1FFFF;
  // New here (or spilled past a full bucket): the first free slot from the
  // bucket on, claimed with a CAS (which leaves the slot's value in c on
  // failure). Numbers are reserved 32 at a time: spare holds the next one and
  // the end of its block in the high half.
  if ((unsigned)spare == spare >> 32) spare = (next_number.fetch_add(32) + 32) * 0x100000001 - 32;
  for (u64 i = b - buckets, c, mine = tag << 17 | (unsigned)spare;; i = (i + 1) % (8 << 14))
    if ((!(c = buckets[i]) && buckets[i].compare_exchange_strong(c, mine)) || c >> 17 == tag)
      return c ? c & 0x1FFFF : spare++ & 0x1FFFF;
}

static void work(u64 t) {
  // Once every thread exists (a page fault holds up thread creation), fault in
  // the two huge pages, one thread each, before anyone else touches them.
  sync_all->arrive_and_wait();
  if (t < 2) tab[t << 21] = 0;
  // regions.csv: slice t holds the lines whose preceding LF is in it.
  char *p = (char *)memchr(rg + rlen * t / T, '\n', rlen) + 1, *e = rg + min(rlen * (t + 1) / T, rlen - 1);
  for (u64 id = 0, c; p <= e; group_of[id] = code_group[c], id = 0, p += 5) {
    while (*p != ',') id = id * 10 + *p++ - '0';
    c = (p[1] - 'A') * 676 + (p[2] - 'A') * 26 + p[3] - 'A';
    call_once(numbered[c], [c] { code_group[c] = ++ngroups; });
  }
  sync_all->arrive_and_wait();
  G = ngroups + 1;
  // ',' at 0xC, LF at 0xA, 0x80 elsewhere
  const u64 lo = 0x8080808080808080, hi = 0x8080802C800A8080;
  const V sep = _mm256_set_epi64x(hi, lo, hi, lo);
  const __m128i ones = _mm_set1_epi8(-1);
  long tot[256][8] = {}, sg[128];
  unsigned pos[1040], qb[64], nm[3][128];
  u64 qm[64], dg[3][128], hs[128], lens[128], spare = 0;
  for (u64 claim; (claim = next_piece.fetch_add(PIECE)) < len;) {
    u64 end = min(claim + PIECE, len), at = max<u64>(claim, 1) - 1, filled = 0, inside = 0, quotes = 0;
    while (at < len && !(in[at] == '\n' && (isdigit(in[at + 1]) || in[at + 1] == '\xEF'))) ++at;
    u64 rec = ++at, org = at; // the piece's origin
    // Quote bytes before offset x, from the masks of the last 64 blocks.
    auto rank = [&](u64 x) { return x -= org, qb[x / 64 % 64] + POP(_bzhi_u64(qm[x / 64 % 64], x % 64)); };
    while (rec < end) {
      // 1. structure. Eight offsets are always stored (those past the count
      // are overwritten later), so the usual count needs no loop exit.
      for (int s = 0; s < 16; ++s, at += 64) {
        _mm_prefetch(in + at + 4096, _MM_HINT_T0);
        V a = LD(in + at), b = LD(in + at + 32);
        u64 q = EQ(a, '"') | EQ(b, '"') << 32, blk = (at - org) / 64 % 64;
        __m128i odd = _mm_clmulepi64_si128(_mm_cvtsi64_si128(q), ones, 0);
        u64 ins = _mm_cvtsi128_si64(odd) ^ inside;
        // Commas and LFs: bytes equal to the table entry their low nibble picks.
        u64 m = (SEP(a) | SEP(b) << 32) & ~ins, k = POP(m);
        inside = (long)ins >> 63, qm[blk] = q, qb[blk] = quotes, quotes += POP(q);
#pragma GCC unroll 8
        for (u64 j = 0; j < 8; ++j, m &= m - 1) pos[filled + j] = at + _tzcnt_u64(m);
        for (u64 j = 8; m; ++j, m &= m - 1) pos[filled + j] = at + _tzcnt_u64(m);
        filled += k;
      }
      // 2. numbers of the records that start in this piece
      u64 n = 0, r = rank(rec);
      for (; (n + 1) * 5 <= filled && rec < end; rec = pos[n++ * 5 + 4] + 1) {
        unsigned *f = pos + n * 5, stop = f[4] - (in[f[4] - 1] == '\r'), neg = in[f[3] + 1] == '-';
        u64 w = digits(in + stop, stop - f[3] - 1 - neg);
        dg[0][n] = digits(in + f[0], f[0] - rec - (in[rec] == '\xEF') * 3), dg[2][n] = digits(in + f[3], f[3] - f[2] - 1);
        dg[1][n] = (w & 0xFFFFFFFFFF) << 8 | w >> 48 << 48, sg[n] = -(long)neg;
      }
      for (int k = 0; k < 3; ++k)
        for (u64 i = 0; i < n; i += 4) {
          V v = _mm256_maddubs_epi16(LD(dg[k] + i), _mm256_set1_epi16(0x010A));
          v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x10064)), v = _mm256_packus_epi32(v, v);
          v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x12710)), v = _mm256_permute4x64_epi64(v, 8);
          _mm_storeu_si128((__m128i *)(nm[k] + i), _mm256_castsi256_si128(v));
        }
      // 3. names, lengths and prefetches
      for (u64 i = 0; i < n; ++i) {
        unsigned *f = pos + i * 5;
        u64 r1 = f[1] - f[0] - 1, r2 = f[2] - f[1] - 1, nq = 0, before = r;
        hs[i] = hash_of(in + f[0] + 2, r1 < 2 ? 0 : r1 - 2, nq);
        u64 tq = (r = rank(f[2])) - before - nq - (r1 ? 2 : 0);
        lens[i] = (r1 ? r1 - 2 - nq / 2 : 0) | (r2 ? r2 - 1 - tq / 2 : 0) << 32, nm[2][i] *= nm[2][i] < nreg;
        _mm_prefetch((char *)(buckets + (hs[i] >> 50) * 8), _MM_HINT_T0);
        _mm_prefetch((char *)group_of + nm[2][i], _MM_HINT_T0);
      }
      // 4. totals and name numbers (hs becomes the bit's word << 6 | bit)
      for (u64 i = 0, g, name; i < n; ++i) {
        long a = (nm[1][i] ^ sg[i]) - sg[i], *o = tot[g = group_of[nm[2][i]]];
        o[0]++, o[1] += nm[0][i], o[2] += a, o[3] += lens[i], o[4] = max(o[4], B + a), o[5] = max(o[5], B - a);
        name = intern(hs[i], spare), hs[i] = (name / 64 * G + g) << 14 | g << 6 | name % 64;
        _mm_prefetch((char *)(seen + (hs[i] >> 14)), _MM_HINT_ET0);
      }
      // 5. distinct names
      for (u64 i = 0; i < n; ++i) {
        u64 bit = 1ull << hs[i] % 64;
        tot[hs[i] >> 6 & 255][6] += !(seen[hs[i] >> 14].fetch_or(bit) & bit);
      }
      memmove(pos, pos + n * 5, (filled -= n * 5) * 4);
    }
    madvise(in + claim, end - claim, MADV_DONTNEED);
  }
  lock_guard l(mu);
  for (long i = 0, *a = *all, *b = *tot; i < 256 * 8; ++i) // sums, and maxima in slots 4, 5
    a[i] = u64(i % 8 - 4) < 2 ? max(a[i], b[i]) : a[i] + b[i];
}

int main() {
  cpu_set_t cs;
  sched_getaffinity(0, sizeof cs, &cs), T = CPU_COUNT(&cs);
  memset(keep, -1, 32), in = open_map("/data/records.csv", len);
  rg = open_map("/data/regions.csv", rlen);
  // Regions are listed in ascending id order: the last line has the largest.
  nreg = strtoul((char *)memrchr(rg, '\n', rlen - 1) + 1, 0, 10) + 1;
  group_of = (unsigned char *)mmap(0, nreg, RW);
  // Name buckets (1 MiB), then the bitset (at most 2^17 names x 256 groups),
  // 2 MiB aligned; the first 4 MiB are huge pages.
  tab = (char *)mmap(0, 7 << 20, RW), tab += -(uintptr_t)tab & ((2 << 20) - 1);
  madvise(tab, 4 << 20, MADV_HUGEPAGE);
  buckets = (atomic<u64> *)tab, seen = buckets + (1 << 17);
  barrier<> b(T);
  vector<jthread> pool;
  sync_all = &b;
  for (u64 t = 0; t < T; ++t) pool.emplace_back(work, t);
  pool.clear(); // joins
  // Countries in code order, then _UNMATCHED (group 0). Cents / 100 as a
  // double prints exactly: every total is far below 2^53 / 100.
  FILE *out = fopen("/out/aggregates.txt", "w");
  for (int c = 0; c <= 17576; ++c) {
    long *a = all[c < 17576 ? code_group[c].load() : 0];
    if ((c < 17576 && !code_group[c]) || !a[0]) continue;
    char cc[] = {char(65 + c / 676), char(65 + c / 26 % 26), char(65 + c % 26), 0};
    fprintf(out, "%s %ld %ld", c < 17576 ? cc : "_UNMATCHED", a[0], a[1]);
    fprintf(out, " %.2f %.2f %.2f", a[2] / 100., (B - a[5]) / 100., (a[4] - B) / 100.);
    fprintf(out, " %ld %ld %ld\n", a[6], a[3] & 0xFFFFFFFF, a[3] >> 32);
  }
  fclose(out);
}
