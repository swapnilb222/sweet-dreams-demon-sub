// Track D: exact shortest paths for 2,000 local queries on a 5M-edge road graph.
// 1. Parse (SWAR, 12 threads) into bins keyed by the tail's part of the id
//    range, then build each part's CSR in cache.
// 2. Contract chains: edges into nodes of out-degree <= 2 that no query names
//    are redirected past them, which drops about 30% of the nodes.
// 3. Renumber the rest by BFS blobs, so graph neighbours are memory neighbours,
//    and build forward and reverse CSR in that numbering.
// 4. Bidirectional Dijkstra per query on a bucket queue, with distances kept
//    in pages handed out on first touch.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <malloc.h>
#include <memory>
#include <numeric>
#include <queue>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Esc = std::vector<std::pair<u32, u64>>; // slot -> weight too big to pack
template <class T> using Buf = std::unique_ptr<T[]>;
template <class T> static Buf<T> buf(size_t n) { return Buf<T>(new T[n]); }
static constexpr u64 INF = u64(1) << 62;
static constexpr u32 CH = 256, BLOB = 512;

// Persistent workers: thread start-up per phase would cost milliseconds.
static unsigned NT;
static std::function<void(unsigned)> job;
static std::barrier<> *gate;
static void par(const std::function<void(unsigned)> &f) {
  job = f;
  gate->arrive_and_wait();
  job(0);
  gate->arrive_and_wait();
}
static u32 share(u32 n, unsigned t) { return u32(u64(n) * t / NT); }

// Edge word: head in the low HB bits, weight above; an all-ones weight means
// the real weight is in the slot's escape table.
static u32 HB, HM, WM;
static u32 pack(u32 v, u64 w, u32 slot, Esc &esc) {
  if (w >= WM)
    esc.push_back({slot, w});
  return v | u32(std::min<u64>(w, WM)) << HB;
}
static u64 weight(u32 x, u32 slot, const Esc &esc) {
  u32 w = x >> HB;
  return w != WM ? w
                 : std::lower_bound(esc.begin(), esc.end(), std::pair{slot, u64(0)})
                       ->second;
}
static void merge(Esc &into, std::vector<Esc> &parts) {
  for (auto &e : parts)
    into.insert(into.end(), e.begin(), e.end());
  std::sort(into.begin(), into.end());
}
struct Csr {
  Buf<u32> off, adj;
  Esc esc;
};

// The file followed by zeros, so an 8-byte load at any byte stays mapped.
static const char *map(const std::string &path, size_t &len) {
  int fd = open(path.c_str(), O_RDONLY);
  struct stat st{};
  if (fd < 0 || fstat(fd, &st))
    std::exit(2);
  len = size_t(st.st_size);
  auto *p = static_cast<char *>(mmap(nullptr, len + 4096, PROT_READ,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (len)
    mmap(p, len, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
  close(fd);
  return p;
}
// Digits have bit 0x10 set and separators do not, so one load finds the end
// of a number of up to 7 digits and its digits combine in pairs, quads, halves.
static inline u32 num(const char *&p) {
  u64 w;
  memcpy(&w, p, 8);
  u64 stop = ~w & 0x1010101010101010ull;
  if (!stop) {
    u32 v = 0;
    while (*p >= '0')
      v = v * 10 + u32(*p++ - '0');
    return ++p, v;
  }
  unsigned len = unsigned(std::countr_zero(stop)) / 8;
  p += len + 1;
  w = (w << (64 - 8 * len)) & 0x0F0F0F0F0F0F0F0Full;
  w = (w * 10 + (w >> 8)) & 0x00FF00FF00FF00FFull;
  w = (w * 100 + (w >> 16)) & 0x0000FFFF0000FFFFull;
  return u32(w * 10000 + (w >> 32));
}

// One thread's edges, filed by tail part in chunks carved from one arena.
struct Bin {
  Buf<u32> key; // tail offset in part << HB | head
  Buf<u16> w;   // 0xFFFF: next value of heavy[part]
  std::vector<std::vector<u32>> chunks, heavy;
  std::vector<u32> len;
  template <class F> void each(u32 p, F &&f) const {
    u32 left = len[p];
    for (u32 c : chunks[p]) {
      u32 k = std::min(left, CH);
      for (u32 i = c * CH; i < c * CH + k; ++i)
        f(key[i], w[i]);
      left -= k;
    }
  }
};

static Csr read_graph(const std::string &path, u32 &n,
                      const std::vector<std::pair<u32, u32>> &qs,
                      std::vector<u64> &bypass) {
  size_t size;
  const char *text = map(path, size), *p = text;
  n = num(p);
  num(p);
  HB = std::max(1, std::bit_width(n - 1));
  HM = (1u << HB) - 1, WM = ~0u >> HB;
  const u32 PB = std::min(16u, 32 - HB), parts = ((n - 1) >> PB) + 1;
  std::vector<const char *> cut(NT + 1, text + size);
  cut[0] = p;
  for (unsigned t = 1; t < NT; ++t) {
    const char *q = p + size_t(text + size - p) * t / NT;
    while (q < text + size && q[-1] != '\n')
      ++q;
    cut[t] = q;
  }
  std::vector<Bin> bins(NT);
  par([&](unsigned t) {
    Bin &b = bins[t];
    size_t cap = size_t(cut[t + 1] - cut[t]) / 6 + size_t(parts + 1) * CH;
    b.key = buf<u32>(cap), b.w = buf<u16>(cap);
    b.chunks.resize(parts), b.heavy.resize(parts), b.len.assign(parts, 0);
    std::vector<u32> at(parts);
    u32 next = 0;
    for (const char *q = cut[t]; q < cut[t + 1];) {
      u32 u = num(q), v = num(q), w = num(q), k = u >> PB;
      if (u == v)
        continue; // a self-loop never shortens a path
      if (b.len[k]++ % CH == 0)
        b.chunks[k].push_back(next), at[k] = next++ * CH;
      if (w >= 0xFFFF)
        b.heavy[k].push_back(w), w = 0xFFFF;
      b.key[at[k]] = (u & ((1u << PB) - 1)) << HB | v, b.w[at[k]++] = u16(w);
    }
  });
  std::vector<u32> base(parts + 1, 0);
  for (u32 k = 0; k < parts; ++k) {
    base[k + 1] = base[k];
    for (auto &b : bins)
      base[k + 1] += b.len[k];
  }
  std::vector<u64> term((n >> 6) + 1);
  for (auto [s, t] : qs)
    term[s >> 6] |= u64(1) << (s & 63), term[t >> 6] |= u64(1) << (t & 63);
  bypass.assign(term.size(), 0);
  Csr g;
  g.off = buf<u32>(n + 1), g.adj = buf<u32>(base[parts]);
  g.off[n] = base[parts];
  std::vector<Esc> esc(NT);
  std::atomic<u32> next{0};
  par([&](unsigned t) {
    std::vector<u32> cnt(1u << PB);
    for (u32 k; (k = next++) < parts;) {
      u32 lo = k << PB, len = std::min(n - lo, 1u << PB), at = base[k];
      std::fill(cnt.begin(), cnt.end(), 0);
      for (auto &b : bins)
        b.each(k, [&](u32 key, u16) { ++cnt[key >> HB]; });
      for (u32 i = 0, u = lo; i < len; ++i, ++u) {
        bool skip = cnt[i] <= 2 && !(term[u >> 6] >> (u & 63) & 1);
        bypass[u >> 6] |= u64(skip) << (u & 63); // parts are word aligned
        g.off[u] = at, at += cnt[i], cnt[i] = g.off[u];
      }
      for (auto &b : bins) {
        auto h = b.heavy[k].begin();
        b.each(k, [&](u32 key, u16 w) {
          u32 s = cnt[key >> HB]++;
          g.adj[s] = pack(key & HM, w == 0xFFFF ? *h++ : w, s, esc[t]);
        });
      }
    }
  });
  merge(g.esc, esc);
  return g;
}

// Redirects every edge of a kept node that enters a bypassable node (out-degree
// <= 2, no query endpoint) along the chain to the first node that must stay;
// a useless walk becomes a self-loop. Walks advance in lockstep, 32 at a time,
// so their cache misses overlap.
static void contract(Csr &g, u32 n, const std::vector<u64> &bypass) {
  auto skip = [&](u32 v) { return bypass[v >> 6] >> (v & 63) & 1; };
  struct Walk {
    u32 e, root, prev, at, steps;
    u64 w;
  };
  std::vector<Esc> esc(NT);
  par([&](unsigned t) {
    std::vector<Walk> live;
    auto run = [&] {
      while (!live.empty()) {
        size_t kept = 0;
        for (Walk k : live) {
          u32 ahead = 0, next = 0, end = ~0u;
          u64 nw = 0;
          for (u32 e = g.off[k.at]; e < g.off[k.at + 1]; ++e)
            if (u32 h = g.adj[e] & HM; h != k.prev && h != k.at)
              ++ahead, next = h, nw = weight(g.adj[e], e, g.esc);
          if (ahead > 1)
            end = k.at;
          else if (ahead == 0 || next == k.root)
            end = k.root, k.w = 0;
          else if (k.w += nw, k.prev = k.at, k.at = next;
                   !skip(next) || ++k.steps == 64)
            end = next;
          if (end != ~0u)
            g.adj[k.e] = pack(end, k.w, k.e, esc[t]);
          else
            __builtin_prefetch(&g.off[next]), live[kept++] = k;
        }
        live.resize(kept);
      }
    };
    for (u32 u = share(n, t); u < share(n, t + 1); ++u) {
      for (u32 e = g.off[u]; !skip(u) && e < g.off[u + 1]; ++e)
        if (u32 y = g.adj[e] & HM; skip(y))
          live.push_back({e, u, u, y, 0, weight(g.adj[e], e, g.esc)}),
              __builtin_prefetch(&g.off[y]);
      if (live.size() >= 32)
        run();
    }
    run();
  });
  // Fresh entries first, so they win over stale ones for the same slot.
  Esc all;
  esc.push_back(std::move(g.esc));
  for (auto &e : esc)
    all.insert(all.end(), e.begin(), e.end());
  std::stable_sort(all.begin(), all.end(),
                   [](auto &a, auto &b) { return a.first < b.first; });
  all.erase(std::unique(all.begin(), all.end(),
                        [](auto &a, auto &b) { return a.first == b.first; }),
            all.end());
  g.esc = std::move(all);
}

// New ids: each thread grows compact BFS blobs over unclaimed nodes, seeding
// the next blob from the last one's frontier, so consecutive ids are close.
// Visiting a node also copies its edges, so the forward graph in the new
// numbering is then a streaming pass that only renames heads.
struct Region {
  std::vector<u32> ord, ends;
  Buf<u32> out;
  Esc esc;
};
static Csr reorder(Csr &&g, u32 &n, std::vector<std::pair<u32, u32>> &qs,
                   const std::vector<u64> &bypass) {
  auto skip = [&](u32 v) { return bypass[v >> 6] >> (v & 63) & 1; };
  // Claims race without locked instructions, checked first against a private
  // bitmap. A node claimed twice leaves a copy that no edge reaches, since
  // heads resolve through one id entry, so it is never on a shortest path.
  std::vector<std::atomic<u64>> taken((n >> 6) + 1);
  std::vector<Region> reg(NT);
  par([&](unsigned t) {
    std::vector<u64> mine(taken.size());
    auto seen = [&](u32 v) {
      return (mine[v >> 6] | taken[v >> 6].load(std::memory_order_relaxed)) >>
                 (v & 63) & 1;
    };
    auto claim = [&](u32 v) {
      u64 bit = u64(1) << (v & 63), w;
      if (mine[v >> 6] & bit)
        return false;
      mine[v >> 6] |= bit;
      if ((w = taken[v >> 6].load(std::memory_order_relaxed)) & bit)
        return false;
      taken[v >> 6].store(w | bit, std::memory_order_relaxed);
      return true;
    };
    Region &r = reg[t];
    r.ord.reserve(n / NT + n / NT / 4), r.ends.reserve(r.ord.capacity());
    r.out = buf<u32>(g.off[n]);
    std::vector<u32> seeds;
    u32 j = 0;
    for (u32 scan = share(n, t);;) {
      u32 seed = ~0u;
      while (seed == ~0u && !seeds.empty()) {
        u32 v = seeds.back();
        seeds.pop_back();
        if (claim(v))
          seed = v;
      }
      for (; seed == ~0u && scan < share(n, t + 1); ++scan)
        if (!skip(scan) && claim(scan))
          seed = scan;
      if (seed == ~0u)
        return;
      size_t blob = r.ord.size();
      r.ord.push_back(seed);
      for (size_t i = blob; i < r.ord.size(); ++i) {
        if (i + 4 < r.ord.size())
          __builtin_prefetch(&g.adj[g.off[r.ord[i + 4]]]);
        for (u32 u = r.ord[i], e = g.off[u]; e < g.off[u + 1]; ++e) {
          u32 x = g.adj[e], v = x & HM;
          if (v == u)
            continue;
          if (x >> HB == WM)
            r.esc.push_back({j, weight(x, e, g.esc)});
          r.out[j++] = x;
          if (r.ord.size() - blob < BLOB) {
            if (claim(v))
              r.ord.push_back(v), __builtin_prefetch(&g.off[v]);
          } else if (!seen(v))
            seeds.push_back(v);
        }
        r.ends.push_back(j);
      }
    }
  });
  g = {};
  std::vector<u32> vs(NT + 1, 0), es(NT + 1, 0);
  for (unsigned t = 0; t < NT; ++t)
    vs[t + 1] = vs[t] + u32(reg[t].ord.size()),
           es[t + 1] = es[t] + (reg[t].ends.empty() ? 0 : reg[t].ends.back());
  auto id = buf<u32>(n);
  par([&](unsigned t) {
    for (u32 i = 0; i < reg[t].ord.size(); ++i)
      id[reg[t].ord[i]] = vs[t] + i;
  });
  n = vs[NT];
  Csr f;
  f.off = buf<u32>(n + 1), f.adj = buf<u32>(es[NT]);
  f.off[n] = es[NT];
  std::vector<Esc> esc(NT);
  par([&](unsigned t) {
    Region &r = reg[t];
    for (u32 i = 0; i < r.ord.size(); ++i)
      f.off[vs[t] + i] = es[t] + (i ? r.ends[i - 1] : 0);
    u32 m = es[t + 1] - es[t], *a = &f.adj[es[t]];
    for (u32 j = 0; j < m; ++j) {
      if (j + 32 < m)
        __builtin_prefetch(&id[r.out[j + 32] & HM]);
      a[j] = id[r.out[j] & HM] | (r.out[j] & ~HM);
    }
    for (auto [j, w] : r.esc)
      esc[t].push_back({es[t] + j, w});
    r = {};
  });
  merge(f.esc, esc);
  for (auto &[s, t] : qs)
    s = id[s], t = id[t];
  return f;
}

// Transpose. Each thread owns a slice of heads; edges into other slices are
// handed over through inboxes, so no counter is shared.
static Csr reverse(const Csr &f, u32 n) {
  struct In {
    u32 v, x, e; // head, tail | weight bits, forward slot
  };
  std::vector<std::vector<std::vector<In>>> box(
      NT, std::vector<std::vector<In>>(NT));
  std::vector<u32> cut(NT + 1), tot(NT + 1, 0);
  for (unsigned t = 0; t <= NT; ++t)
    cut[t] = share(n, t);
  Csr r;
  r.off = buf<u32>(n + 1), r.adj = buf<u32>(f.off[n]);
  r.off[n] = f.off[n];
  par([&](unsigned t) {
    u32 lo = cut[t], hi = cut[t + 1];
    std::fill(&r.off[lo], &r.off[hi], 0);
    for (u32 u = lo; u < hi; ++u)
      for (u32 e = f.off[u]; e < f.off[u + 1]; ++e)
        if (u32 x = f.adj[e], v = x & HM; v - lo < hi - lo)
          ++r.off[v];
        else
          box[t][std::upper_bound(cut.begin(), cut.end(), v) - cut.begin() - 1]
              .push_back({v, u | (x & ~HM), e});
  });
  par([&](unsigned t) {
    for (auto &b : box)
      for (In i : b[t])
        ++r.off[i.v];
    for (u32 v = cut[t]; v < cut[t + 1]; ++v)
      tot[t + 1] += r.off[v];
  });
  for (unsigned t = 0; t < NT; ++t)
    tot[t + 1] += tot[t];
  std::vector<Esc> esc(NT);
  par([&](unsigned t) {
    u32 lo = cut[t], hi = cut[t + 1];
    std::vector<u32> at(hi - lo);
    for (u32 v = lo, s = tot[t]; v < hi; ++v)
      at[v - lo] = s, s += r.off[v], r.off[v] = at[v - lo];
    auto put = [&](u32 v, u32 x, u32 e) {
      u32 s = at[v - lo]++;
      r.adj[s] = x;
      if (x >> HB == WM)
        esc[t].push_back({s, weight(x, e, f.esc)});
    };
    for (u32 u = lo; u < hi; ++u)
      for (u32 e = f.off[u]; e < f.off[u + 1]; ++e)
        if (u32 x = f.adj[e], v = x & HM; v - lo < hi - lo)
          put(v, u | (x & ~HM), e);
    for (auto &b : box)
      for (In i : b[t])
        put(i.v, i.x, i.e);
  });
  merge(r.esc, esc);
  return r;
}

// One search direction: a ring of distance buckets (far jumps wait in a heap)
// and distances in pages materialized on first write, recycled per query.
struct Side {
  static constexpr unsigned SH = 6, NB = 1024, PS = 7, PG = 1u << PS;
  const Csr &g;
  std::vector<u64 *> dir;
  std::vector<u32> used;
  Buf<u64> pool;
  u64 blank[PG];
  std::vector<std::vector<std::pair<u32, u32>>> bk; // node, low bits of d
  std::priority_queue<std::pair<u64, u32>, std::vector<std::pair<u64, u32>>,
                      std::greater<>>
      far;
  u64 cur = 0;
  size_t near = 0;
  Side(const Csr &c, u32 n)
      : g(c), dir((n >> PS) + 1), pool(buf<u64>(dir.size() * PG)), bk(NB) {
    std::fill(blank, blank + PG, INF);
    std::fill(dir.begin(), dir.end(), +blank);
  }
  const u64 &get(u32 v) const { return dir[v >> PS][v & (PG - 1)]; }
  u64 &at(u32 v) {
    u64 *&p = dir[v >> PS];
    if (p == blank) {
      p = &pool[used.size() * PG];
      std::fill(p, p + PG, INF);
      used.push_back(v >> PS);
    }
    return p[v & (PG - 1)];
  }
  void push(u64 d, u32 v) {
    if ((d >> SH) - cur < NB)
      bk[(d >> SH) & (NB - 1)].push_back({v, u32(d)}), ++near;
    else
      far.push({d, v});
  }
  void clear() {
    for (u32 pg : used)
      dir[pg] = blank;
    used.clear();
    for (auto &b : bk)
      b.clear();
    far = {};
    cur = near = 0;
  }
  void start(u32 s) { clear(), at(s) = 0, push(0, s); }
  // Moves to the first nonempty bucket; returns its smallest possible key.
  u64 lower() {
    u64 fb = far.empty() ? ~u64(0) : far.top().first >> SH;
    if (!near && far.empty())
      return INF;
    if (!near)
      cur = fb;
    while (cur < fb && bk[cur & (NB - 1)].empty())
      ++cur;
    for (; !far.empty() && (far.top().first >> SH) < cur + NB; far.pop())
      bk[(far.top().first >> SH) & (NB - 1)].push_back(
          {far.top().second, u32(far.top().first)}),
          ++near;
    return cur << SH;
  }
  // Settles the current bucket (label-correcting inside it); returns the count.
  size_t drain(const Side &o, u64 &best) {
    auto &b = bk[cur & (NB - 1)];
    size_t done = 0;
    for (size_t k = 0; k < b.size(); ++k) {
      if (k + 8 < b.size())
        __builtin_prefetch(&g.off[b[k + 8].first]);
      if (k + 4 < b.size()) {
        __builtin_prefetch(&g.adj[g.off[b[k + 4].first]]);
        __builtin_prefetch(&get(b[k + 4].first));
      }
      auto [u, low] = b[k];
      u64 d = get(u);
      if (u32(d) != low)
        continue;
      ++done;
      for (u32 e = g.off[u]; e < g.off[u + 1]; ++e) {
        u32 x = g.adj[e], v = x & HM;
        u64 nd = d + weight(x, e, g.esc), &dv = at(v);
        if (nd < dv) {
          dv = nd, push(nd, v);
          best = std::min(best, nd + o.get(v));
        }
      }
    }
    near -= b.size();
    b.clear();
    return done;
  }
};

// Forward alone for short queries, then bidirectional, growing the side that
// has settled less; stops once the two lower bounds reach the best meeting.
struct Solver {
  static constexpr size_t PROBE = 256;
  const Csr &F, &R;
  Side f, b;
  Solver(const Csr &fw, const Csr &rv, u32 n)
      : F(fw), R(rv), f(fw, n), b(rv, n) {}
  int64_t solve(u32 s, u32 t) {
    if (s == t)
      return 0;
    if (F.off[s] == F.off[s + 1] || R.off[t] == R.off[t + 1])
      return -1;
    b.clear(); // not started yet: every backward label is INF
    f.start(s);
    u64 best = INF;
    size_t fs = 0, bs = 0;
    while (fs < PROBE) {
      u64 lf = f.lower(), dt = f.get(t);
      if (dt <= lf || lf == INF)
        return dt >= INF ? -1 : int64_t(dt);
      fs += f.drain(b, best);
    }
    b.start(t);
    best = f.get(t);
    for (;;) {
      u64 lf = f.lower(), lb = b.lower();
      if (lf == INF || lb == INF || lf + lb >= best)
        break;
      if (fs <= bs)
        fs += f.drain(b, best);
      else
        bs += b.drain(f, best);
    }
    return best >= INF ? -1 : int64_t(best);
  }
};

int main(int argc, char **argv) {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); // partly used arrays stay small
  mallopt(M_MMAP_THRESHOLD, 1 << 16);    // freed arrays go back to the kernel
  const char *env = getenv("SD_DATA"), *envo = getenv("SD_OUT");
  std::string data = argc > 2 ? argv[1] : env ? env : "/data",
              out = argc > 2 ? argv[2] : envo ? envo : "/out";
  NT = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
  gate = new std::barrier<>(NT);
  for (unsigned t = 1; t < NT; ++t)
    std::thread([t] {
      for (;;)
        gate->arrive_and_wait(), job(t), gate->arrive_and_wait();
    }).detach();
  size_t qsize;
  const char *q = map(data + "/queries.txt", qsize), *qend = q + qsize;
  std::vector<std::pair<u32, u32>> qs;
  while (q < qend) {
    u32 s = num(q);
    qs.push_back({s, num(q)});
  }
  u32 n;
  std::vector<u64> bypass;
  Csr fwd = read_graph(data + "/graph.txt", n, qs, bypass);
  contract(fwd, n, bypass);
  fwd = reorder(std::move(fwd), n, qs, bypass);
  Csr rev = reverse(fwd, n);
  std::vector<u32> order(qs.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](u32 a, u32 b) { return qs[a] < qs[b]; });
  std::vector<int64_t> ans(qs.size());
  std::atomic<size_t> next{0};
  par([&](unsigned) {
    Solver solver(fwd, rev, n);
    for (size_t k; (k = next++) < qs.size();)
      ans[order[k]] = solver.solve(qs[order[k]].first, qs[order[k]].second);
  });
  std::string text(qs.size() * 21, '\0');
  char *w = text.data();
  for (int64_t a : ans)
    w = std::to_chars(w, w + 20, a).ptr, *w++ = '\n';
  FILE *file = fopen((out + "/distances.txt").c_str(), "wb");
  if (!file || fwrite(text.data(), 1, size_t(w - text.data()), file) !=
                   size_t(w - text.data()) || fclose(file))
    return 1;
  _exit(0);
}
