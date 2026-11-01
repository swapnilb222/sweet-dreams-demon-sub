// Speed Demon 2026, Track A (Ingest).
//
// One parallel region, three phases separated by barriers:
// 1. Threads claim 1 MiB chunks of the mmap'd log, parse every line once and
//    append a 4-byte record (20-bit latency, error bit, 11-bit endpoint id
//    from one shared table) to a per-thread bucket for its hour.
//    Barrier: endpoint ids are ranked in byte order, the output order.
// 2. Hours, largest first: merge the hour's buckets, select the percentiles
//    and keep per-group stats plus the exact size of the hour's output.
//    Barrier: hour sizes become file offsets; the output file is mapped.
// 3. Hours are formatted straight into the mapped output in parallel.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int HOURS = 72, MAX_EP = 2048, SLOTS = 16384, MAX_THREADS = 12;
constexpr size_t CHUNK = 1 << 20, PAGE = 4096;
// Latencies of 2^20 - 1 and up (about 1%) do not fit a record; they are kept
// whole in a side list instead.
constexpr uint32_t BIG = (1u << 20) - 1;
// Buckets grow in 4 KiB blocks from one pool per thread, so page tables stay
// compact and each bucket wastes at most one partly filled block.
constexpr size_t BLOCK = 512;

const char *data;
size_t size;
std::atomic<size_t> next_chunk;
std::atomic<int> next_hour;

uint64_t load8(const char *p) {
  uint64_t w;
  std::memcpy(&w, p, 8);
  return w;
}

unsigned hour_of(const char *p) {
  unsigned day = (p[8] - '0') * 10 + (p[9] - '0');
  day = p[6] == '0' ? day - 30 : day + 1; // Oct 30, 31 -> 0, 1; Nov 1, 2 -> 2, 3
  return day * 24 + (p[11] - '0') * 10 + (p[12] - '0') - 18;
}

unsigned eq_mask(const char *p, char c) {
  return _mm256_movemask_epi8(_mm256_cmpeq_epi8(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p)),
      _mm256_set1_epi8(c)));
}

const char *find(const char *p, char c) {
  unsigned m;
  while (!(m = eq_mask(p, c)))
    p += 32;
  return p + __builtin_ctz(m);
}

uint32_t neq_mask(const char *a, const char *b) {
  return ~uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a)),
      _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b)))));
}

// Compares 64 bytes at a time, masking the tail with bzhi: no length branch
// for the 94% of traffic whose endpoint fits.
bool same(const char *a, const char *b, uint32_t len) {
  for (;; a += 64, b += 64, len -= 64) {
    uint64_t diff = neq_mask(a, b) | uint64_t(neq_mask(a + 32, b + 32)) << 32;
    if (len <= 64)
      return !_bzhi_u64(diff, len);
    if (diff)
      return false;
  }
}

// Decimal digits of v: bit length * log10(2) estimates it, one table compare
// corrects it.
unsigned digits(uint32_t v) {
  static const uint32_t pow10[] = {1,         10,        100,     1000,
                                   10000,     100000,    1000000, 10000000,
                                   100000000, 1000000000};
  unsigned t = (32 - __builtin_clz(v | 1)) * 1233 >> 12;
  return t + ((v | 1) >= pow10[t]);
}

// Writes v two digits at a time, right to left, from a pair table.
char *put(char *o, uint32_t v) {
  static const char pairs[] = "0001020304050607080910111213141516171819"
                              "2021222324252627282930313233343536373839"
                              "4041424344454647484950515253545556575859"
                              "6061626364656667686970717273747576777879"
                              "8081828384858687888990919293949596979899";
  char *end = o + digits(v), *q = end;
  for (; v >= 100; v /= 100)
    std::memcpy(q -= 2, pairs + v % 100 * 2, 2);
  if (v >= 10)
    std::memcpy(q - 2, pairs + v * 2, 2);
  else
    q[-1] = char('0' + v);
  return end;
}

// Latency digits start at s and end at the newline (at most 10). Branch-free:
// the newline position comes from one compare, the digits are right-aligned
// and combined pairwise with multiply-adds. Also returns the next line start.
uint32_t parse_latency(const char *s, const char *&next) {
  auto load = [](const char *p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
  };
  int len = __builtin_ctz(
      _mm_movemask_epi8(_mm_cmpeq_epi8(load(s), _mm_set1_epi8('\n'))));
  next = s + len + 1;
  __m128i v = _mm_subs_epu8(load(s + len - 16), _mm_set1_epi8('0'));
  __m128i lane =
      _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
  v = _mm_maddubs_epi16(v, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1,
                                         10, 1, 10, 1, 10, 1));
  v = _mm_madd_epi16(v, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
  return uint32_t((uint64_t(_mm_extract_epi32(v, 1)) * 10000 +
                   uint32_t(_mm_extract_epi32(v, 2))) *
                      10000 +
                  uint32_t(_mm_extract_epi32(v, 3)));
}

// Small ranges: an element holds rank r when # less <= r and # greater <=
// n - 1 - r.
// Eight candidates sit in one register while every element streams past, so
// counting has no data-dependent branches. Lanes past n read neighbouring
// values (scratch has slack) and are ignored.
void select_small(const uint32_t *v, uint32_t n, const uint32_t *rank, int nr,
                  uint32_t *out) {
  const __m256i flip = _mm256_set1_epi32(INT32_MIN); // unsigned via signed
  for (uint32_t i = 0; i < n; i += 8) {
    __m256i x = _mm256_xor_si256(
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + i)), flip);
    __m256i less = _mm256_setzero_si256(), more = less;
    for (uint32_t j = 0; j < n; ++j) {
      __m256i y = _mm256_set1_epi32(int(v[j] ^ 0x80000000u));
      less = _mm256_sub_epi32(less, _mm256_cmpgt_epi32(x, y));
      more = _mm256_sub_epi32(more, _mm256_cmpgt_epi32(y, x));
    }
    __m256i valid = _mm256_cmpgt_epi32(
        _mm256_set1_epi32(int(n - i)), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    for (int k = 0; k < nr; ++k) {
      __m256i miss = _mm256_or_si256(
          _mm256_cmpgt_epi32(less, _mm256_set1_epi32(int(rank[k]))),
          _mm256_cmpgt_epi32(more, _mm256_set1_epi32(int(n - 1 - rank[k]))));
      unsigned hit = _mm256_movemask_ps(
          _mm256_castsi256_ps(_mm256_andnot_si256(miss, valid)));
      out[k] = hit ? v[i + __builtin_ctz(hit)] : out[k];
    }
  }
}

// Finds the values of ranks rank[0..nr) (ascending) in v[lo, hi): branch-free
// partitions narrow towards the ranks, then counting finishes.
void select(uint32_t *v, uint32_t lo, uint32_t hi, const uint32_t *rank,
            int nr, uint32_t *out) {
  while (nr) {
    if (hi - lo <= 16) {
      uint32_t local[3];
      for (int k = 0; k < nr; ++k)
        local[k] = rank[k] - lo;
      select_small(v + lo, hi - lo, local, nr, out);
      return;
    }
    uint32_t a = v[lo], b = v[(lo + hi) / 2], c = v[hi - 1];
    uint32_t pivot = std::max(std::min(a, b), std::min(std::max(a, b), c));
    uint32_t less = lo, upto = lo;
    for (uint32_t j = lo; j < hi; ++j) {
      uint32_t x = v[j];
      v[j] = v[less];
      v[less] = x;
      less += x < pivot;
    }
    // Splitting off the pivot's copies is only needed for progress when
    // nothing was smaller; otherwise [less, hi) already shrank.
    if (less == lo)
      for (uint32_t j = lo; j < hi; ++j) {
        uint32_t x = v[j];
        v[j] = v[upto];
        v[upto] = x;
        upto += x == pivot;
      }
    else
      upto = less;
    int l = 0, r;
    while (l < nr && rank[l] < less)
      ++l;
    for (r = l; r < nr && rank[r] < upto; ++r)
      ;
    select(v, lo, less, rank, l, out);
    for (int k = l; k < r; ++k)
      out[k] = pivot;
    rank += r, out += r, nr -= r, lo = upto;
  }
}

// One open-addressing endpoint table shared by every thread. A slot's key
// packs hash, length and id, so a probe is one load and compare. A new name
// (about 1842 per run) claims its slot as BUSY, is copied to its fixed-stride
// arena row, then published with a release store; readers that meet BUSY
// spin on that slot.
struct Dictionary {
  static constexpr uint64_t BUSY = 1;
  std::atomic<uint64_t> slots[SLOTS];
  char names[MAX_EP + 1][128]; // the spare row keeps 64-byte compares in bounds
  uint8_t lens[MAX_EP];
  std::atomic<unsigned> size;

  std::string_view name(unsigned id) const { return {names[id], lens[id]}; }

  uint32_t lookup(const char *s, uint32_t len) {
    uint64_t head = load8(s), tail = 0;
    if (len < 8)
      head &= (1ull << (8 * len)) - 1;
    else
      tail = load8(s + len - 8);
    uint32_t hash = _mm_crc32_u64(_mm_crc32_u64(len, head), tail);
    uint64_t want = uint64_t(hash) << 32 | len << 16;
    uint64_t key = slots[hash % SLOTS].load(std::memory_order_acquire);
    if ((key & ~0xFFFFull) == want && same(s, names[key & 0xFFFF], len))
      [[likely]] return key & 0xFFFF;
    return probe(s, len, hash, want);
  }

  // Everything but a first-slot hit, kept out of line so the parse loop stays
  // small and keeps its vector constants in registers.
  [[gnu::noinline]] uint32_t probe(const char *s, uint32_t len, uint32_t hash,
                                   uint64_t want) {
    for (uint32_t i = hash;; ++i) {
      std::atomic<uint64_t> &slot = slots[i % SLOTS];
      uint64_t key = slot.load(std::memory_order_acquire);
      if (!key && slot.compare_exchange_strong(key, BUSY)) [[unlikely]] {
        unsigned id = size++;
        std::memcpy(names[id], s, len);
        lens[id] = uint8_t(len);
        slot.store(want | id, std::memory_order_release);
        return id;
      }
      if (key <= BUSY) [[unlikely]] { // being inserted or claimed: look again
        --i;
        continue;
      }
      if ((key & ~0xFFFFull) == want && same(s, names[key & 0xFFFF], len))
        return key & 0xFFFF;
    }
  }
};

Dictionary dict;

struct Worker {
  void *base; // the pool's mapping; pool advances through it
  uint32_t *pool, *cur[HOURS], *limit[HOURS];
  std::vector<uint32_t *> blocks[HOURS];
  std::vector<uint64_t> big[HOURS]; // (latency << 12) | (error << 11) | id
  uint32_t count[MAX_EP], errors[MAX_EP], offset[MAX_EP];
  uint32_t *scratch;

  [[gnu::noinline]] void keep_big(unsigned h, uint64_t r) {
    big[h].push_back(r);
  }
  [[gnu::noinline]] uint32_t *refill(unsigned h, uint32_t *&c) {
    blocks[h].push_back(c = pool);
    return pool += BLOCK;
  }

  template <class F> void each_record(int h, F f) {
    for (uint32_t *b : blocks[h])
      for (uint32_t *r = b, *e = b == blocks[h].back() ? cur[h] : b + BLOCK;
           r < e; ++r)
        f(*r);
    for (uint64_t r : big[h])
      f(r);
  }

  void parse(const char *p, const char *end) {
    uint64_t key = 0;
    unsigned h = 0;
    uint32_t *c = cur[0], *lim = limit[0]; // the current hour's bucket cursor
    while (p < end) {
      if (load8(p + 5) != key) [[unlikely]] { // bytes 5..12, "MM-DDTHH": hour
        cur[h] = c, limit[h] = lim;
        key = load8(p + 5), h = hour_of(p);
        c = cur[h], lim = limit[h];
      }
      // Methods are 3 to 7 bytes, so the endpoint starts at p + 32..36.
      uint64_t m = eq_mask(p + 31, ' ') | uint64_t(eq_mask(p + 63, ' ')) << 32;
      const char *s = p + 32 + __builtin_ctzll(m);
      m &= m - 1;
      const char *e = m ? p + 31 + __builtin_ctzll(m) : find(p + 95, ' ');
      uint32_t id = dict.lookup(s, uint32_t(e - s)) | (e[1] == '5') << 11;
      uint32_t v = parse_latency(e + 5, p);
      if (v >= BIG) [[unlikely]] {
        keep_big(h, uint64_t(v) << 12 | id);
        continue;
      }
      if (c == lim) [[unlikely]]
        lim = refill(h, c);
      *c++ = v << 12 | id;
    }
    cur[h] = c, limit[h] = lim;
  }
};

Worker workers[MAX_THREADS]; // zero pages, faulted in by their own threads
unsigned threads, neps;
std::vector<uint16_t> sorted_ids; // output order -> endpoint id
uint16_t rank_of[MAX_EP];         // endpoint id -> output order
int by_size[HOURS];               // hours, largest first
struct Stat {
  uint32_t n, errors, p[3];
};
Stat stats[HOURS * MAX_EP];        // [hour][output order]; zero pages
size_t hour_bytes[HOURS + 1];      // becomes each hour's file offset
char *out;

void merge() {
  neps = dict.size;
  sorted_ids.resize(neps);
  for (unsigned g = 0; g < neps; ++g)
    sorted_ids[g] = uint16_t(g);
  std::sort(sorted_ids.begin(), sorted_ids.end(), [](uint16_t a, uint16_t b) {
    return dict.name(a) < dict.name(b);
  });
  for (unsigned k = 0; k < neps; ++k)
    rank_of[sorted_ids[k]] = uint16_t(k);
  // Hour sizes, rounded up to whole blocks, schedule phase 2.
  size_t lines[HOURS] = {};
  for (int h = 0; h < HOURS; ++h) {
    by_size[h] = h;
    for (unsigned t = 0; t < threads; ++t)
      lines[h] += workers[t].blocks[h].size() * BLOCK + workers[t].big[h].size();
  }
  std::sort(by_size, by_size + HOURS,
            [&](int a, int b) { return lines[a] > lines[b]; });
}

void aggregate(Worker &me, int h) {
  uint32_t *cnt = me.count, *err = me.errors, *off = me.offset, *v = me.scratch;
  std::fill_n(cnt, neps, 0);
  std::fill_n(err, neps, 0);
  for (unsigned t = 0; t < threads; ++t)
    workers[t].each_record(h, [&](uint64_t r) {
      unsigned g = rank_of[r & 2047];
      ++cnt[g];
      err[g] += r >> 11 & 1;
    });
  uint32_t acc = 0;
  for (unsigned g = 0; g < neps; ++g)
    off[g] = acc, acc += cnt[g];
  for (unsigned t = 0; t < threads; ++t)
    workers[t].each_record(h, [&](uint64_t r) {
      v[off[rank_of[r & 2047]]++] = uint32_t(r >> 12);
    });
  // Line: 21-byte hour stamp with its space, name, six numbers each after a
  // space, newline.
  size_t bytes = 0;
  for (unsigned g = 0; g < neps; ++g) {
    uint32_t n = cnt[g];
    if (!n)
      continue;
    uint32_t *vg = v + off[g] - n; // off[g] now marks the end of group g
    uint32_t rank[3] = {(50 * n + 99) / 100 - 1, (95 * n + 99) / 100 - 1,
                        (99 * n + 99) / 100 - 1};
    Stat &s = stats[h * MAX_EP + g];
    s = {n, err[g], {}};
    select(vg, 0, n, rank, 3, s.p);
    bytes += 28 + dict.lens[sorted_ids[g]] + digits(n) +
             digits(s.p[0]) + digits(s.p[1]) + digits(s.p[2]) +
             digits(s.errors) +
             digits(uint32_t(uint64_t(s.errors) * 1000000 / n));
  }
  hour_bytes[h] = bytes;
}

void format(int h) {
  unsigned clock = h + 18, day = clock / 24; // hour 0 is Oct 30, 18:00
  char stamp[22];
  std::snprintf(stamp, sizeof stamp, "2026-%s-%02uT%02u:00:00Z ",
                day < 2 ? "10" : "11", day < 2 ? 30 + day : day - 1, clock % 24);
  char *o = out + hour_bytes[h];
  for (unsigned g = 0; g < neps; ++g) {
    const Stat &s = stats[h * MAX_EP + g];
    if (!s.n)
      continue;
    std::string_view name = dict.name(sorted_ids[g]);
    std::memcpy(o, stamp, 21);
    std::memcpy(o + 21, name.data(), name.size());
    o += 21 + name.size();
    *o++ = ' ';
    o = put(o, s.n);
    for (uint32_t p : s.p) {
      *o++ = ' ';
      o = put(o, p);
    }
    *o++ = ' ';
    o = put(o, s.errors);
    *o++ = ' ';
    o = put(o, uint32_t(uint64_t(s.errors) * 1000000 / s.n));
    *o++ = '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  // With THP on "always", every thread's malloc arena, stack and the stats
  // table would be charged a whole 2 MiB page; small pages keep M honest.
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  std::string_view in_dir = argc > 2 ? argv[1] : "/data";
  std::string_view out_dir = argc > 2 ? argv[2] : "/out";
  int fd = open((std::string(in_dir) + "/requests.log").c_str(), O_RDONLY);
  int out_fd = open((std::string(out_dir) + "/summary.txt").c_str(),
                    O_RDWR | O_CREAT | O_TRUNC, 0644);
  struct stat st;
  if (fd < 0 || out_fd < 0 || fstat(fd, &st))
    return 1;
  size = size_t(st.st_size);
  if (!size)
    return 0;

  // Keep a spare page past EOF so vector loads never fault.
  size_t span = (size + 2 * PAGE) & ~(PAGE - 1);
  void *base = mmap(nullptr, span, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  data = static_cast<const char *>(
      mmap(base, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0));
  if (base == MAP_FAILED || data == MAP_FAILED)
    return 1;

  threads = std::clamp(std::thread::hardware_concurrency(), 1u,
                       unsigned(MAX_THREADS));
  threads = unsigned(std::min<size_t>(threads, (size + CHUNK - 1) / CHUNK));
  // A thread can at most parse every line of the file (40 bytes minimum each).
  // Reserve that much address space for its records, and as much again for
  // its phase-2 scratch; only touched pages are charged.
  size_t lines = size / 40 + 8, cap = (2 * lines + (HOURS + 1) * BLOCK) * 4;
  for (unsigned t = 0; t < threads; ++t) {
    void *p = mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
      return 1;
    madvise(p, cap, MADV_NOHUGEPAGE); // a huge page per block would be charged
    workers[t].pool = static_cast<uint32_t *>(workers[t].base = p);
    workers[t].scratch = workers[t].pool + lines + (HOURS + 1) * BLOCK;
  }

  bool failed = false;
  std::barrier merged(threads, merge);
  std::barrier sized(threads, [&]() noexcept {
    size_t total = 0;
    for (int h = 0; h <= HOURS; ++h)
      std::swap(total, hour_bytes[h]), total += hour_bytes[h];
    failed = ftruncate(out_fd, off_t(hour_bytes[HOURS]));
    out = static_cast<char *>(mmap(nullptr, hour_bytes[HOURS], PROT_WRITE,
                                   MAP_SHARED, out_fd, 0));
    failed |= out == MAP_FAILED;
    next_hour = 0;
  });
  auto work = [&](unsigned t) {
    for (size_t c; (c = next_chunk.fetch_add(1)) * CHUNK < size;) {
      // A chunk owns the lines that start inside it.
      const char *p = c ? find(data + c * CHUNK - 1, '\n') + 1 : data;
      workers[t].parse(p, data + std::min((c + 1) * CHUNK, size));
      // Drop the chunk's page-table entries now, in parallel, rather than
      // serially at exit; the page cache keeps the data. Never past EOF:
      // that would zero whatever memory follows the mapping.
      madvise(const_cast<char *>(data) + c * CHUNK,
              std::min(CHUNK, size - c * CHUNK), MADV_DONTNEED);
    }
    merged.arrive_and_wait();
    for (int i; (i = next_hour.fetch_add(1)) < HOURS;)
      aggregate(workers[t], by_size[i]);
    sized.arrive_and_wait();
    // Records are no longer needed; free them in parallel.
    madvise(workers[t].base, cap, MADV_DONTNEED);
    for (int h; !failed && (h = next_hour.fetch_add(1)) < HOURS;)
      format(h);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t)
    pool.emplace_back(work, t);
  work(0);
  for (auto &th : pool)
    th.join();
  _exit(failed);
}
