#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>
namespace {
using U64 = uint64_t;
constexpr U64 inf = UINT64_MAX / 4;
struct Input {
  const char *data = nullptr, *p = nullptr, *end = nullptr;
  size_t size = 0;
  explicit Input(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("Cannot open input");
    struct stat st{};
    if (fstat(fd, &st)) {
      close(fd);
      throw std::runtime_error("Cannot stat input");
    }
    size = size_t(st.st_size);
    if (size) {
      data = static_cast<const char *>(
          mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
      if (data == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("Cannot map input");
      }
    }
    close(fd);
    p = data;
    end = size ? data + size : data;
  }
  ~Input() {
    if (size)
      munmap(const_cast<char *>(data), size);
  }
  uint32_t next() {
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9')
      value = value * 10 + unsigned(*p++ - '0');
    ++p;
    return value;
  }
};
template <class T> struct HugeAllocator {
  using value_type = T;
  HugeAllocator() = default;
  template <class U> HugeAllocator(const HugeAllocator<U> &) {}
  static constexpr size_t page = 2 * 1024 * 1024;
  T *allocate(size_t n) {
    size_t bytes = (n * sizeof(T) + page - 1) & ~(page - 1);
    void *raw = mmap(nullptr, bytes + page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
      throw std::bad_alloc();
    uintptr_t base = reinterpret_cast<uintptr_t>(raw),
              aligned = (base + page - 1) & ~(page - 1);
    if (aligned != base)
      munmap(raw, aligned - base);
    size_t tail = page - (aligned - base);
    if (tail)
      munmap(reinterpret_cast<void *>(aligned + bytes), tail);
    madvise(reinterpret_cast<void *>(aligned), bytes, MADV_HUGEPAGE);
    return reinterpret_cast<T *>(aligned);
  }
  void deallocate(T *p, size_t n) {
    munmap(p, (n * sizeof(T) + page - 1) & ~(page - 1));
  }
  template <class U> bool operator==(const HugeAllocator<U> &) const {
    return true;
  }
};
struct Edge {
  uint32_t to, weight;
  Edge() noexcept {}
  Edge(uint32_t t, uint32_t w) : to(t), weight(w) {}
};
struct Graph {
  std::vector<uint32_t, HugeAllocator<uint32_t>> offsets;
  std::vector<Edge, HugeAllocator<Edge>> edges;
};
template <class Fn> void parallel(unsigned count, Fn fn) {
  std::vector<std::thread> threads;
  std::vector<std::exception_ptr> errors(count);
  for (unsigned k = 0; k < count; ++k)
    threads.emplace_back([&, k] {
      try {
        fn(k);
      } catch (...) {
        errors[k] = std::current_exception();
      }
    });
  for (auto &t : threads)
    t.join();
  for (auto error : errors)
    if (error)
      std::rethrow_exception(error);
}
std::array<Graph, 2> read_graph(const std::string &path) {
  Input input(path);
  uint32_t n = input.next(), m = input.next();
  std::array<Graph, 2> g;
  const char *start = input.p;
  struct Raw {
    uint32_t u, v, w;
  };
  unsigned parts = m < 10000 ? 1 : 6;
  // Separate write-heavy vector headers to avoid false sharing during parsing.
  struct alignas(64) Chunk {
    std::vector<Raw> edges;
  };
  std::vector<Chunk> chunks(parts);
  std::vector<const char *> cuts(parts + 1);
  cuts[0] = start;
  cuts[parts] = input.end;
  for (unsigned k = 1; k < parts; ++k) {
    const char *p = start + (input.end - start) * k / parts;
    cuts[k] =
        static_cast<const char *>(memchr(p, '\n', size_t(input.end - p))) + 1;
  }
  parallel(parts, [&](unsigned k) {
    auto &chunk = chunks[k].edges;
    chunk.reserve(size_t(m) / parts + m / (parts * 10) + 16);
    const char *p = cuts[k];
    auto next = [&]() {
      uint32_t v = 0;
      while (*p >= '0' && *p <= '9')
        v = v * 10 + unsigned(*p++ - '0');
      ++p;
      return v;
    };
    while (p < cuts[k + 1]) {
      uint32_t u = next(), v = next(), w = next();
      chunk.push_back({u, v, w});
    }
  });
  parallel(2, [&](unsigned k) {
    auto &x = g[k];
    x.offsets.resize(size_t(n) + 1);
    x.edges.resize(m);
    for (const auto &chunk : chunks)
      for (auto e : chunk.edges)
        ++x.offsets[(k ? e.v : e.u) + 1];
    for (uint32_t u = 0; u < n; ++u)
      x.offsets[u + 1] += x.offsets[u];
    for (const auto &chunk : chunks)
      for (auto e : chunk.edges)
        x.edges[--x.offsets[(k ? e.v : e.u) + 1]] = {k ? e.u : e.v, e.w};
    // Scatter decrements row ends; shift them to recover the CSR row starts.
    for (uint32_t u = 0; u < n; ++u)
      x.offsets[u] = x.offsets[u + 1];
    x.offsets[n] = m;
  });
  return g;
}
struct Item {
  U64 distance;
  uint32_t node;
};
struct Heap {
  std::array<std::vector<Item>, 65> buckets;
  U64 last = 0;
  size_t count = 0;
  void clear() {
    for (auto &b : buckets)
      b.clear();
    last = 0;
    count = 0;
  }
  void push(Item x) {
    buckets[std::bit_width(x.distance ^ last)].push_back(x);
    ++count;
  }
  bool empty() const { return !count; }
  size_t size() const { return count; }
  Item top() {
    if (buckets[0].empty()) {
      unsigned k = 1;
      while (buckets[k].empty())
        ++k;
      last = UINT64_MAX;
      for (auto x : buckets[k])
        last = std::min(last, x.distance);
      for (auto x : buckets[k])
        buckets[std::bit_width(x.distance ^ last)].push_back(x);
      buckets[k].clear();
    }
    return buckets[0].back();
  }
  void pop() {
    buckets[0].pop_back();
    --count;
  }
};
struct Distances {
  struct Slot {
    U64 distance = 0;
    uint32_t node = 0, epoch = 0;
  };
  std::vector<Slot> slots;
  uint32_t epoch = 0;
  size_t used = 0;
  explicit Distances(uint32_t) : slots(1024) {}
  size_t index(uint32_t u) const {
    size_t i = (u * 2654435761u) & (slots.size() - 1);
    while (slots[i].epoch == epoch && slots[i].node != u)
      i = (i + 1) & (slots.size() - 1);
    return i;
  }
  void clear() {
    ++epoch;
    used = 0;
    if (!epoch) {
      for (auto &x : slots)
        x.epoch = 0;
      epoch = 1;
    }
  }
  U64 get(uint32_t u) const {
    const auto &x = slots[index(u)];
    return x.epoch == epoch ? x.distance : inf;
  }
  void grow() {
    auto old = std::move(slots);
    slots.resize(old.size() * 2);
    for (auto x : old)
      if (x.epoch == epoch)
        slots[index(x.node)] = x;
  }
  bool improve(uint32_t u, U64 d) {
    if ((used + 1) * 2 >= slots.size())
      grow();
    auto &x = slots[index(u)];
    if (x.epoch == epoch && d >= x.distance)
      return false;
    if (x.epoch != epoch) {
      x.node = u;
      x.epoch = epoch;
      ++used;
    }
    x.distance = d;
    return true;
  }
  void set(uint32_t u, U64 d) { improve(u, d); }
};

U64 shortest(const std::array<Graph, 2> &g, uint32_t s, uint32_t t,
             std::array<Distances, 2> &d, std::array<Heap, 2> &heaps) {
  if (s == t)
    return 0;
  if (g[0].offsets[s] == g[0].offsets[s + 1] ||
      g[1].offsets[t] == g[1].offsets[t + 1])
    return inf;
  for (unsigned k = 0; k < 2; ++k) {
    d[k].clear();
    heaps[k].clear();
  }
  d[0].set(s, 0);
  heaps[0].push({0, s});
  d[1].set(t, 0);
  heaps[1].push({0, t});
  // Tentative opposite-side labels give upper bounds, not an early-stop rule.
  U64 best = inf;
  while (true) {
    U64 lower[2];
    for (unsigned k = 0; k < 2; ++k) {
      while (!heaps[k].empty() &&
             heaps[k].top().distance != d[k].get(heaps[k].top().node))
        heaps[k].pop();
      if (heaps[k].empty())
        return best;
      lower[k] = heaps[k].top().distance;
    }
    if (lower[0] + lower[1] >= best)
      return best;
    unsigned k = heaps[0].size() <= heaps[1].size() ? 0 : 1;
    auto [distance, u] = heaps[k].top();
    heaps[k].pop();
    for (uint32_t i = g[k].offsets[u]; i < g[k].offsets[u + 1]; ++i) {
      auto [v, w] = g[k].edges[i];
      U64 next = distance + w;
      if (d[k].improve(v, next)) {
        heaps[k].push({next, v});
        best = std::min(best, next + d[1 - k].get(v));
      }
    }
  }
}
void solve(const std::string &data, const std::string &out) {
  auto g = read_graph(data + "/graph.txt");
  uint32_t n = uint32_t(g[0].offsets.size() - 1);
  Input input(data + "/queries.txt");
  std::vector<std::pair<uint32_t, uint32_t>> queries;
  while (input.p != input.end) {
    auto s = input.next(), t = input.next();
    queries.emplace_back(s, t);
  }
  std::vector<U64> answers(queries.size());
  unsigned count = n < 4096 ? 1 : 12;
  count = std::min<unsigned>(count, unsigned(queries.size()));
  std::atomic<size_t> cursor{0};
  parallel(count, [&](unsigned) {
    std::array<Distances, 2> distances{Distances(n), Distances(n)};
    std::array<Heap, 2> heaps;
    for (size_t q; (q = cursor.fetch_add(1, std::memory_order_relaxed)) <
                   queries.size();) {
      auto [s, t] = queries[q];
      answers[q] = shortest(g, s, t, distances, heaps);
    }
  });
  FILE *file = std::fopen((out + "/distances.txt").c_str(), "wb");
  if (!file)
    throw std::runtime_error("Cannot open output");
  for (auto answer : answers)
    if (std::fprintf(file, "%lld\n",
                     answer == inf ? -1LL : static_cast<long long>(answer)) <
        0) {
      std::fclose(file);
      throw std::runtime_error("Output write failed");
    }
  if (std::fclose(file))
    throw std::runtime_error("Output close failed");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 1 && argc != 3)
      throw std::runtime_error("Usage: solution [data-dir out-dir]");
    solve(argc == 3 ? argv[1] : "/data", argc == 3 ? argv[2] : "/out");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
