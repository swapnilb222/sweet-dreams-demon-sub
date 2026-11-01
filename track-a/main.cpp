// Speed Demon 2026, Track A (Ingest).
//
// One parallel region, three phases separated by barriers:
// 1. Threads claim 1 MiB chunks of the mmap'd log, parse every line once and
//    append a 4-byte record (20-bit latency, error bit, 11-bit thread-local
//    endpoint id) to a per-thread bucket for its hour.
//    Barrier: every distinct endpoint gets a global id in byte order.
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
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int HOURS = 72, MAX_EP = 2048, SLOTS = 4096, MAX_THREADS = 12;
constexpr size_t CHUNK = 1 << 20, PAGE = 4096;
// Latencies of 2^20 - 1 and up (about 1%) do not fit a record; they are kept
// whole in a side list instead.
constexpr uint32_t BIG = (1u << 20) - 1;
// Buckets grow in 4 KiB blocks from one pool per thread, so page tables stay
// compact and each bucket wastes at most one partly filled block.
constexpr size_t BLOCK = 1024;

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

bool same(const char *a, const char *b, uint32_t len) {
  for (uint32_t i = 0;; i += 32) {
    uint64_t diff = ~uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i)),
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i)))));
    if (len - i <= 32)
      return !(diff & ((1ull << (len - i)) - 1));
    if (diff)
      return false;
  }
}

unsigned digits(uint32_t v) {
  unsigned d = 1;
  for (uint32_t p = 10; p <= v && d < 10; p *= 10)
    ++d;
  return d;
}

char *put(char *o, uint32_t v) {
  char *end = o + digits(v);
  for (char *q = end; q > o; v /= 10)
    *--q = char('0' + v % 10);
  return end;
}

// Moves the elements of ranks rank[0..nr) (ascending) into their sorted
// positions within v[lo, hi), with branch-free three-way partitions.
void select(uint32_t *v, uint32_t lo, uint32_t hi, const uint32_t *rank,
            int nr) {
  while (nr) {
    if (hi - lo <= 32) {
      std::sort(v + lo, v + hi);
      return;
    }
    uint32_t a = v[lo], b = v[(lo + hi) / 2], c = v[hi - 1];
    uint32_t pivot = std::max(std::min(a, b), std::min(std::max(a, b), c));
    uint32_t less = lo, upto;
    for (uint32_t j = lo; j < hi; ++j) {
      uint32_t x = v[j];
      v[j] = v[less];
      v[less] = x;
      less += x < pivot;
    }
    upto = less;
    for (uint32_t j = less; j < hi; ++j) {
      uint32_t x = v[j];
      v[j] = v[upto];
      v[upto] = x;
      upto += x == pivot;
    }
    int l = 0, r;
    while (l < nr && rank[l] < less)
      ++l;
    for (r = l; r < nr && rank[r] < upto; ++r)
      ;
    select(v, lo, less, rank, l);
    rank += r, nr -= r, lo = upto;
  }
}

// Open-addressing endpoint table; names are copied into a compact arena so
// comparisons do not chase the whole file.
struct Dictionary {
  struct Slot {
    uint32_t hash;
    uint16_t len, id;
    const char *name; // saves a dependent load through names[id]
  };
  Slot slots[SLOTS];
  std::string_view names[MAX_EP];
  char *arena = static_cast<char *>(std::malloc(MAX_EP * 128 + 32));
  size_t used = 0;
  unsigned size = 0;

  uint32_t lookup(const char *s, uint32_t len) {
    uint64_t head = load8(s), tail = 0;
    if (len < 8)
      head &= (1ull << (8 * len)) - 1;
    else
      tail = load8(s + len - 8);
    uint32_t hash = _mm_crc32_u64(_mm_crc32_u64(len, head), tail);
    for (uint32_t i = hash;; ++i) {
      Slot &sl = slots[i % SLOTS];
      if (!sl.len) {
        names[size] = {static_cast<char *>(std::memcpy(arena + used, s, len)),
                       len};
        used += len;
        sl = {hash, uint16_t(len), uint16_t(size), names[size].data()};
        return size++;
      }
      if (sl.hash == hash && sl.len == len &&
          same(s, sl.name, len))
        return sl.id;
    }
  }
};

struct Worker {
  Dictionary local;
  uint16_t remap[MAX_EP]; // local id -> output order
  void *base; // the pool's mapping; pool advances through it
  uint32_t *pool, *cur[HOURS], *limit[HOURS];
  std::vector<uint32_t *> blocks[HOURS];
  std::vector<uint64_t> big[HOURS]; // (latency << 12) | (error << 11) | id
  uint32_t count[MAX_EP], errors[MAX_EP], offset[MAX_EP];
  uint32_t *scratch;

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
    while (p < end) {
      if (load8(p + 5) != key) // bytes 5..12, "MM-DDTHH", identify the hour
        key = load8(p + 5), h = hour_of(p);
      // Methods are 3 to 7 bytes, so the endpoint starts at p + 32..36.
      uint64_t m = eq_mask(p + 31, ' ') | uint64_t(eq_mask(p + 63, ' ')) << 32;
      const char *s = p + 32 + __builtin_ctzll(m);
      m &= m - 1;
      const char *e = m ? p + 31 + __builtin_ctzll(m) : find(p + 95, ' ');
      uint32_t id = local.lookup(s, uint32_t(e - s)) | (e[1] == '5') << 11;
      uint32_t v = 0;
      for (p = e + 5; *p != '\n'; ++p)
        v = v * 10 + uint32_t(*p - '0');
      ++p;
      if (v >= BIG) {
        big[h].push_back(uint64_t(v) << 12 | id);
        continue;
      }
      if (cur[h] == limit[h]) {
        blocks[h].push_back(cur[h] = pool);
        limit[h] = pool += BLOCK;
      }
      *cur[h]++ = v << 12 | id;
    }
  }
};

Worker workers[MAX_THREADS]; // zero pages, faulted in by their own threads
unsigned threads, neps;
Dictionary global;
std::vector<uint16_t> sorted_ids; // output order -> global id
int by_size[HOURS];               // hours, largest first
struct Stat {
  uint32_t n, errors, p[3];
};
Stat *stats;                       // [hour][output order]
size_t hour_bytes[HOURS + 1];      // becomes each hour's file offset
char *out;

void merge() {
  for (unsigned t = 0; t < threads; ++t)
    for (unsigned l = 0; l < workers[t].local.size; ++l)
      workers[t].remap[l] = uint16_t(global.lookup(
          workers[t].local.names[l].data(),
          uint32_t(workers[t].local.names[l].size())));
  neps = global.size;
  sorted_ids.resize(neps);
  std::vector<uint16_t> rank(neps);
  for (unsigned g = 0; g < neps; ++g)
    sorted_ids[g] = uint16_t(g);
  std::sort(sorted_ids.begin(), sorted_ids.end(), [](uint16_t a, uint16_t b) {
    return global.names[a] < global.names[b];
  });
  for (unsigned k = 0; k < neps; ++k)
    rank[sorted_ids[k]] = uint16_t(k);
  for (unsigned t = 0; t < threads; ++t)
    for (unsigned l = 0; l < workers[t].local.size; ++l)
      workers[t].remap[l] = rank[workers[t].remap[l]];
  // Hour sizes, rounded up to whole blocks, schedule phase 2 and bound its
  // scratch buffers.
  size_t lines[HOURS] = {};
  for (int h = 0; h < HOURS; ++h) {
    by_size[h] = h;
    for (unsigned t = 0; t < threads; ++t)
      lines[h] += workers[t].blocks[h].size() * BLOCK + workers[t].big[h].size();
  }
  std::sort(by_size, by_size + HOURS,
            [&](int a, int b) { return lines[a] > lines[b]; });
  for (unsigned t = 0; t < threads; ++t)
    workers[t].scratch =
        static_cast<uint32_t *>(std::malloc(lines[by_size[0]] * 4 + 4));
  stats = static_cast<Stat *>(std::calloc(size_t(HOURS) * neps, sizeof(Stat)));
}

void aggregate(Worker &me, int h) {
  uint32_t *cnt = me.count, *err = me.errors, *off = me.offset, *v = me.scratch;
  std::fill_n(cnt, neps, 0);
  std::fill_n(err, neps, 0);
  for (unsigned t = 0; t < threads; ++t)
    workers[t].each_record(h, [&](uint64_t r) {
      unsigned g = workers[t].remap[r & 2047];
      ++cnt[g];
      err[g] += r >> 11 & 1;
    });
  uint32_t acc = 0;
  for (unsigned g = 0; g < neps; ++g)
    off[g] = acc, acc += cnt[g];
  for (unsigned t = 0; t < threads; ++t)
    workers[t].each_record(h, [&](uint64_t r) {
      v[off[workers[t].remap[r & 2047]]++] = uint32_t(r >> 12);
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
    select(vg, 0, n, rank, 3);
    Stat &s = stats[size_t(h) * neps + g];
    s = {n, err[g], {vg[rank[0]], vg[rank[1]], vg[rank[2]]}};
    bytes += 28 + global.names[sorted_ids[g]].size() + digits(n) +
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
    const Stat &s = stats[size_t(h) * neps + g];
    if (!s.n)
      continue;
    std::string_view name = global.names[sorted_ids[g]];
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
  // Reserve that much address space; only touched pages are charged.
  size_t cap = (size / 40 + (HOURS + 1) * BLOCK) * 4;
  for (unsigned t = 0; t < threads; ++t) {
    void *p = mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
      return 1;
    madvise(p, cap, MADV_NOHUGEPAGE); // a huge page per block would be charged
    workers[t].pool = static_cast<uint32_t *>(workers[t].base = p);
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
