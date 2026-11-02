// Track B: answer the queries in one pass over the corpus.
//
// 1. The threads parse queries.txt in slices and put every distinct term into
//    one exact table (64 shards, each filled by one thread). A token that is
//    no query term costs one lookup and is dropped.
// 2. A 1/64 sample of docs.txt estimates how common each term is. That
//    decides speed only, never answers: common Boolean terms get a bitmap,
//    the rest lists of document ids; each phrase is anchored on its rarest
//    word.
// 3. docs.txt is scanned in phases of about 32 MB (at most 65,535 documents,
//    so ids within a phase fit 16 bits), in claims of 256 KB taken from a
//    counter. A phrase is checked when its anchor occurs, against the term
//    ids of the document's tokens; a match is written as a document of a
//    pseudo-term that stands for the phrase.
// 4. After each phase the rows are sorted into lists and every query is
//    answered on that phase alone, into a running tally per query (count, id
//    sum, ten smallest ids). The answers of phase k - 1 run on the threads
//    that have run out of claims of phase k. No full index ever exists.
#include <fcntl.h>
#include <immintrin.h>
#include <linux/futex.h>
#include <malloc.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using U8 = uint8_t;
using U16 = uint16_t;
using U32 = uint32_t;
using U64 = uint64_t;

// CLAIM bytes per scan claim, read CH at a time and tokenized BATCH at a
// time; buckets are prefetched PF tokens ahead; rows go to shards in chunks of
// CHUNK; queries are answered QB at a time.
constexpr U32 CLAIM = 1 << 18, CH = 1 << 17, BATCH = 1 << 13, PAD = 128, PF = 16;
constexpr U32 CHUNK = 32, QB = 64, HOT_RATIO = 32, MAX_HEAVY = 1023;
constexpr U64 PHASE = 32ull << 20;
// A term's action: what the scan does with its token. HEAVY: set bit LOW of
// the document's bitmap row; LIST: write a row for term LOW; ANCHOR: check
// the phrases anchored on term LOW. 0 = no term; OVER marks a full bucket.
constexpr U32 ANCHOR = 1u << 31, HEAVY = 1u << 30, LIST = 1u << 29,
              LOW = LIST - 1, OVER = ~0u;

[[noreturn]] static void die(const char *what) {
  fprintf(stderr, "error: %s\n", what);
  _exit(1);
}

template <class T> static T *reserve(U64 n) { // zero pages, backed on use
  void *p = mmap(nullptr, std::max<U64>(n * sizeof(T), 1), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED)
    die("mmap");
  return (T *)p;
}

// Eight bytes with ASCII letters lowercased; token bytes are only [A-Za-z0-9].
static U64 load8(const U8 *p) {
  U64 x;
  memcpy(&x, p, 8);
  return x | 0x2020202020202020ULL;
}
static U64 mix(U64 a, U64 b) {
  return (a ^ (b * 0x9E3779B97F4A7C15ULL)) * 0xD6E8FEB86659FD93ULL;
}

// Exact key: bytes 0-7 and 8-15, zero padded (tokens never contain 0x00).
// Tokens of 17-32 bytes put a hash with bit 63 set into k1 (ASCII never sets
// it); a hit on such a key is verified against the stored bytes.
struct Key {
  U64 k0, k1;
  bool operator==(const Key &) const = default;
};
using Tail = std::array<U64, 3>;
static Tail tail_words(const U8 *p, U32 n) {
  return {load8(p + 8), _bzhi_u64(load8(p + 16), std::min(n - 16, 8u) * 8),
          n > 24 ? _bzhi_u64(load8(p + 24), (n - 24) * 8) : 0};
}
static Key make_key(const U8 *p, U32 n) {
  U32 a = n < 8 ? n : 8, b = n < 8 ? 0 : n - 8;
  Key k{_bzhi_u64(load8(p), a * 8), _bzhi_u64(load8(p + 8), b * 8)};
  if (n > 16) [[unlikely]] {
    Tail t = tail_words(p, n);
    k.k1 = (mix(mix(t[0], t[1]), t[2]) ^ n) | 1ULL << 63;
  }
  return k;
}

// The term table: four tags and their actions per cache line. A tag is the
// whole 64-bit hash, which for a token of up to 8 bytes is a bijection of
// its bytes, so equal tags are equal tokens; a longer term also keeps its
// second word (k1) beside the table, and its slot's bit in `meta` says so.
// The home bucket is the top hash bits; a full bucket passes on to the next
// one of its shard (the top 6 bits), so that each shard is filled by one
// thread. A free slot has action 0; act[4] is always 0, for a miss.
struct alignas(64) Bucket {
  U64 h[4];
  U32 act[5], meta, pad[2]; // meta: bits 0-3 = slot has k1, bit 8 = overflowed
};
static U64 hash_of(Key k) { // bijective in k0 when k1 = 0
  return (k.k0 ^ k.k1 * 0x9E3779B97F4A7C15ULL) * 0xD6E8FEB86659FD93ULL;
}
struct Table {
  Bucket *b;
  U64 *k1;    // per slot, for terms longer than 8 bytes
  Tail *tail; // per slot, for terms of 17-32 bytes
  U32 bits, sbits;
  U32 home(U64 h) const { return h >> (64 - bits); }
  U32 next(U32 i) const {
    U32 m = (1u << sbits) - 1;
    return (i & ~m) | ((i + 1) & m);
  }
  bool same(U32 i, U32 j, Key k, U64 h, const U8 *p, U32 n) const {
    return b[i].act[j] && b[i].h[j] == h &&
           (b[i].meta >> j & 1 ? k1[i * 4 + j] : 0) == k.k1 &&
           (n <= 16 || tail[i * 4 + j] == tail_words(p, n));
  }
  U32 slow(Key k, U64 h, const U8 *p, U32 n) const {
    for (U32 i = home(h);; i = next(i)) {
      for (U32 j = 0; j < 4; ++j)
        if (same(i, j, k, h, p, n))
          return b[i].act[j];
      if (!(b[i].meta & 256))
        return 0;
    }
  }
  U32 find(U64 k1v, U64 h) const { // a token of at most 16 bytes
    static const U64 zero = 0;
    U32 i = home(h);
    const Bucket &x = b[i];
    __m256i m = _mm256_cmpeq_epi64(_mm256_load_si256((const __m256i *)x.h), _mm256_set1_epi64x(h));
    U32 s = _tzcnt_u32(_mm256_movemask_pd(_mm256_castsi256_pd(m)) | 16), a = x.act[s];
    bool ok = a && *(x.meta >> s & 1 ? &k1[i * 4 + s] : &zero) == k1v;
    if (!ok && (a || x.meta & 256)) [[unlikely]]
      return slow(Key{0, k1v}, h, nullptr, 0);
    return ok ? a : 0;
  }
};
static Table tbl;

// Only the shard's owner calls this. Returns the id of k, or assigns next.
static U32 insert(Key k, U64 h, U32 &next, const U8 *p, U32 n) {
  for (U32 i = tbl.home(h), steps = 0;; i = tbl.next(i)) {
    Bucket &x = tbl.b[i];
    for (U32 j = 0; j < 4; ++j) {
      if (!x.act[j]) {
        x.h[j] = h, x.act[j] = next;
        if (k.k1)
          x.meta |= 1 << j, tbl.k1[i * 4 + j] = k.k1;
        if (n > 16)
          tbl.tail[i * 4 + j] = tail_words(p, n);
        return next++;
      }
      if (tbl.same(i, j, k, h, p, n))
        return x.act[j];
    }
    x.meta |= 256;
    if (++steps > 1u << tbl.sbits)
      die("table shard full");
  }
}

struct Info { // a term's list in the current phase
  U32 first, n;
};
struct Phrase {
  U32 w[8], len, pivot, next, term; // w: the words' actions after planning
};
struct Tally {
  U64 sum;
  U32 count, id[10];
};
struct alignas(64) Ctr { // one set per phase, so none is ever reset
  std::atomic<U32> claim{0}, build{0}, answer{0};
  std::atomic<U64> post{0};
};

static int fd, qfd;
static U64 size, qsize;
static U8 *qmem;
static const U8 *qs;
static U32 threads, nq, nocc, nphr, nid, nheavy, wpb, nph, rbits, nshard;
static std::atomic<U32> heavy_ctr{0};
static std::atomic<U64> sample_docs{0};
// per term id
static U8 *kind;          // 1: in a BOOL query, 2: in a phrase
static U32 *gcnt, *act, *phead;
static Info *info;
// per query, and per term occurrence of the queries
static U32 *qoff, *qps, *qt;
static U8 *qn;
static char *qop;
static Phrase *phr;
static Tally *tal;
// phases
static std::vector<U64> pbyte;  // phase p: lines starting in [pbyte[p], pbyte[p+1])
static std::vector<U32> pbase;  // its first document id
static std::unique_ptr<Ctr[]> ctr;
static U32 *rows;               // per shard: chunks of [claim + 1, rows...]
static U64 rcap;
static std::unique_ptr<std::atomic<U64>[]> rclaimed;
static U16 *post;               // the current phase's lists
static U64 *bm;                 // two sets of bitmaps, (nheavy + 1) * wpb words each

static std::atomic<U32> arrived{0}, round_{0};
// A barrier on one futex word; the last thread to arrive runs `serial` first.
template <class F> static void barrier(F &&serial) {
  U32 r = round_.load(std::memory_order_acquire);
  if (arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == threads) {
    serial();
    arrived.store(0, std::memory_order_relaxed);
    round_.fetch_add(1, std::memory_order_release);
    syscall(SYS_futex, &round_, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
    return;
  }
  for (int i = 0; i < 20000; ++i) {
    if (round_.load(std::memory_order_acquire) != r)
      return;
    _mm_pause();
  }
  while (round_.load(std::memory_order_acquire) == r)
    syscall(SYS_futex, &round_, FUTEX_WAIT_PRIVATE, r, nullptr, nullptr, 0);
}
static void barrier() {
  barrier([] {});
}

static U64 alnum_mask(const U8 *p) { // bit i: byte i is [A-Za-z0-9]
  auto half = [](const U8 *q) -> U64 {
    __m256i b = _mm256_loadu_si256((const __m256i *)q);
    __m256i l = _mm256_or_si256(b, _mm256_set1_epi8(0x20));
    __m256i a = _mm256_cmpgt_epi8(_mm256_set1_epi8(-102),
                                  _mm256_add_epi8(l, _mm256_set1_epi8(0x1F)));
    __m256i d = _mm256_cmpgt_epi8(_mm256_set1_epi8(-118),
                                  _mm256_add_epi8(b, _mm256_set1_epi8(0x50)));
    return U32(_mm256_movemask_epi8(_mm256_or_si256(a, d)));
  };
  return half(p) | half(p + 32) << 32;
}
// Positions of the set bits, base added; writes 16 slots even when fewer.
static U32 flatten(U32 *o, U32 base, U64 bits) {
  U32 n = __builtin_popcountll(bits);
  for (U32 i = 0; i < 12; ++i)
    o[i] = base + _tzcnt_u64(bits), bits = _blsr_u64(bits);
  for (U32 i = 12; i < n; ++i)
    o[i] = base + _tzcnt_u64(bits), bits = _blsr_u64(bits);
  return n;
}
// The number of set bits in `words` words (a multiple of 4) and the sum of
// their positions, 32 bytes at a time: per byte, its popcount and the sum of
// its bit positions come from nibble tables; a byte's position in its word
// is added as 8 * index * popcount, a word's as 64 * index * popcount.
static void count_bits(const U64 *bits, U32 words, U64 &count, U64 &sum) {
  auto table = [](const char *t) { return _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)t)); };
  const __m256i ones = table("\0\1\1\2\1\2\2\3\1\2\2\3\2\3\3\4");
  const __m256i places = table("\0\0\1\1\2\2\3\3\3\3\4\4\5\5\6\6");
  const __m256i weights = _mm256_set1_epi64x(0x3830282018100800), low = _mm256_set1_epi8(15),
                zero = _mm256_setzero_si256();
  __m256i counts = zero, sums = zero, offsets = zero, starts = zero;
  __m256i first = _mm256_setr_epi64x(0, 64, 128, 192);
  for (U32 w = 0; w < words; w += 4) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(bits + w));
    __m256i lo = _mm256_and_si256(x, low), hi = _mm256_and_si256(_mm256_srli_epi16(x, 4), low);
    __m256i hi_ones = _mm256_shuffle_epi8(ones, hi);
    __m256i per_byte = _mm256_add_epi8(_mm256_shuffle_epi8(ones, lo), hi_ones);
    __m256i pos = _mm256_add_epi8(_mm256_add_epi8(_mm256_shuffle_epi8(places, lo), _mm256_shuffle_epi8(places, hi)),
                                  _mm256_slli_epi16(hi_ones, 2)); // a high nibble's bits sit 4 higher
    __m256i per_word = _mm256_sad_epu8(per_byte, zero);
    counts = _mm256_add_epi64(counts, per_word);
    sums = _mm256_add_epi64(sums, _mm256_sad_epu8(pos, zero));
    starts = _mm256_add_epi64(starts, _mm256_mul_epu32(per_word, first));
    offsets = _mm256_add_epi32(offsets, _mm256_madd_epi16(_mm256_maddubs_epi16(per_byte, weights), _mm256_set1_epi16(1)));
    first = _mm256_add_epi64(first, _mm256_set1_epi64x(256));
  }
  auto total = [](__m256i v) { // every 32-bit half holds a non-negative count below 2^32
    alignas(32) U32 l[8];
    _mm256_store_si256((__m256i *)l, v);
    U64 t = 0;
    for (U32 x : l)
      t += x;
    return t;
  };
  count = total(counts), sum = total(sums) + total(starts) + total(offsets);
}
static bool has(const U16 *b, U32 n, U16 x) { // branch-free binary search
  if (!n)
    return false;
  for (; n > 1; n -= n / 2)
    b = b[n / 2] <= x ? b + n / 2 : b;
  return *b == x;
}

struct Set { // a bitmap of wpb words, or a sorted list
  const U64 *bits;
  const U16 *ids;
  U32 n;
};

struct Worker {
  // queries parsed by this thread: per line, then per term occurrence
  std::vector<U8> pn, pph, plen; // plen: length, bit 7 = Boolean
  std::vector<char> pop;
  std::vector<Key> pkey;
  std::vector<U64> phash;
  std::vector<U32> pat;
  std::vector<U32> byown[64]; // occurrences per table shard
  U32 qbase = 0, tbase = 0, pbase_ = 0;
  // scan state
  std::vector<U8> buf;
  std::vector<U32> db, an;      // the document's actions, its anchors' positions
  std::vector<U16> cnt;         // sample: occurrences per term, saturating
  std::vector<U64> lh;          // list hits: term << 32 | doc
  std::vector<U32> hh;          // heavy hits: bitmap << 16 | doc
  std::vector<U32 *> cur, lim;  // per row shard: the open chunk
  U32 dn = 0, na = 0, nl = 0, nh = 0, doc = 0, rel = 0, base = 0, gcur = ~0u, hdr = 0, set = 0;
  bool fresh = true;
  U64 docs = 0;
  // build and answer scratch
  std::vector<U32> lcnt, lcur;
  std::vector<std::pair<U32, U32>> chunks;
  std::vector<U16> la, lb;
  std::vector<U64> bits, marks;
  std::unique_ptr<char[]> out;
  U64 out_len = 0;

  // ---- scan ----
  // Sets the bits of the heavy hits buffered since the last group of 64
  // documents began. The first and last group of a claim may be shared.
  void apply_heavy(bool shared) {
    U64 *o = bm + U64(set) * (nheavy + 1) * wpb;
    for (U32 i = 0; i < nh; ++i) {
      U64 *w = o + (hh[i] >> 16) * wpb + (hh[i] >> 6 & 1023), bit = 1ULL << (hh[i] & 63);
      if (shared)
        __atomic_fetch_or(w, bit, __ATOMIC_RELAXED);
      else
        *w |= bit;
    }
    nh = 0;
  }
  void open_chunk(U32 s) {
    U64 off = rclaimed[s].fetch_add(CHUNK, std::memory_order_relaxed);
    if (off + CHUNK > rcap)
      die("row shard full");
    U32 *c = rows + s * rcap + off;
    c[0] = hdr, cur[s] = c + 1, lim[s] = c + CHUNK;
  }
  void route() { // list hits to the row shards
    U32 mask = nshard - 1;
    for (U32 i = 0; i < nl; ++i) {
      U32 id = lh[i] >> 32, s = id & mask;
      if (cur[s] == lim[s])
        open_chunk(s);
      *cur[s]++ = ((id >> rbits) + 1) << 16 | U16(lh[i]);
    }
    nl = 0;
  }
  void phrases() { // the phrases anchored at each anchor of this document
    for (U32 i = 0; i < na; ++i)
      for (U32 p = an[i], k = phead[db[p] & LOW]; k;) {
        const Phrase &f = phr[k - 1];
        k = f.next;
        if (p < f.pivot || p - f.pivot + f.len > dn ||
            !std::equal(f.w, f.w + f.len, &db[p - f.pivot]))
          continue;
        if (nl == lh.size())
          lh.resize(2 * nl);
        lh[nl++] = U64(f.term) << 32 | rel;
      }
  }
  template <bool SAMPLE> void next_doc() { // at the id token of a document
    ++doc, ++docs;
    if constexpr (!SAMPLE) {
      if (na)
        phrases();
      rel = doc - base;
      if (rel >> 6 != gcur) {
        if (gcur != ~0u)
          apply_heavy(fresh), fresh = false;
        gcur = rel >> 6;
      }
    }
    dn = na = 0;
  }
  template <bool SAMPLE> void token(U32 a) {
    if constexpr (SAMPLE)
      cnt[a] += cnt[a] != 0xFFFF;
    else {
      db[dn] = a, an[na] = dn++, na += a >> 31;
      hh[nh] = (a & LOW) << 16 | rel, nh += a >> 30 & 1;
      lh[nl] = U64(a & LOW) << 32 | rel, nl += a >> 29 & 1;
    }
  }

  // Complete lines. Tokens are found 64 bytes at a time from a bitmask; a
  // token followed by TAB is a document id and starts a document.
  template <bool SAMPLE> void piece(const U8 *p, U32 len) {
    U32 g = 0;
    for (U32 i = 0; p[i] != '\t'; ++i)
      g = g * 10 + p[i] - '0';
    doc = g - 1;
    static thread_local U32 S[BATCH / 2 + 80], E[BATCH / 2 + 80];
    static thread_local U64 H[BATCH / 2 + 2], K1[BATCH / 2 + 2];
    const Table t = tbl;
    U64 carry = 0;
    for (U32 pos = 0; pos < len;) {
      U32 ns = carry ? 1 : 0, ne = 0, stop = std::min(len, pos + BATCH);
      for (; pos < stop; pos += 64) {
        U64 m = alnum_mask(p + pos);
        if (len - pos < 64)
          m = _bzhi_u64(m, len - pos);
        U64 prev = m << 1 | carry;
        carry = m >> 63;
        ns += flatten(S + ns, pos, m & ~prev);
        ne += flatten(E + ne, pos, ~m & prev);
      }
      for (U32 i = 0; i < ne; ++i) {
        U32 n = E[i] - S[i];
        Key k = p[E[i]] == '\t' ? Key{0, 1} : n > 32 ? Key{0, 2} : make_key(p + S[i], n);
        H[i] = hash_of(k), K1[i] = k.k1;
      }
      if (dn + ne >= db.size())
        db.resize(2 * (dn + ne)), an.resize(2 * (dn + ne));
      if (nl + ne >= lh.size())
        lh.resize(2 * (nl + ne));
      if (nh + ne >= hh.size())
        hh.resize(2 * (nh + ne));
      for (U32 i = 0; i < ne; ++i) {
        if (i + PF < ne)
          _mm_prefetch((const char *)&t.b[t.home(H[i + PF])], _MM_HINT_T0);
        // k1: 1 = a document id, 2 = over 32 bytes, bit 63 = 17-32 bytes
        if (U64 k1 = K1[i]; k1 - 1 < 2 || k1 >> 63) [[unlikely]] {
          if (k1 == 1)
            next_doc<SAMPLE>();
          else
            token<SAMPLE>(k1 == 2 ? 0 : t.slow(Key{0, k1}, H[i], p + S[i], E[i] - S[i]));
        } else
          token<SAMPLE>(t.find(k1, H[i]));
      }
      if (!SAMPLE)
        route();
      if (ns > ne)
        S[0] = S[ne];
    }
    if (!SAMPLE && na)
      phrases(), route();
    dn = na = 0;
  }

  // The lines that start in [from, to), read in chunks of CH bytes or more.
  template <bool SAMPLE> void range(U64 from, U64 to) {
    U64 at = from ? from - 1 : 0;
    U32 have = 0, s = 0;
    bool skip = from > 0, done = from >= to;
    while (!done) {
      U8 *b = buf.data();
      ssize_t r = pread(fd, b + have, buf.size() - PAD - have, at + have);
      if (r < 0)
        die("read docs.txt");
      have += r;
      U32 e = have;
      if (at + have < size) {
        auto *nl = (const U8 *)memrchr(b, '\n', have);
        if (!nl) { // a line longer than the buffer
          buf.resize(2 * buf.size());
          continue;
        }
        e = nl - b + 1;
      }
      if (skip)
        s = (const U8 *)memchr(b, '\n', e) - b + 1, skip = false;
      if (to - at <= e) { // stop after the line holding byte to - 1
        e = (const U8 *)memchr(b + (to - at) - 1, '\n', e - (to - at) + 1) - b + 1;
        done = true;
      }
      if (s < e)
        piece<SAMPLE>(b + s, e - s);
      done |= at + e >= size;
      memmove(b, b + e, have - e);
      at += e, have -= e, s = 0;
    }
  }
  void claim(U32 p, U32 j) {
    U64 from = pbyte[p] + U64(j) * CLAIM, to = std::min(pbyte[p + 1], from + CLAIM);
    base = pbase[p], set = p & 1, hdr = j + 1, gcur = ~0u, fresh = true;
    range<false>(from, to);
    apply_heavy(true);
    for (U32 s = 0; s < nshard; ++s) {
      if (cur[s] != lim[s])
        *cur[s] = 0; // the end of a chunk that is not full
      cur[s] = lim[s] = nullptr;
    }
  }

  // ---- build: one row shard's lists for phase p ----
  void build(U32 s, U32 p) {
    U32 *reg = rows + s * rcap;
    U64 claimed = rclaimed[s].load(std::memory_order_relaxed);
    chunks.clear();
    for (U64 o = 0; o < claimed; o += CHUNK)
      chunks.push_back({reg[o], U32(o)});
    std::sort(chunks.begin(), chunks.end()); // claim order = document order
    U32 nloc = (nid >> rbits) + 1;
    lcnt.assign(nloc, 0), lcur.resize(nloc);
    auto rows_of = [&](auto f) {
      for (auto [c, o] : chunks)
        for (U32 i = 1; i < CHUNK && reg[o + i]; ++i)
          f(reg[o + i]);
    };
    rows_of([&](U32 r) { ++lcnt[(r >> 16) - 1]; });
    U64 total = 0;
    for (U32 l = 0; l < nloc; ++l)
      total += lcnt[l] ? lcnt[l] + 1 : 0;
    U64 at = ctr[p].post.fetch_add(total, std::memory_order_relaxed);
    for (U32 l = 0; l < nloc; ++l) { // a sentinel before each list; lcnt := its start
      U32 c = lcnt[l];
      lcnt[l] = lcur[l] = c ? at + 1 : 0;
      if (c)
        post[at] = 0xFFFF, at += c + 1;
    }
    // A document repeats a term: its rows are adjacent, so a repeat is
    // overwritten by the next document.
    rows_of([&](U32 r) {
      U16 *o = post + lcur[(r >> 16) - 1];
      *o = U16(r);
      lcur[(r >> 16) - 1] += o[-1] != U16(r);
    });
    for (U32 l = 0; l < nloc; ++l)
      if (U32 id = l << rbits | s; id <= nid)
        info[id] = {lcnt[l], lcur[l] - lcnt[l]};
    rclaimed[s].store(0, std::memory_order_relaxed);
    U64 h0 = U64(nheavy + 1) * s / nshard, h1 = U64(nheavy + 1) * (s + 1) / nshard;
    memset(bm + (U64((p + 1) & 1) * (nheavy + 1) + h0) * wpb, 0, (h1 - h0) * wpb * 8);
  }

  // ---- answer ----
  Set set_of(U32 id, U32 p) const {
    U32 a = act[id];
    if (a & HEAVY)
      return {bm + (U64(p & 1) * (nheavy + 1) + (a & LOW)) * wpb, nullptr, 0};
    return {nullptr, post + info[id].first, info[id].n};
  }
  // Values of a in b (keep) or not in b. A list 16 times shorter is searched
  // for; otherwise one side is marked in `marks` and the other tested.
  U32 filter(const U16 *a, U32 an, const U16 *b, U32 bn, bool keep, U16 *o) {
    U32 m = 0;
    if (an * 16 <= bn) {
      for (U32 j = 0; j < an; ++j)
        o[m] = a[j], m += has(b, bn, a[j]) == keep;
      return m;
    }
    const U16 *x = keep ? a : b, *y = keep ? b : a;
    U32 xn = keep ? an : bn, yn = keep ? bn : an;
    U64 *mk = marks.data();
    for (U32 j = 0; j < xn; ++j)
      mk[x[j] >> 6] |= 1ULL << (x[j] & 63);
    for (U32 j = 0; j < yn; ++j)
      o[m] = y[j], m += (mk[y[j] >> 6] >> (y[j] & 63) & 1) == keep;
    for (U32 j = 0; j < xn; ++j)
      mk[x[j] >> 6] = 0;
    return m;
  }
  // One step of a BOOL query, strictly left to right.
  Set apply(char op, Set a, Set b) {
    if (!b.bits && !b.n) // an empty term
      return op == 'A' ? Set{nullptr, nullptr, 0} : a;
    if (op == 'A' && (a.bits ? !b.bits : !b.bits && b.n < a.n))
      std::swap(a, b);
    U16 *o = a.ids == la.data() || b.ids == la.data() ? lb.data() : la.data();
    if (op != 'O' && !a.bits) {
      U32 m = 0;
      if (!b.bits)
        m = filter(a.ids, a.n, b.ids, b.n, op == 'A', o);
      else
        for (U32 j = 0; j < a.n; ++j)
          o[m] = a.ids[j], m += (b.bits[a.ids[j] >> 6] >> (a.ids[j] & 63) & 1) == (op == 'A');
      return {nullptr, o, m};
    }
    if (op == 'O' && !a.bits && !b.bits && a.n + b.n < wpb) {
      U32 m = 0, i = 0, j = 0;
      while (i < a.n && j < b.n) {
        U16 x = a.ids[i], y = b.ids[j];
        o[m++] = x < y ? x : y, i += x <= y, j += y <= x;
      }
      while (i < a.n)
        o[m++] = a.ids[i++];
      while (j < b.n)
        o[m++] = b.ids[j++];
      return {nullptr, o, m};
    }
    U64 *r = bits.data();
    if (a.bits) {
      if (a.bits != r)
        memcpy(r, a.bits, wpb * 8);
    } else {
      memset(r, 0, wpb * 8);
      for (U32 j = 0; j < a.n; ++j)
        r[a.ids[j] >> 6] |= 1ULL << (a.ids[j] & 63);
    }
    if (b.bits) {
      if (op == 'A')
        for (U32 k = 0; k < wpb; ++k)
          r[k] &= b.bits[k];
      else if (op == 'O')
        for (U32 k = 0; k < wpb; ++k)
          r[k] |= b.bits[k];
      else
        for (U32 k = 0; k < wpb; ++k)
          r[k] &= ~b.bits[k];
    } else if (op == 'O')
      for (U32 j = 0; j < b.n; ++j)
        r[b.ids[j] >> 6] |= 1ULL << (b.ids[j] & 63);
    else
      for (U32 j = 0; j < b.n; ++j)
        r[b.ids[j] >> 6] &= ~(1ULL << (b.ids[j] & 63));
    return {r, nullptr, 0};
  }
  void answer(U32 batch, U32 p) {
    U32 base = pbase[p];
    for (U32 q = batch * QB, e = std::min(nq, q + QB); q < e; ++q) {
      Set r = set_of(qps[q] ? qps[q] : qt[qoff[q]], p);
      if (!qps[q])
        for (U32 i = 1; i < qn[q]; ++i)
          if (qop[qoff[q] + i] == 'O' || r.bits || r.n)
            r = apply(qop[qoff[q] + i], r, set_of(qt[qoff[q] + i], p));
      Tally &t = tal[q];
      U64 c = 0, s = 0;
      if (r.bits) {
        count_bits(r.bits, wpb, c, s);
        for (U32 w = 0, k = t.count; k < 10 && w < wpb; ++w)
          for (U64 x = r.bits[w]; x && k < 10; x &= x - 1)
            t.id[k++] = base + w * 64 + __builtin_ctzll(x);
      } else {
        for (U32 j = 0; j < r.n; ++j)
          s += r.ids[j];
        for (U32 j = 0; j < r.n && t.count + j < 10; ++j)
          t.id[t.count + j] = base + r.ids[j];
        c = r.n;
      }
      t.count += c, t.sum += s + c * base;
    }
  }
};

static std::vector<Worker> workers;

static U64 next_line(U64 pos) { // the first line start at or after pos
  if (pos == 0 || pos >= size)
    return std::min(pos, size);
  U8 b[4096];
  for (U64 at = pos - 1; at < size; at += sizeof b) {
    ssize_t r = pread(fd, b, sizeof b, at);
    if (r <= 0)
      break;
    if (auto *nl = (const U8 *)memchr(b, '\n', r))
      return at + (nl - b) + 1;
  }
  return size;
}
static U32 id_at(U64 line) { // the id of the line starting at `line`
  U8 b[24] = {};
  if (pread(fd, b, sizeof b - 1, line) <= 0)
    die("read docs.txt");
  U32 g = 0;
  for (U32 i = 0; b[i] >= '0' && b[i] <= '9'; ++i)
    g = g * 10 + b[i] - '0';
  return g;
}
// Phases of about PHASE bytes, each starting at a line, with at most 65,535
// documents each so that a document's offset in its phase fits 16 bits.
static void lay_out_phases() {
  U32 ndocs = 0;
  if (size) { // the id of the last line, + 1
    U64 at = size - 1;
    while (at) {
      U8 b[4096];
      U64 from = at > sizeof b ? at - sizeof b : 0;
      if (pread(fd, b, at - from, from) <= 0)
        die("read docs.txt");
      if (auto *nl = (const U8 *)memrchr(b, '\n', at - from)) {
        at = from + (nl - b) + 1;
        break;
      }
      at = from;
    }
    ndocs = id_at(at) + 1;
  }
  for (nph = std::max<U64>(1, (size + PHASE - 1) / PHASE);; nph *= 2) {
    pbyte.assign(nph + 1, size), pbase.assign(nph + 1, ndocs);
    U32 widest = 0;
    for (U32 p = 0; p < nph; ++p) {
      pbyte[p] = next_line(size * p / nph);
      pbase[p] = pbyte[p] < size ? id_at(pbyte[p]) : ndocs;
    }
    for (U32 p = 0; p < nph; ++p)
      widest = std::max(widest, pbase[p + 1] - pbase[p]);
    if (widest < 0xFFFF) {
      wpb = (widest / 64 + 4) & ~3u;
      break;
    }
  }
  ctr.reset(new Ctr[nph]);
}

static char *put(char *o, U64 v) {
  char t[20];
  int n = 0;
  do
    t[n++] = '0' + v % 10;
  while (v /= 10);
  while (n)
    *o++ = t[--n];
  return o;
}

static U64 query_line(U64 pos) { // first line start at or after pos
  if (pos == 0 || pos >= qsize)
    return std::min(pos, qsize);
  auto *nl = (const U8 *)memchr(qs + pos - 1, '\n', qsize - pos + 1);
  return nl ? nl - qs + 1 : qsize;
}

// Lines of queries.txt starting in this thread's slice, parsed by position:
// in BOOL the even words are terms, so `and`, `or`, `not` are just terms.
static void parse_queries(Worker &me, U32 w) {
  for (U64 p = query_line(qsize * w / threads), end = query_line(qsize * (w + 1) / threads);
       p < end;) {
    bool ph = qs[p] == 'P';
    p += ph ? 8 : 5;
    me.pph.push_back(ph);
    U32 n = 0;
    char op = 0;
    for (;; ++n) {
      U64 s = p;
      while (p < qsize && ((qs[p] >= 'a' && qs[p] <= 'z') || (qs[p] >= '0' && qs[p] <= '9')))
        ++p;
      Key k = make_key(qs + s, p - s);
      U64 h = hash_of(k);
      me.byown[h >> 58].push_back(me.pkey.size());
      me.pkey.push_back(k), me.phash.push_back(h), me.pat.push_back(s);
      me.plen.push_back((p - s) | (ph ? 0 : 0x80)), me.pop.push_back(op);
      if (p >= qsize || qs[p] != ' ')
        break;
      ++p;
      if (!ph) {
        op = qs[p];
        while (qs[p] != ' ')
          ++p;
        ++p;
      }
    }
    if (ph && n + 1 > 8)
      die("phrase longer than 8 words");
    me.pn.push_back(n + 1);
    while (p < qsize && qs[p++] != '\n')
      ;
  }
}

static U32 sbase[65], distinct[64]; // first term id of each table shard - 1; tags in it

// Serial: offsets of each thread's queries, term occurrences and phrases;
// the table (each shard sized by its occurrences, so it can never fill);
// per-term and per-query arrays.
static void lay_out() {
  U32 occ[64] = {};
  for (Worker &x : workers) {
    x.qbase = nq, x.tbase = nocc, x.pbase_ = nphr;
    nq += x.pn.size(), nocc += x.pkey.size();
    for (U8 f : x.pph)
      nphr += f;
    for (U32 s = 0; s < 64; ++s)
      occ[s] += x.byown[s].size();
  }
  U32 most = 0;
  for (U32 s = 0; s < 64; ++s)
    sbase[s + 1] = sbase[s] + occ[s], most = std::max(most, distinct[s]);
  tbl.sbits = 0; // load at most 5/8
  while (4u << tbl.sbits < most + most * 3 / 5 + 1)
    ++tbl.sbits;
  tbl.bits = 6 + tbl.sbits;
  tbl.b = reserve<Bucket>(1ULL << tbl.bits);
  tbl.k1 = reserve<U64>(4ULL << tbl.bits), tbl.tail = reserve<Tail>(4ULL << tbl.bits);
  nid = nocc + nphr;
  kind = reserve<U8>(nid + 1), gcnt = reserve<U32>(nid + 1), act = reserve<U32>(nid + 1);
  phead = reserve<U32>(nid + 1), info = reserve<Info>(nid + 1);
  qoff = reserve<U32>(nq + 1), qps = reserve<U32>(nq + 1), qn = reserve<U8>(nq + 1);
  qt = reserve<U32>(nocc + 1), qop = reserve<char>(nocc + 1);
  phr = reserve<Phrase>(nphr + 1), tal = reserve<Tally>(nq + 1);
  for (rbits = 6; (nid >> rbits) + 1 >= 0xFFFF;)
    ++rbits;
  nshard = 1u << rbits;
  U64 widest = 0;
  for (U32 p = 0; p < nph; ++p)
    widest = std::max(widest, pbyte[p + 1] - pbyte[p]);
  rcap = widest + (widest / CLAIM + 2) * CHUNK;
  rows = reserve<U32>(nshard * rcap);
  rclaimed.reset(new std::atomic<U64>[nshard]());
  post = reserve<U16>(nshard * rcap + nid + 64);
}

static void intern(Worker &me, U32 w) {
  for (U32 i = 0, t = me.tbase, f = me.pbase_; i < me.pn.size(); t += me.pn[i++]) {
    U32 q = me.qbase + i;
    qoff[q] = t, qn[q] = me.pn[i], qps[q] = me.pph[i] ? nocc + 1 + f++ : 0;
  }
  std::copy(me.pop.begin(), me.pop.end(), qop + me.tbase);
  for (U32 s = w; s < 64; s += threads) {
    U32 next = sbase[s] + 1;
    for (Worker &x : workers)
      for (U32 i : x.byown[s]) {
        U32 n = x.plen[i] & 0x7F;
        U32 t = qt[x.tbase + i] = insert(x.pkey[i], x.phash[i], next, qs + x.pat[i], n);
        kind[t] |= x.plen[i] >> 7 ? 1 : 2;
      }
  }
}

static void work(U32 w) {
  for (U32 c = 2 * w + 1; c <= 2 * w + 2 && c < threads; ++c)
    std::thread(work, c).detach(); // spawn as a tree
  Worker &me = workers[w];
  for (U64 got = qsize * w / threads, end = qsize * (w + 1) / threads; got < end;) {
    ssize_t r = pread(qfd, qmem + got, end - got, got);
    if (r <= 0)
      die("read queries.txt");
    got += r;
  }
  barrier();
  parse_queries(me, w);
  barrier();
  for (U32 s = w; s < 64; s += threads) {
    std::vector<U64> hs;
    for (Worker &x : workers)
      for (U32 i : x.byown[s])
        hs.push_back(x.phash[i]);
    std::sort(hs.begin(), hs.end());
    distinct[s] = std::unique(hs.begin(), hs.end()) - hs.begin();
  }
  barrier(lay_out);
  intern(me, w);
  barrier();
  // Phrase records; then the sample: occurrences of each term in 1/64 of the
  // corpus, in one slice per thread.
  for (U32 i = 0, t = me.tbase, f = me.pbase_; i < me.pn.size(); t += me.pn[i++])
    if (me.pph[i]) {
      Phrase &x = phr[f++];
      x.len = me.pn[i], x.term = nocc + 1 + f - 1;
      std::copy(qt + t, qt + t + x.len, x.w);
    }
  std::vector<U8>().swap(me.pn), std::vector<U8>().swap(me.plen);
  std::vector<char>().swap(me.pop), std::vector<Key>().swap(me.pkey);
  std::vector<U64>().swap(me.phash), std::vector<U32>().swap(me.pat);
  for (auto &v : me.byown)
    std::vector<U32>().swap(v);
  me.buf.resize(CH + PAD), me.db.resize(BATCH), me.an.resize(BATCH);
  me.cnt.assign(nid + 1, 0);
  U64 at = size * w / threads;
  me.range<true>(at, std::min(size, at + size / (64 * threads) + 1));
  for (U32 t = 1; t <= nid; ++t)
    if (me.cnt[t])
      std::atomic_ref<U32>(gcnt[t]).fetch_add(me.cnt[t], std::memory_order_relaxed);
  sample_docs += me.docs;
  std::vector<U16>().swap(me.cnt);
  barrier([] { delete[] qmem; });
  // Plan, in slices: anchor each phrase on its rarest word; give every term
  // its action; then put the actions in the table and the phrases.
  for (U32 f = U64(nphr) * w / threads; f < U64(nphr) * (w + 1) / threads; ++f) {
    Phrase &x = phr[f];
    for (U32 i = 1; i < x.len; ++i)
      if (gcnt[x.w[i]] < gcnt[x.w[x.pivot]])
        x.pivot = i;
    x.next = std::atomic_ref<U32>(phead[x.w[x.pivot]]).exchange(f + 1);
  }
  barrier();
  U64 hot = sample_docs / HOT_RATIO;
  for (U32 t = U64(nid) * w / threads + 1; t <= U64(nid) * (w + 1) / threads; ++t) {
    U32 a = t;
    if (t > nocc)
      a = LIST | t;
    else if (phead[t])
      a = ANCHOR | (kind[t] & 1 ? LIST : 0) | t;
    else if (kind[t] & 1) {
      a = LIST | t;
      if (gcnt[t] > hot)
        if (U32 h = heavy_ctr.fetch_add(1, std::memory_order_relaxed) + 1; h <= MAX_HEAVY)
          a = HEAVY | h;
    }
    act[t] = kind[t] || t > nocc ? a : 0;
  }
  barrier([] {
    nheavy = std::min(heavy_ctr.load(), MAX_HEAVY);
    bm = reserve<U64>(2 * U64(nheavy + 1) * wpb);
  });
  for (U64 i = (U64(1) << tbl.bits) * w / threads; i < (U64(1) << tbl.bits) * (w + 1) / threads; ++i)
    for (U32 j = 0; j < 4; ++j)
      tbl.b[i].act[j] = act[tbl.b[i].act[j]];
  for (U32 f = U64(nphr) * w / threads; f < U64(nphr) * (w + 1) / threads; ++f)
    for (U32 i = 0; i < phr[f].len; ++i)
      phr[f].w[i] = act[phr[f].w[i]];
  me.lh.resize(BATCH), me.hh.resize(BATCH);
  me.cur.assign(nshard, nullptr), me.lim.assign(nshard, nullptr);
  me.la.resize(wpb * 64), me.lb.resize(wpb * 64), me.bits.resize(wpb), me.marks.resize(wpb);
  barrier();
  U32 batches = (nq + QB - 1) / QB;
  for (U32 stage = 0; stage <= nph; ++stage) {
    if (stage < nph) {
      U32 claims = (pbyte[stage + 1] - pbyte[stage] + CLAIM - 1) / CLAIM;
      for (U32 j; (j = ctr[stage].claim.fetch_add(1)) < claims;)
        me.claim(stage, j);
    }
    if (stage > 0)
      for (U32 j; (j = ctr[stage - 1].answer.fetch_add(1)) < batches;)
        me.answer(j, stage - 1);
    if (stage == nph)
      break;
    barrier();
    for (U32 s; (s = ctr[stage].build.fetch_add(1)) < nshard;)
      me.build(s, stage);
    barrier();
  }
  barrier();
  U32 lo = U64(nq) * w / threads, hi = U64(nq) * (w + 1) / threads;
  me.out.reset(new char[U64(hi - lo) * 160 + 1]);
  char *o = me.out.get();
  for (U32 q = lo; q < hi; ++q) {
    o = put(o, tal[q].count), *o++ = ' ', o = put(o, tal[q].sum);
    for (U32 i = 0; i < std::min(tal[q].count, 10u); ++i)
      *o++ = ' ', o = put(o, tal[q].id[i]);
    *o++ = '\n';
  }
  me.out_len = o - me.out.get();
  barrier();
}

int main(int argc, char **argv) {
  mallopt(M_MMAP_THRESHOLD, 1 << 17); // fixed: freed big blocks go back
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // a huge page is charged in full
  std::string data = argc > 1 ? argv[1] : "/data";
  std::string outdir = argc > 2 ? argv[2] : "/out";
  qfd = open((data + "/queries.txt").c_str(), O_RDONLY);
  struct stat st;
  if (qfd < 0 || fstat(qfd, &st))
    die("cannot open queries.txt");
  qsize = st.st_size;
  qs = qmem = new U8[qsize + PAD]; // read in parallel slices by the threads
  memset(qmem + qsize, 0, PAD);
  fd = open((data + "/docs.txt").c_str(), O_RDONLY);
  if (fd < 0 || fstat(fd, &st))
    die("cannot open docs.txt");
  size = st.st_size;
  lay_out_phases();

  cpu_set_t cpus;
  threads = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  if (const char *e = getenv("SD_THREADS"))
    threads = std::max(1, atoi(e));
  workers.resize(threads);
  work(0); // returns after every thread has formatted its share
  mkdir(outdir.c_str(), 0755);
  int out = open((outdir + "/results.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    die("cannot write results.txt");
  for (const Worker &x : workers)
    for (U64 done = 0; done < x.out_len;) {
      ssize_t r = write(out, x.out.get() + done, x.out_len - done);
      if (r <= 0)
        die("write results.txt");
      done += r;
    }
  _exit(0);
}
