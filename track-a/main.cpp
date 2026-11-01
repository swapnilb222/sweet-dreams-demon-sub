// Track A: per-hour, per-endpoint request counts, p50/p95/p99 and 5xx rates.
// The log is nearly time-ordered: blocks are parsed into per-hour runs and an
// hour is finalized once the finished file prefix is kLag hours past it.
// A late line that breaks this is detected and the run is repeated exactly.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t kHours = 72, kIds = 2048, kBlock = 256 << 10, kStreams = 4;
constexpr uint32_t kLag = 2, kNever = 1000, kKeep = 2;
constexpr uint32_t kMaxLine = 178, kRunCap = kBlock / 40 + 16, kPartCap = kIds * 256;
const char *const kDays[] = {"2026-10-30", "2026-10-31", "2026-11-01",
                             "2026-11-02"};

struct __attribute__((packed)) Rec {
  uint32_t lat;
  uint16_t tag;
}; // tag: id | 5xx << 11
using Run = std::pair<Rec *, uint32_t>;
using Part =
    std::pair<std::unique_ptr<char[]>, size_t>; // output text, its length

[[noreturn]] void fail(const char *what) {
  std::perror(what);
  std::exit(1);
}
__m256i load(const void *p) {
  return _mm256_loadu_si256(static_cast<const __m256i *>(p));
}
uint64_t load64(const void *p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t spaces(const char *p) {
  return _mm256_movemask_epi8(
      _mm256_cmpeq_epi8(load(p), _mm256_set1_epi8(' ')));
}
// Index of the first space at or after q + 64.
uint32_t far_space(const char *q) {
  uint32_t k = 64, m;
  while (!(m = spaces(q + k)))
    k += 32;
  return k + __builtin_ctz(m);
}
// The value of eight decimal digits (0..9 per byte, most significant first).
uint64_t digits8(uint64_t v) {
  __m128i x = _mm_maddubs_epi16(_mm_cvtsi64_si128(v), _mm_set1_epi16(0x010a));
  x = _mm_madd_epi16(x, _mm_set1_epi32(0x00010064));
  const uint64_t r = _mm_cvtsi128_si64(x);
  return (r & 0xffffffff) * 10000 + (r >> 32);
}

// Lane permutations that compress the lanes of an 8-bit mask to the bottom
// (kLow) or, for the lanes outside the mask, to the top (kHigh) of a vector.
struct Lanes {
  uint8_t low[256][8]{}, high[256][8]{};
  constexpr Lanes() {
    for (int m = 0; m < 256; ++m)
      for (int lane = 0, in = 0, out = 0; lane < 8; ++lane)
        if (m >> lane & 1)
          low[m][in++] = lane;
        else
          high[m][__builtin_popcount(m) + out++] = lane;
  }
};
constexpr Lanes kLanes;

// Copies src[lo, hi) to dst so that the values below pivot come first; returns
// how many there are. Values are int32 (latencies with the sign bit flipped).
uint32_t split(const int32_t *src, int32_t *dst, uint32_t lo, uint32_t hi,
               int32_t pivot) {
  const __m256i bound = _mm256_set1_epi32(pivot);
  uint32_t i = lo, l = lo, r = hi;
  for (; hi - i >= 16; i += 8) {
    const __m256i v = load(src + i);
    const uint32_t m = _mm256_movemask_ps(
        _mm256_castsi256_ps(_mm256_cmpgt_epi32(bound, v)));
    auto lanes = [&](const uint8_t *idx) {
      return _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)idx));
    };
    _mm256_storeu_si256((__m256i *)(dst + l),
                        _mm256_permutevar8x32_epi32(v, lanes(kLanes.low[m])));
    _mm256_storeu_si256((__m256i *)(dst + r - 8),
                        _mm256_permutevar8x32_epi32(v, lanes(kLanes.high[m])));
    l += __builtin_popcount(m);
    r -= 8 - __builtin_popcount(m);
  }
  for (; i < hi; ++i) {
    const int32_t v = src[i];
    dst[l] = dst[r - 1] = v;
    l += v < pivot;
    r -= v >= pivot;
  }
  return l - lo;
}

// Sorts the first n <= 16 values of a (plus 16 readable slots) into out[0, 16)
// with a bitonic network on two vectors; unused lanes sort to the end.
template <int M> __m256i step(__m256i v, __m256i other) {
  return _mm256_blend_epi32(_mm256_min_epi32(v, other),
                            _mm256_max_epi32(v, other), M);
}
__m256i sort8(__m256i v) {
  const __m256i flip = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
  v = step<0xAA>(v, _mm256_shuffle_epi32(v, 0xB1));
  v = step<0xCC>(v, _mm256_shuffle_epi32(v, 0x1B));
  v = step<0xAA>(v, _mm256_shuffle_epi32(v, 0xB1));
  v = step<0xF0>(v, _mm256_permutevar8x32_epi32(v, flip));
  v = step<0xCC>(v, _mm256_shuffle_epi32(v, 0x4E));
  return step<0xAA>(v, _mm256_shuffle_epi32(v, 0xB1));
}
__m256i merge8(__m256i v) { // v is a bitonic sequence
  v = step<0xF0>(v, _mm256_permute2x128_si256(v, v, 1));
  v = step<0xCC>(v, _mm256_shuffle_epi32(v, 0x4E));
  return step<0xAA>(v, _mm256_shuffle_epi32(v, 0xB1));
}
void sort16(const int32_t *a, uint32_t n, int32_t *out) {
  const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7),
                big = _mm256_set1_epi32(INT32_MAX), count = _mm256_set1_epi32(n);
  __m256i x = _mm256_blendv_epi8(big, load(a), _mm256_cmpgt_epi32(count, iota)),
          y = _mm256_blendv_epi8(
              big, load(a + 8),
              _mm256_cmpgt_epi32(count, _mm256_add_epi32(iota, _mm256_set1_epi32(8))));
  x = sort8(x);
  y = _mm256_permutevar8x32_epi32(sort8(y), _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0));
  _mm256_storeu_si256((__m256i *)out, merge8(_mm256_min_epi32(x, y)));
  _mm256_storeu_si256((__m256i *)(out + 8), merge8(_mm256_max_epi32(x, y)));
}

// Stores the values at the sorted positions k[0..nk) of a[lo, hi) into out;
// the positions are ascending and inside the range. a and b swap roles at each
// level (b is scratch of the same size).
void multiselect(int32_t *a, int32_t *b, uint32_t lo, uint32_t hi,
                 const uint32_t *k, int nk, int32_t *out, int depth = 0) {
  if (hi - lo <= 16) {
    alignas(32) int32_t sorted[16];
    sort16(a + lo, hi - lo, sorted);
    for (int i = 0; i < nk; ++i)
      out[i] = sorted[k[i] - lo];
    return;
  }
  if (depth > 40) {
    std::sort(a + lo, a + hi);
    for (int i = 0; i < nk; ++i)
      out[i] = a[k[i]];
    return;
  }
  const int32_t x = a[lo], y = a[lo + (hi - lo) / 2], z = a[hi - 1];
  int32_t pivot = std::max(std::min(x, y), std::min(std::max(x, y), z));
  uint32_t cut = lo + split(a, b, lo, hi, pivot);
  if (cut == lo) { // pivot is the minimum: peel off its duplicates
    if (pivot == INT32_MAX) {
      std::fill(out, out + nk, pivot);
      return;
    }
    cut = lo + split(a, b, lo, hi, pivot + 1);
    for (int i = 0; i < nk && k[i] < cut; ++i)
      out[i] = pivot;
    if (cut == hi)
      return;
    int done = std::lower_bound(k, k + nk, cut) - k;
    if (done < nk)
      multiselect(b, a, cut, hi, k + done, nk - done, out + done, depth + 1);
    return;
  }
  const int left = std::lower_bound(k, k + nk, cut) - k;
  if (left)
    multiselect(b, a, lo, cut, k, left, out, depth + 1);
  if (left < nk)
    multiselect(b, a, cut, hi, k + left, nk - left, out + left, depth + 1);
}

// Dictionary inserts are tiny and bursty at startup, so waiters spin instead of
// sleeping.
struct SpinLock {
  std::atomic_flag held = ATOMIC_FLAG_INIT;
  void lock() {
    for (int i = 0; held.test_and_set(std::memory_order_acquire); ++i)
      if (i < 64)
        _mm_pause();
      else
        sched_yield();
  }
  void unlock() { held.clear(std::memory_order_release); }
};

// Exact endpoint dictionary. The global part is an append-only log (key image,
// length, fingerprint) behind a lock whose size is published with a release
// store; entries below it never change. Each worker mirrors the log in a private
// bucket table, so the hot lookups are plain SIMD reads without sharing. Keys
// are stored after a space and zero-padded, so n + 1 equal bytes prove equality.
struct Names {
  alignas(64) char img[kIds][128]{};
  uint32_t len[kIds]{}, hash[kIds]{};
  std::atomic<uint32_t> size{0};
  std::vector<uint16_t> sorted; // ids in byte order of their names (under mu)
  SpinLock mu;

  bool same(uint32_t id, const char *ep, uint32_t n) const {
    auto eq = [&](int i) {
      return uint64_t(uint32_t(_mm256_movemask_epi8(
                 _mm256_cmpeq_epi8(load(ep + i), load(img[id] + i)))))
             << i;
    };
    return !_bzhi_u64(~(eq(0) | eq(32)), n + 1) &&
           (n < 64 || !std::memcmp(img[id] + 64, ep + 64, n - 63));
  }
};

struct Local { // one worker's private open-addressing view of the dictionary
  static constexpr uint32_t kSlots = 1 << 14;
  uint16_t slot[kSlots]{}; // id + 1, or 0 when free
  uint32_t synced = 0;

  void put(uint32_t h, uint32_t id) {
    uint32_t i = h >> 18;
    while (slot[i])
      i = (i + 1) % kSlots;
    slot[i] = id + 1;
  }
  int lookup(const Names &names, const char *ep, uint32_t n, uint32_t h) const {
    for (uint32_t i = h >> 18; slot[i]; i = (i + 1) % kSlots)
      if (names.same(slot[i] - 1, ep, n))
        return slot[i] - 1;
    return -1;
  }
  // Copies the log entries published since the last visit.
  void sync(const Names &names) {
    for (const uint32_t end = names.size.load(std::memory_order_acquire);
         synced < end; ++synced)
      put(names.hash[synced], synced);
  }
  uint32_t add(Names &names, const char *ep, uint32_t n, uint32_t h) {
    sync(names);
    if (const int id = lookup(names, ep, n, h); id >= 0)
      return id;
    std::lock_guard<SpinLock> lock(names.mu);
    sync(names); // another worker may have published it meanwhile
    if (const int id = lookup(names, ep, n, h); id >= 0)
      return id;
    const uint32_t id = names.size.load(std::memory_order_relaxed);
    if (id >= kIds)
      fail("too many endpoints");
    std::memcpy(names.img[id], ep, n);
    names.img[id][n] = ' ';
    names.len[id] = n;
    names.hash[id] = h;
    names.sorted.insert(
        std::lower_bound(names.sorted.begin(), names.sorted.end(), id,
                         [&](uint32_t x, uint32_t y) {
                           return std::memcmp(names.img[x], names.img[y], 128) <
                                  0;
                         }),
        id);
    names.size.store(id + 1, std::memory_order_release);
    put(h, id);
    synced = id + 1;
    return id;
  }
  __attribute__((always_inline)) uint32_t find(Names &names, const char *ep,
                                               uint32_t n) {
    const uint64_t keep = n < 8 ? n * 8 : 64;
    const uint64_t x = _bzhi_u64(load64(ep), keep),
                   y = _bzhi_u64(load64(ep + std::max(n, 8u) - 8), keep);
    const uint32_t h = _mm_crc32_u64(_mm_crc32_u64(n, x), y);
    const int id = lookup(names, ep, n, h);
    return id >= 0 ? id : add(names, ep, n, h);
  }
};

// Where a stream appends: the hour of its previous line is remembered.
struct Stream {
  const char *pos, *stop;
  uint64_t date = 0; // bytes 5..12 of the previous line
  Rec **at = nullptr;
};

// The eight decimal digits of v < 10^8 as bytes 0..9, most significant first.
uint64_t digits_of(uint32_t v) {
  uint64_t x = v / 10000 | uint64_t{v % 10000} << 32;
  uint64_t q = (x * 5243 >> 19) & 0x0000007f0000007f;
  x = q | (x - q * 100) << 16;
  q = (x * 103 >> 10) & 0x000f000f000f000f;
  return q | (x - q * 10) << 8;
}
// Appends a space and v; 16 bytes of slack must follow.
__attribute__((always_inline)) inline char *put(char *out, uint32_t v) {
  *out++ = ' ';
  if (v >= 100000000)
    return std::to_chars(out, out + 10, v).ptr;
  const uint64_t d = digits_of(v);
  const uint32_t skip = __builtin_ctzll(d | uint64_t{1} << 56) / 8;
  const uint64_t text = (d + 0x3030303030303030) >> 8 * skip;
  std::memcpy(out, &text, 8);
  return out + 8 - skip;
}

struct Job {
  const char *base;
  size_t size;
  uint32_t lag, nblocks, threads;
  int fd;
  Names names;
  std::mutex mu, out_mu;
  std::condition_variable cv;
  std::vector<Run> runs[kHours]; // per hour, from the registered blocks
  std::vector<int> top; // per block: its last hour, or -1 until registered
  uint32_t next_block = 0, watermark = 0, prefix_max = 0, registered = 0,
           next_hour = 0;
  bool bad = false; // a line arrived after its hour was finalized
  Part parts[kHours];
  off_t end[kHours + 1]{}; // output offset after each hour
  uint32_t written = 0;    // hours written so far
  bool writing = false;
  std::atomic<size_t> zapped{0}; // 4 MiB steps of the mapping released
  std::vector<Rec *> runbufs; // free buffers, guarded by mu
  std::vector<std::vector<int32_t>> latbufs;
  std::vector<std::unique_ptr<char[]>> outbufs;

  Job(const char *b, size_t n, uint32_t l, int f, uint32_t t)
      : base(b), size(n), lag(l), nblocks((n + kBlock - 1) / kBlock),
        threads(t), fd(f), top(nblocks, -1) {}
  ~Job() {
    for (Rec *r : runbufs)
      std::free(r);
  }

  template <class T> T take(std::vector<T> &pool) {
    std::lock_guard<std::mutex> lock(mu);
    if (pool.empty())
      return T();
    T t = std::move(pool.back());
    pool.pop_back();
    return t;
  }
  static const char *next_line(const char *p) {
    while (p[-1] != '\n')
      ++p;
    return p;
  }
  const char *align(uint32_t block) const {
    return block ? next_line(base +
                             std::min<size_t>(size_t{block} * kBlock, size))
                 : base;
  }

  // Parses one line of the stream; 256 readable bytes must follow it.
  __attribute__((always_inline)) void line(Local &local, Stream &st,
                                           Rec *(&wp)[kHours],
                                           Rec *(&bp)[kHours]) {
    const char *p = st.pos;
    if (const uint64_t date = load64(p + 5); date != st.date) {
      st.date = date;
      st.at = wp + (p[9] + (p[6] & 1)) * 24 + p[11] * 10 + p[12] - 1698;
      if (!*st.at) {
        Rec *r = take(runbufs);
        *st.at = bp[st.at - wp] =
            r ? r
              : static_cast<Rec *>(
                    std::malloc(kRunCap * sizeof(Rec) + sizeof(uint64_t)));
      }
    }
    _mm_prefetch(p + 1280, _MM_HINT_T0);
    _mm_prefetch(p + 1344, _MM_HINT_T0);
    const char *q = p + 28;
    uint64_t m = spaces(q) | uint64_t{spaces(q + 32)} << 32;
    const uint32_t k1 = __builtin_ctzll(m);
    m &= m - 1;
    const uint32_t k2 = m ? __builtin_ctzll(m) : far_space(q);
    const uint32_t id = local.find(names, q + k1 + 1, k2 - k1 - 1);
    const char *s = q + k2 + 1;
    const uint32_t error = *s == '5';
    s += 4;
    uint64_t v = load64(s) ^ 0x3030303030303030, lat;
    const uint64_t other = ((v + 0x7676767676767676) | v) & 0x8080808080808080;
    if (other) {
      const uint32_t digits = __builtin_ctzll(other) >> 3;
      lat = digits8(v << (64 - 8 * digits));
      s += digits;
    } else {
      for (lat = digits8(v), s += 8; *s != '\n'; ++s)
        lat = lat * 10 + (*s - '0');
    }
    const uint64_t rec = lat | uint64_t{id | error << 11} << 32;
    std::memcpy((*st.at)++, &rec, sizeof(Rec));
    st.pos = s + 1;
  }

  void parse(Local &local, uint32_t block) {
    Rec *wp[kHours]{}, *bp[kHours]{};
    Stream st[kStreams]; // interleaved streams overlap their latencies
    const char *p = align(block), *end = align(block + 1);
    for (uint32_t i = 0; i < kStreams; ++i) {
      st[i].pos = i ? next_line(p + (end - p) * i / kStreams) : p;
      st[i].stop = i + 1 < kStreams ? nullptr : end;
    }
    for (uint32_t i = 0; i + 1 < kStreams; ++i)
      st[i].stop = st[i + 1].pos;
    // Every line has at most kMaxLine bytes, so this many lines are in range.
    for (size_t rounds = 1; rounds;) {
      size_t room = SIZE_MAX;
      for (const Stream &x : st)
        room = std::min<size_t>(room, x.stop - x.pos);
      for (rounds = room / kMaxLine; rounds--;) {
#pragma GCC unroll 4
        for (Stream &x : st)
          line(local, x, wp, bp);
      }
      rounds = room / kMaxLine;
    }
    for (Stream &x : st)
      while (x.pos < x.stop)
        line(local, x, wp, bp);

    std::lock_guard<std::mutex> lock(mu);
    int last = 0;
    for (uint32_t h = 0; h < kHours; ++h) {
      if (!bp[h])
        continue;
      last = h;
      if (h < next_hour) {
        bad = true;
        runbufs.push_back(bp[h]);
      } else
        runs[h].push_back({bp[h], static_cast<uint32_t>(wp[h] - bp[h])});
    }
    top[block] = last;
    for (++registered; watermark < nblocks && top[watermark] >= 0; ++watermark)
      prefix_max = std::max<int>(prefix_max, top[watermark]);
    cv.notify_all();
  }

  void finalize(uint32_t hour) {
    std::vector<Run> mine;
    std::vector<uint16_t> order;
    {
      std::lock_guard<std::mutex> lock(mu);
      mine.swap(runs[hour]);
      std::lock_guard<SpinLock> names_lock(names.mu);
      order = names.sorted;
    }
    uint32_t count[2 * kIds] = {}, pos[kIds],
                       total = 0; // by tag: ids, then their 5xx
    for (const auto &[rec, n] : mine)
      for (uint32_t i = 0; i < n; ++i)
        ++count[rec[i].tag];
    uint16_t live[kIds]; // the endpoints seen in this hour, in output order
    uint32_t seen = 0;
    for (const uint16_t id : order) {
      const uint32_t n = count[id] + count[id + kIds];
      pos[id] = total;
      total += n;
      live[seen] = id;
      seen += n != 0;
    }
    std::vector<int32_t> lat = take(latbufs);
    lat.reserve(1 << 17);
    lat.resize(total + 16);
    for (const auto &[rec, n] : mine)
      for (uint32_t i = 0; i < n; ++i)
        lat[pos[rec[i].tag % kIds]++] = rec[i].lat ^ 0x80000000;

    static thread_local std::vector<int32_t> scratch;
    std::unique_ptr<char[]> text = take(outbufs);
    if (!text)
      text.reset(new char[kPartCap + 256]);
    char *out = text.get(), label[22];
    std::snprintf(label, sizeof label, "%sT%02u:00:00Z ",
                  kDays[(hour + 18) / 24], (hour + 18) % 24);
    for (uint32_t i = 0; i < seen; ++i) {
      const uint16_t id = live[i];
      const uint32_t errors = count[id + kIds], n = count[id] + errors;
      int32_t *const a = lat.data() + pos[id] - n;
      const uint32_t ranks[3] = {(50 * n + 99) / 100 - 1, (95 * n + 99) / 100 - 1,
                                 (99 * n + 99) / 100 - 1};
      int32_t value[3];
      if (scratch.size() < n + 16)
        scratch.resize(n + 16);
      multiselect(a, scratch.data(), 0, n, ranks, 3, value);
      std::memcpy(out, label, 21);
      for (uint32_t i = 0; i < 128; i += 32)
        _mm256_storeu_si256((__m256i *)(out + 21 + i), load(names.img[id] + i));
      out += 21 + names.len[id];
      out = put(put(put(put(put(out, n), uint32_t(value[0]) ^ 0x80000000),
                        uint32_t(value[1]) ^ 0x80000000),
                    uint32_t(value[2]) ^ 0x80000000),
                errors);
      out = errors ? put(out, uint64_t{errors} * 1000000 / n) : put(out, 0);
      *out++ = '\n';
    }
    const size_t length = out - text.get();
    {
      std::lock_guard<std::mutex> lock(mu);
      latbufs.push_back(std::move(lat));
      for (const auto &[rec, n] : mine)
        runbufs.push_back(rec);
    }
    emit(hour, {std::move(text), length});
  }

  // Whoever completes an hour writes all hours that are ready in order (a
  // thread that finds a writer active just leaves its part), starts their
  // writeback and drops older cached output pages (best effort).
  void emit(uint32_t hour, Part ready) {
    std::unique_lock<std::mutex> lock(out_mu);
    parts[hour] = std::move(ready);
    if (writing)
      return;
    for (writing = true; written < kHours && parts[written].first; ++written) {
      Part part = std::move(parts[written]);
      lock.unlock();
      for (size_t done = 0; done < part.second;) {
        const ssize_t w =
            write(fd, part.first.get() + done, part.second - done);
        if (w <= 0)
          fail("write summary");
        done += w;
      }
      end[written + 1] = end[written] + part.second;
      if (written + 1 == kHours && ftruncate(fd, end[kHours]))
        fail("size summary");
      sync_file_range(fd, end[written], part.second, SYNC_FILE_RANGE_WRITE);
      if (written >= kKeep)
        posix_fadvise(fd, 0, end[written - kKeep + 1], POSIX_FADV_DONTNEED);
      {
        std::lock_guard<std::mutex> pool_lock(mu);
        outbufs.push_back(std::move(part.first));
      }
      lock.lock();
    }
    writing = false;
  }

  bool claimable() const {
    return next_hour < kHours &&
           (registered == nblocks || next_hour + lag <= prefix_max);
  }

  // Finalizes hours once they are safe and otherwise parses blocks, but never
  // more than two per worker past the oldest unfinished one.
  void work(uint32_t index) {
    const auto local = std::make_unique<Local>();
    std::unique_lock<std::mutex> lock(mu);
    for (;;) {
      if (claimable()) {
        const uint32_t hour = next_hour++;
        cv.notify_all();
        lock.unlock();
        finalize(hour);
        lock.lock();
      } else if (next_block < nblocks && next_block < watermark + 2 * threads) {
        const uint32_t block = next_block++;
        lock.unlock();
        parse(*local, block);
        lock.lock();
      } else if (next_hour >= kHours) {
        lock.unlock(); // drop the file mapping in shared 4 MiB steps so that
                       // exit is cheaper
        constexpr size_t kStep = 4 << 20;
        for (size_t at; (at = zapped++ * kStep) < size;)
          madvise(const_cast<char *>(base) + at, std::min(kStep, size - at),
                  MADV_DONTNEED);
        return;
      } else {
        cv.wait(lock);
      }
    }
  }
  std::thread pool[16]; // worker i is started by worker (i - 1) / 2
  std::atomic<uint32_t> started{1};
  // Workers start their children before working, so that a dozen threads are up
  // after a few thread creations instead of eleven.
  void start(uint32_t i) {
    for (uint32_t c = 2 * i + 1; c <= 2 * i + 2 && c < threads; ++c) {
      pool[c] = std::thread([this, c] { start(c); });
      started.fetch_add(1, std::memory_order_release);
    }
    work(i);
  }
  bool run() {
    start(0);
    while (started.load(std::memory_order_acquire) < threads)
      std::this_thread::yield();
    for (uint32_t i = 1; i < threads; ++i)
      pool[i].join();
    return !bad;
  }
};
} // namespace

int main(int argc, char **argv) {
  // Under transparent huge pages ("always" on Debian) each thread's malloc
  // arena and stack would be charged whole 2 MiB pages; small pages keep M low.
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  const char *const dir = argc > 2 ? argv[1] : "/data",
                    *const out = argc > 2 ? argv[2] : "/out";
  const int in = open(
      (std::string(dir) + "/requests.log").c_str(), O_RDONLY);
  const size_t bytes = in < 0 ? 0 : lseek(in, 0, SEEK_END);
  // Zero pages follow the file so that the last lines can be scanned past its
  // end.
  char *map = static_cast<char *>(mmap(nullptr, bytes + 8192, PROT_READ,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (in < 0 || map == MAP_FAILED ||
      (bytes && mmap(map, bytes, PROT_READ, MAP_PRIVATE | MAP_FIXED, in, 0) ==
                    MAP_FAILED))
    fail("map requests");
  const int fd =
      open((std::string(out) + "/summary.txt").c_str(),
           O_WRONLY | O_CREAT, 0644); // an old file is overwritten in place
  if (fd < 0)
    fail("open summary");
  cpu_set_t cpus{};
  sched_getaffinity(0, sizeof cpus, &cpus);
  const uint32_t threads = std::min(16, CPU_COUNT(&cpus));
  if (!(new Job(map, bytes, kLag, fd, threads))->run()) {
    if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET))
      fail("rewind summary");
    (new Job(map, bytes, kNever, fd, threads))->run();
  }
  _exit(0); // the kernel reclaims everything faster than free() does
}