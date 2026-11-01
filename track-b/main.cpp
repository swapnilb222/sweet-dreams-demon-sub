// Track B: one query-directed pass over the corpus, no global index.
//
// 1. The threads parse queries.txt in slices and build an exact dictionary of
//    the query terms, one shard per thread.
// 2. A 1/64 sample of docs.txt estimates document frequencies. They decide
//    speed only, never answers: hot Boolean terms get a bitmap row per block,
//    cold ones a sorted list; each phrase gets its rarest term as anchor; each
//    Boolean query a guard set of terms that must occur for a block to matter.
// 3. docs.txt is read in morsels claimed from a shared cursor. A block collects
//    D documents of one thread (from several morsels if need be). At its end
//    every guarded query is evaluated on it, strictly left to right; phrases
//    are checked per document around anchor occurrences.
// 4. Counts and id sums are added to shared results; the ten smallest ids are
//    merged under a per-query spin flag, so answers never depend on timing.
#include <fcntl.h>
#include <malloc.h>
#include <immintrin.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using U8 = uint8_t;
using U16 = uint16_t;
using U32 = uint32_t;
using U64 = uint64_t;

// D documents per block (W words per bitmap row); lists of up to LMAX ids are
// kept as lists by OR; BATCH bytes are tokenized per step; dictionary buckets
// are prefetched PF tokens ahead; CH bytes are read at a time.
constexpr U32 D = 2048, W = D / 64, LMAX = D / 16, BATCH = 4096, PF = 8;
constexpr U32 CH = 1 << 17, HOT_RATIO = 32, PAD = 128;
constexpr U32 ANCHORF = 1u << 31, COLDF = 1u << 30, IDMASK = COLDF - 1;

[[noreturn]] static void die(const char *what) {
  fprintf(stderr, "error: %s\n", what);
  _exit(1);
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

// Query-term dictionary: three exact keys per 64-byte bucket. One shard per
// thread (chosen by the top hash bits) so that the threads build it in
// parallel; `more` = 1 + next bucket of an overflow chain inside the shard.
// Values are term ids with ANCHORF / COLDF flags; 0 = absent.
struct alignas(64) Bucket {
  U64 k0[3], k1[3];
  U32 val[3], more;
};
struct Dict {
  Bucket *b = nullptr;
  U64 nb = 0;
  std::vector<std::array<U32, 2>> shard; // first bucket, bucket count
  static U64 hash(Key k) { return mix(k.k0, k.k1); }
  U32 owner(U64 h) const { return (h >> 32) * shard.size() >> 32; }
  U32 index(U64 h) const {
    auto [base, n] = shard[owner(h)];
    return base + (U32(h >> 16) * U64(n) >> 32);
  }
  U32 find(Key k, U32 i) const {
    for (;;) {
      const Bucket &x = b[i];
      U32 r = 0;
      for (int j = 0; j < 3; ++j)
        r |= x.k0[j] == k.k0 && x.k1[j] == k.k1 ? x.val[j] : 0;
      if (r || !x.more) [[likely]]
        return r;
      i = x.more - 1;
    }
  }
  // Only the shard's owner calls this. Returns the id of k, or assigns next.
  U32 insert(Key k, U64 h, U32 &next) {
    auto [base, n] = shard[owner(h)];
    for (U32 i = index(h);;) {
      Bucket &x = b[i];
      for (int j = 0; j < 3; ++j) {
        if (!x.val[j]) {
          x.k0[j] = k.k0, x.k1[j] = k.k1;
          return x.val[j] = ++next;
        }
        if (x.k0[j] == k.k0 && x.k1[j] == k.k1)
          return x.val[j];
      }
      if (!x.more)
        x.more = (i + 1 == base + n ? base : i + 1) + 1;
      i = x.more - 1;
    }
  }
};

struct alignas(64) Result { // shared by all threads
  std::atomic<U64> sum{0};
  std::atomic<U32> count{0}, bound{UINT32_MAX};
  std::atomic<bool> busy{false};
  U8 n = 0;
  U32 id[10]; // the smallest matching ids so far
};

static int fd;
static U64 size;
static Dict dict;
static std::unordered_map<U32, Tail> long_terms;
static std::vector<U32> qoff, qt, df, abeg, alist, gbeg, glist, always;
static std::vector<U8> qn, qph, qsel, kind; // kind: 0 none, 1 phrase, 2 bool
static std::vector<char> qop;
static Result *res;
static std::atomic<U64> cursor{0}, sample_docs{0};
static const U8 *qs; // queries.txt
static U8 *qmem;
static int qfd;
static U64 qsize;
static U32 threads, nq, nterms, nhot, ncold, nraw;

static void offer(U32 q, const U32 *ids, U32 k) {
  Result &f = res[q];
  if (ids[0] >= f.bound.load(std::memory_order_relaxed))
    return;
  while (f.busy.exchange(true, std::memory_order_acquire))
    _mm_pause();
  U32 m[10], a = 0, b = 0, n = 0;
  while (n < 10 && (a < f.n || b < k))
    m[n++] = b == k || (a < f.n && f.id[a] < ids[b]) ? f.id[a++] : ids[b++];
  memcpy(f.id, m, n * 4);
  f.n = n;
  if (n == 10)
    f.bound.store(m[9], std::memory_order_relaxed);
  f.busy.store(false, std::memory_order_release);
}
static void add_result(U32 q, U32 c, U64 s, const U32 *ids, U32 k) {
  if (!c)
    return;
  res[q].count.fetch_add(c, std::memory_order_relaxed);
  res[q].sum.fetch_add(s, std::memory_order_relaxed);
  if (k)
    offer(q, ids, k);
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
  for (U32 i = 0; i < 16; ++i)
    o[i] = base + _tzcnt_u64(bits), bits = _blsr_u64(bits);
  for (U32 i = 16; i < n; ++i)
    o[i] = base + _tzcnt_u64(bits), bits = _blsr_u64(bits);
  return n;
}
static U32 bitsum(U64 w) { // sum of the positions of the set bits
  return __builtin_popcountll(w & 0xAAAAAAAAAAAAAAAAULL) +
         2 * __builtin_popcountll(w & 0xCCCCCCCCCCCCCCCCULL) +
         4 * __builtin_popcountll(w & 0xF0F0F0F0F0F0F0F0ULL) +
         8 * __builtin_popcountll(w & 0xFF00FF00FF00FF00ULL) +
         16 * __builtin_popcountll(w & 0xFFFF0000FFFF0000ULL) +
         32 * __builtin_popcountll(w & 0xFFFFFFFF00000000ULL);
}

struct Set { // a term within the block: a hot bitmap row or a sorted list
  const U64 *row;
  const U16 *ids;
  U32 n;
  bool empty() const { return !row && !n; }
};
// a AND b and a NOT b on sorted lists, written to o (o may alias a). A side
// at least 8 times shorter is probed by binary search, otherwise merged.
static U32 intersect(const U16 *a, U32 an, const U16 *b, U32 bn, U16 *o) {
  U32 m = 0;
  if (an * 8 < bn || bn * 8 < an) {
    bool small_a = an < bn;
    const U16 *x = small_a ? a : b, *y = small_a ? b : a, *lo = y;
    for (U32 i = 0, xn = small_a ? an : bn; i < xn; ++i) {
      lo = std::lower_bound(lo, y + (small_a ? bn : an), x[i]);
      if (lo != y + (small_a ? bn : an) && *lo == x[i])
        o[m++] = x[i]; // conditional: o may alias the searched side
    }
    return m;
  }
  for (U32 i = 0, j = 0; i < an && j < bn;) {
    o[m] = a[i], m += a[i] == b[j];
    U16 x = a[i], y = b[j];
    i += x <= y, j += y <= x;
  }
  return m;
}
static U32 subtract(const U16 *a, U32 an, const U16 *b, U32 bn, U16 *o) {
  U32 m = 0, i = 0, j = 0;
  if (bn > 8 * an) {
    for (; i < an; ++i) {
      j = std::lower_bound(b + j, b + bn, a[i]) - b;
      o[m] = a[i], m += j == bn || b[j] != a[i];
    }
    return m;
  }
  while (i < an && j < bn) { // branch-free merge step
    U16 x = a[i], y = b[j];
    o[m] = x, m += x < y, i += x <= y, j += y <= x;
  }
  while (i < an)
    o[m++] = a[i++];
  return m;
}
static U32 unite(const U16 *a, U32 an, const U16 *b, U32 bn, U16 *o) {
  U32 m = 0, i = 0, j = 0;
  while (i < an && j < bn) { // branch-free merge step
    U16 x = a[i], y = b[j];
    o[m++] = x < y ? x : y, i += x <= y, j += y <= x;
  }
  while (i < an)
    o[m++] = a[i++];
  while (j < bn)
    o[m++] = b[j++];
  return m;
}

struct Seg { // block-local documents from `local` on have id local + delta
  U32 local, delta;
};

struct Worker {
  // last: dedupe per cold term; pt: cold terms per block, 0 = next document;
  // db: the document's term ids; an: positions of its anchors.
  std::vector<U32> last, scnt, pt, db, an, touched, pm;
  std::vector<U16> csr, la, lb;
  std::vector<std::array<U32, 2>> cl; // cold term: [begin, end) in csr
  std::vector<U64> rows, qbits;
  std::vector<Seg> segs;
  // queries parsed by this thread: per line, then per term occurrence
  std::vector<U8> pn, pph, plen; // plen: length, bit 7 = Boolean
  std::vector<char> pop;
  std::vector<Key> pkey;
  std::vector<U64> phash;
  std::vector<U32> pat, pown;
  std::vector<std::vector<U32>> byown; // occurrences per shard owner
  std::vector<std::pair<U32, Tail>> longs;
  U32 qbase = 0, tbase = 0;
  std::vector<U8> buf;
  std::unique_ptr<char[]> out;
  U64 out_len = 0, docs = 0;
  U32 np = 0, dn = 0, na = 0, doc = 0, l = ~0u, nloc = 0, id = 0;

  Set get(U32 t) const {
    if (t <= nhot)
      return {&rows[U64(t) * W], nullptr, 1};
    auto [b, e] = cl[t - nhot - 1];
    return {nullptr, csr.data() + b, e - b};
  }

  // Strictly left to right; R is a sorted list (rp, rn) or a bitmap row rr.
  // A list result is written in place when rp is already one of our buffers.
  void eval(U32 q) {
    alignas(32) U64 rr[W];
    U16 *const A = la.data(), *const B = lb.data();
    const U16 *rp = A;
    U32 rn = 0;
    bool row = false;
    const U32 *t = &qt[qoff[q]];
    const char *op = &qop[qoff[q]];
    Set s = get(t[0]);
    if (s.row)
      memcpy(rr, s.row, sizeof rr), row = true;
    else
      rp = s.ids, rn = s.n;
    for (U32 i = 1; i < qn[q]; ++i) {
      s = get(t[i]);
      U16 *o = rp == B ? B : A;
      if (s.empty()) {
        if (op[i] == 'A')
          row = false, rn = 0;
      } else if (op[i] == 'O') {
        if (!row && !s.row && rn + s.n <= LMAX) {
          o = rp == A ? B : A;
          rn = unite(rp, rn, s.ids, s.n, o), rp = o;
          continue;
        }
        if (!row) {
          memset(rr, 0, sizeof rr), row = true;
          for (U32 j = 0; j < rn; ++j)
            rr[rp[j] >> 6] |= 1ULL << (rp[j] & 63);
        }
        if (s.row)
          for (U32 k = 0; k < W; ++k)
            rr[k] |= s.row[k];
        else
          for (U32 j = 0; j < s.n; ++j)
            rr[s.ids[j] >> 6] |= 1ULL << (s.ids[j] & 63);
      } else if (row) {
        if (s.row)
          for (U32 k = 0; k < W; ++k)
            rr[k] &= op[i] == 'A' ? s.row[k] : ~s.row[k];
        else if (op[i] == 'A') {
          rn = 0, row = false, rp = A;
          for (U32 j = 0; j < s.n; ++j)
            A[rn] = s.ids[j], rn += rr[s.ids[j] >> 6] >> (s.ids[j] & 63) & 1;
        } else
          for (U32 j = 0; j < s.n; ++j)
            rr[s.ids[j] >> 6] &= ~(1ULL << (s.ids[j] & 63));
      } else {
        U32 m = 0;
        if (s.row) {
          U64 flip = op[i] == 'A' ? 0 : 1;
          for (U32 j = 0; j < rn; ++j)
            o[m] = rp[j], m += (s.row[rp[j] >> 6] >> (rp[j] & 63) & 1) ^ flip;
        } else if (op[i] == 'A')
          m = intersect(rp, rn, s.ids, s.n, o);
        else
          m = subtract(rp, rn, s.ids, s.n, o);
        rn = m, rp = o;
      }
    }
    U32 c = 0, ids[10], k = 0, j = 0;
    U64 sum = 0;
    bool want = segs[0].local + segs[0].delta <
                res[q].bound.load(std::memory_order_relaxed);
    if (!row) {
      for (U32 i = 0; i < rn; ++i) {
        while (j + 1 < segs.size() && rp[i] >= segs[j + 1].local)
          ++j;
        sum += rp[i] + segs[j].delta;
        if (k < 10)
          ids[k++] = rp[i] + segs[j].delta;
      }
      add_result(q, rn, sum, ids, want ? k : 0);
      return;
    }
    for (; j < segs.size(); ++j) {
      U32 lo = segs[j].local, hi = j + 1 < segs.size() ? segs[j + 1].local : nloc;
      U32 cj = 0;
      for (U32 w = lo >> 6; lo < hi && w <= (hi - 1) >> 6; ++w) {
        U64 x = rr[w];
        if (w == lo >> 6)
          x &= ~0ULL << (lo & 63);
        if (w == (hi - 1) >> 6)
          x &= ~0ULL >> (63 - ((hi - 1) & 63));
        U32 pc = __builtin_popcountll(x);
        cj += pc, sum += U64(pc) * 64 * w + bitsum(x);
        for (; want && x && k < 10; x &= x - 1)
          ids[k++] = w * 64 + __builtin_ctzll(x) + segs[j].delta;
      }
      c += cj, sum += U64(cj) * segs[j].delta;
    }
    add_result(q, c, sum, ids, k);
  }

  void flush() { // end of a block: lists for cold terms, then the queries
    if (csr.size() < np)
      csr.resize(np + np / 4);
    if (touched.size() <= std::min(np, ncold))
      touched.resize(std::min(np, ncold) + 1);
    U32 nt = 0, off = 0, loc = ~0u;
    for (U32 i = 0; i < np; ++i)
      if (U32 t = pt[i]) {
        U32 &c = cl[t - nhot - 1][1];
        touched[nt] = t, nt += c++ == 0;
      }
    for (U32 i = 0; i < nt; ++i) {
      auto &c = cl[touched[i] - nhot - 1];
      c[0] = off, off += c[1], c[1] = c[0];
    }
    for (U32 i = 0; i < np; ++i)
      if (U32 t = pt[i])
        csr[cl[t - nhot - 1][1]++] = loc;
      else
        ++loc;
    std::fill(qbits.begin(), qbits.end(), 0);
    for (U32 q : always)
      qbits[q >> 6] |= 1ULL << (q & 63);
    for (U32 i = 0; i < nt; ++i)
      for (U32 k = gbeg[touched[i]]; k < gbeg[touched[i] + 1]; ++k)
        qbits[glist[k] >> 6] |= 1ULL << (glist[k] & 63);
    // Ascending from a per-thread offset: sequential query data, and threads
    // rarely update the same shared results at the same time.
    U32 nw = qbits.size(), w = U64(nw) * id / threads;
    for (U32 i = 0; i < nw; ++i, w = w + 1 == nw ? 0 : w + 1)
      for (U64 x = qbits[w]; x; x &= x - 1)
        eval(w * 64 + __builtin_ctzll(x));
    for (U32 i = 0; i < nt; ++i)
      cl[touched[i] - nhot - 1] = {0, 0};
    std::fill(rows.begin(), rows.end(), 0);
    np = 0, segs.clear();
  }
  void phrases() { // windows around each anchor occurrence of this document
    for (U32 i = 0; i < na; ++i)
      for (U32 p = an[i], k = abeg[db[p]]; k < abeg[db[p] + 1]; ++k) {
        U32 q = alist[k], a = qsel[q], n = qn[q];
        if (p < a || p - a + n > dn)
          continue;
        if (!std::equal(&db[p - a], &db[p - a + n], &qt[qoff[q]]) ||
            std::find(pm.begin(), pm.end(), q) != pm.end())
          continue;
        pm.push_back(q);
        add_result(q, 1, doc, &doc, 1);
      }
    pm.clear();
  }
  template <bool SAMPLE> void next_doc() { // at the id token of a document
    if constexpr (!SAMPLE) {
      if (na)
        phrases();
      if (++l == D) {
        nloc = D, flush(), l = 0;
        segs.push_back({0, doc + 1});
      }
      pt[np++] = 0;
    }
    dn = na = 0, ++doc, ++docs;
  }
  template <bool SAMPLE> void token(U32 v) {
    U32 t = v & IDMASK;
    if constexpr (SAMPLE)
      scnt[t] += last[t] != doc, last[t] = doc;
    else {
      db[dn] = t, an[na] = dn++, na += v >> 31;
      U32 r = t <= nhot ? t : 0;
      rows[U64(r) * W + (l >> 6)] |= 1ULL << (l & 63);
      U32 cold = v >> 30 & 1, x = cold ? t - nhot : 0; // last[0]: dummy
      pt[np] = t, np += cold & (last[x] != doc), last[x] = doc;
    }
  }

  // Complete lines. Tokens are found 64 bytes at a time from a bitmask; a
  // token followed by TAB is a document id and starts a document.
  template <bool SAMPLE> void piece(const U8 *p, U32 len) {
    U32 g = 0;
    for (U32 i = 0; p[i] != '\t'; ++i)
      g = g * 10 + p[i] - '0';
    if (!SAMPLE && (segs.empty() || g != doc + 1))
      segs.push_back({l + 1, g - (l + 1)});
    doc = g - 1;
    static thread_local U32 S[BATCH / 2 + 80], E[BATCH / 2 + 80], H[BATCH / 2 + 2];
    static thread_local Key K[BATCH / 2 + 2];
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
        K[i] = p[E[i]] == '\t' ? Key{0, 1}
               : n > 32        ? Key{0, 0}
                               : make_key(p + S[i], n);
        H[i] = dict.index(Dict::hash(K[i]));
      }
      if (dn + ne >= db.size())
        db.resize(2 * (dn + ne)), an.resize(2 * (dn + ne));
      if (np + ne >= pt.size())
        pt.resize(np + ne + np / 4 + BATCH);
      for (U32 i = 0; i < ne; ++i) {
        if (i + PF < ne)
          _mm_prefetch((const char *)&dict.b[H[i + PF]], _MM_HINT_T0);
        if (K[i].k0 == 0 && K[i].k1 == 1) {
          next_doc<SAMPLE>();
          continue;
        }
        U32 v = dict.find(K[i], H[i]);
        if (v && K[i].k1 >> 63 &&
            long_terms.at(v & IDMASK) != tail_words(p + S[i], E[i] - S[i]))
          v = 0;
        token<SAMPLE>(v);
      }
      if (ns > ne)
        S[0] = S[ne];
    }
    if (!SAMPLE && na)
      phrases();
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
};

static std::vector<Worker> workers;

static bool claim(U64 &from, U64 &to) {
  U64 cur = cursor.load(), next;
  do {
    if (cur >= size)
      return false;
    U64 step = std::clamp<U64>((size - cur) / (4 * threads), 256 << 10, 6 << 20);
    next = std::min(size, cur + step);
  } while (!cursor.compare_exchange_weak(cur, next));
  from = cur, to = next;
  return true;
}

// Speed only, never answers. Hot Boolean terms (sampled df >= N / HOT_RATIO)
// get ids 1..nhot and a bitmap row; cold ones get nhot+1..nhot+ncold and
// per-block lists. Phrase anchor = rarest term. Boolean guard = cheapest set G
// with every result inside the union of S(t), t in G: t_j for j = 0 or an AND
// position, plus every OR term after j.
static void plan() {
  U64 hot_df = sample_docs / HOT_RATIO;
  std::vector<U32> perm(nraw + 1), ndf(nraw + 1), flag(nraw + 1);
  U32 id = 0;
  for (int pass = 0; pass < 3; ++pass)
    for (U32 t = 1; t <= nraw; ++t)
      if (kind[t] == (pass < 2 ? 2 : 1) &&
          (pass == 2 || (pass == 0) == (df[t] > hot_df)))
        perm[t] = ++id, ndf[id] = df[t], flag[id] = pass == 1 ? COLDF : 0,
        nhot += pass == 0, ncold += pass == 1;
  nterms = id, ndf.resize(nterms + 1), df.swap(ndf);
  for (U32 &t : qt)
    t = perm[t];
  std::unordered_map<U32, Tail> renamed;
  for (auto &[t, tail] : long_terms)
    renamed[perm[t]] = tail;
  long_terms.swap(renamed);
  std::vector<U32> acount(nterms + 2), gcount(nterms + 2);
  auto cost = [](U32 t) { return U64(df[t]) + 1; };
  for (U32 q = 0; q < nq; ++q) {
    const U32 *t = &qt[qoff[q]];
    const char *op = &qop[qoff[q]];
    if (qph[q]) {
      for (U32 i = 1; i < qn[q]; ++i)
        if (df[t[i]] < df[t[qsel[q]]])
          qsel[q] = i;
      ++acount[t[qsel[q]]], flag[t[qsel[q]]] |= ANCHORF;
      continue;
    }
    U64 best = UINT64_MAX;
    for (U32 j = 0; j < qn[q]; ++j)
      if (j == 0 || op[j] == 'A') {
        U64 c = cost(t[j]);
        for (U32 k = j + 1; k < qn[q]; ++k)
          c += op[k] == 'O' ? cost(t[k]) : 0;
        if (c < best)
          best = c, qsel[q] = j;
      }
    bool hot = false;
    for (U32 k = qsel[q]; k < qn[q]; ++k)
      hot |= (k == qsel[q] || op[k] == 'O') && t[k] <= nhot;
    if (hot)
      always.push_back(q);
    else
      for (U32 k = qsel[q]; k < qn[q]; ++k)
        gcount[t[k]] += k == qsel[q] || op[k] == 'O';
  }
  auto prefix = [](std::vector<U32> &count, std::vector<U32> &beg,
                   std::vector<U32> &list) {
    beg.assign(count.size(), 0);
    for (U32 i = 1; i < count.size(); ++i)
      beg[i] = beg[i - 1] + count[i - 1];
    list.resize(beg.back());
    std::copy(beg.begin(), beg.end() - 1, count.begin());
  };
  prefix(acount, abeg, alist);
  prefix(gcount, gbeg, glist);
  for (U32 q = 0, h = 0; q < nq; ++q) {
    const U32 *t = &qt[qoff[q]];
    if (qph[q])
      alist[acount[t[qsel[q]]]++] = q;
    else if (h < always.size() && always[h] == q)
      ++h;
    else
      for (U32 k = qsel[q]; k < qn[q]; ++k)
        if (k == qsel[q] || qop[qoff[q] + k] == 'O')
          glist[gcount[t[k]]++] = q;
  }
  for (U64 i = 0; i < dict.nb; ++i)
    for (U32 &v : dict.b[i].val)
      v = perm[v] | flag[perm[v]];
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
  U32 owners = threads;
  me.pown.assign(owners, 0), me.byown.resize(owners);
  for (U64 p = query_line(qsize * w / threads),
           end = query_line(qsize * (w + 1) / threads);
       p < end;) {
    bool ph = qs[p] == 'P';
    p += ph ? 8 : 5;
    me.pph.push_back(ph);
    U32 n = 0;
    char op = 0;
    for (;; ++n) {
      U64 s = p;
      while (p < qsize && ((qs[p] >= 'a' && qs[p] <= 'z') ||
                           (qs[p] >= '0' && qs[p] <= '9')))
        ++p;
      Key k = make_key(qs + s, p - s);
      U64 h = Dict::hash(k);
      me.pkey.push_back(k), me.phash.push_back(h), me.pat.push_back(s);
      me.plen.push_back((p - s) | (ph ? 0 : 0x80)), me.pop.push_back(op);
      U32 o = (h >> 32) * owners >> 32;
      ++me.pown[o], me.byown[o].push_back(me.pkey.size() - 1);
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
    me.pn.push_back(n + 1);
    while (p < qsize && qs[p++] != '\n')
      ;
  }
}

// Serial step between parse phases: offsets of each thread's queries and
// term occurrences, shard sizes (half the occurrences each, so 3 slots per
// bucket can never fill up) and raw id ranges (occurrence offsets).
static void lay_out() {
  U32 qn_total = 0, occ = 0, buckets = 0;
  for (Worker &x : workers)
    x.qbase = qn_total, x.tbase = occ, qn_total += x.pn.size(),
    occ += x.pkey.size();
  nq = qn_total, nraw = occ;
  dict.shard.resize(threads);
  std::vector<U32> owned(threads);
  for (Worker &x : workers)
    for (U32 o = 0; o < threads; ++o)
      owned[o] += x.pown[o];
  for (U32 o = 0, id = 0; o < threads; ++o) {
    dict.shard[o] = {buckets, owned[o] / 2 + 1};
    buckets += owned[o] / 2 + 1;
    workers[o].pown.assign(1, id), id += owned[o]; // first raw id - 1
  }
  dict.nb = buckets;
  dict.b = (Bucket *)aligned_alloc(64, buckets * U64(64));
  qoff.resize(nq + 1), qn.resize(nq), qph.resize(nq), qsel.resize(nq);
  qt.resize(occ), qop.resize(occ), kind.resize(occ + 1), df.resize(occ + 1);
  qoff[nq] = occ;
  res = (Result *)aligned_alloc(64, std::max<U64>(nq, 1) * sizeof(Result));
}

static void build_dictionary(Worker &me, U32 w) {
  for (U32 i = 0, t = me.tbase; i < me.pn.size(); t += me.pn[i++])
    qoff[me.qbase + i] = t, qn[me.qbase + i] = me.pn[i],
                    qph[me.qbase + i] = me.pph[i];
  std::copy(me.pop.begin(), me.pop.end(), qop.begin() + me.tbase);
  for (U32 q = U64(nq) * w / threads; q < U64(nq) * (w + 1) / threads; ++q)
    new (&res[q]) Result();
  auto [base, n] = dict.shard[w];
  memset(dict.b + base, 0, U64(n) * sizeof(Bucket));
  U32 next = me.pown[0];
  for (Worker &x : workers)
    for (U32 i : x.byown[w]) {
      U32 t = qt[x.tbase + i] = dict.insert(x.pkey[i], x.phash[i], next);
      U32 len = x.plen[i] & 0x7F;
      kind[t] = std::max<U8>(kind[t], x.plen[i] >> 7 ? 2 : 1);
      if (len > 16 && next == t) // first occurrence of a long term
        me.longs.push_back({t, tail_words(qs + x.pat[i], len)});
    }
}

static std::barrier<void (*)()> *barrier;

static void work(U32 w) {
  for (U32 c = 2 * w + 1; c <= 2 * w + 2 && c < threads; ++c)
    std::thread(work, c).detach(); // spawn as a tree
  Worker &me = workers[w];
  auto &bar = *barrier;
  for (U64 got = qsize * w / threads, end = qsize * (w + 1) / threads;
       got < end;) {
    ssize_t r = pread(qfd, qmem + got, end - got, got);
    if (r <= 0)
      die("read queries.txt");
    got += r;
  }
  bar.arrive_and_wait();
  parse_queries(me, w);
  bar.arrive_and_wait(); // lay_out
  build_dictionary(me, w);
  bar.arrive_and_wait(); // long terms merged
  std::vector<U8>().swap(me.pn), std::vector<U8>().swap(me.pph);
  std::vector<U8>().swap(me.plen), std::vector<char>().swap(me.pop);
  std::vector<Key>().swap(me.pkey), std::vector<U64>().swap(me.phash);
  std::vector<U32>().swap(me.pat);
  me.buf.resize(CH + PAD);
  me.last.assign(nraw + 1, ~0u), me.scnt.assign(nraw + 1, 0);
  me.db.resize(BATCH), me.an.resize(BATCH);
  U64 at = size * w / threads;
  me.range<true>(at, std::min(size, at + size / (64 * threads) + 1));
  for (U32 t = 1; t <= nraw; ++t)
    if (me.scnt[t])
      std::atomic_ref<U32>(df[t]).fetch_add(me.scnt[t]);
  sample_docs += me.docs;
  std::vector<U32>().swap(me.scnt);
  std::vector<U32>().swap(me.last);
  bar.arrive_and_wait();
  me.last.assign(ncold + 1, ~0u);
  me.cl.resize(ncold), me.rows.resize(U64(nhot + 1) * W), me.id = w;
  me.qbits.resize(nq / 64 + 1), me.la.resize(D), me.lb.resize(D);
  // Capacity reserved once (untouched pages cost nothing): growing would
  // remap, and every munmap interrupts all cores for a TLB flush.
  me.pt.reserve(U64(D) * 64 + BATCH), me.csr.reserve(U64(D) * 64);
  me.touched.reserve(std::min<U64>(ncold + 1, U64(D) * 64));
  me.l = ~0u;
  for (U64 from, to; claim(from, to);)
    me.range<false>(from, to);
  if (me.l != ~0u)
    me.nloc = me.l + 1, me.flush();
  bar.arrive_and_wait();
  U32 lo = U64(nq) * w / threads, hi = U64(nq) * (w + 1) / threads;
  me.out.reset(new char[U64(hi - lo) * 160 + 1]);
  char *o = me.out.get();
  for (U32 q = lo; q < hi; ++q) {
    o = put(o, res[q].count), *o++ = ' ', o = put(o, res[q].sum);
    for (U32 i = 0; i < res[q].n; ++i)
      *o++ = ' ', o = put(o, res[q].id[i]);
    *o++ = '\n';
  }
  me.out_len = o - me.out.get();
  bar.arrive_and_wait();
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

  cpu_set_t cpus;
  threads = sched_getaffinity(0, sizeof cpus, &cpus) ? 1 : CPU_COUNT(&cpus);
  if (const char *e = getenv("SD_THREADS"))
    threads = std::max(1, atoi(e));
  workers.resize(threads);
  std::barrier<void (*)()> bar(threads, [] {
    static int phase = 0;
    if (phase == 1)
      lay_out();
    if (phase == 2) {
      for (Worker &x : workers)
        for (auto &[t, tail] : x.longs)
          long_terms[t] = tail;
      delete[] qmem;
    }
    if (phase == 3)
      plan();
    ++phase;
  });
  barrier = &bar;
  work(0); // returns after every thread has formatted its share
  mkdir(outdir.c_str(), 0755);
  int out = open((outdir + "/results.txt").c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
