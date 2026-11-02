// Track D: exact shortest paths for 2,000 local queries on a 5M-edge road graph.
// 1. Parse (SWAR, 12 threads) into bins keyed by the tail's part of the id
//    range, then build each part's CSR in cache.
// 2. Renumber by BFS blobs, so graph neighbours are memory neighbours. While
//    expanding a node, each edge into a node of out-degree <= 2 that no query
//    names is walked along its chain to the first node that must stay, which
//    drops about 30% of the nodes. Then build forward and reverse CSR.
// 4. Bidirectional Dijkstra per query on a bucket queue, with distances kept
//    in pages handed out on first touch.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdio>
#include <cstdint>
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

using u32 = uint32_t;
using u64 = uint64_t;
using Esc = std::vector<std::pair<u32, u64>>; // slot -> weight too big to pack
template <class T> using Buf = std::unique_ptr<T[]>;
template <class T> static Buf<T> buf(size_t n) { return Buf<T>(new T[n]); }
static constexpr u64 INF = u64(1) << 62;
static constexpr u32 BLOB = 512, BATCH = 8;

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
// Sorted by slot; for a slot listed twice, lookups find the earlier part's.
static void merge(Esc &into, std::vector<Esc> &parts) {
  for (auto &e : parts)
    into.insert(into.end(), e.begin(), e.end());
  std::stable_sort(into.begin(), into.end(),
                   [](auto &a, auto &b) { return a.first < b.first; });
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
    _exit(2);
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
  __builtin_memcpy(&w, p, 8);
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

// One thread's edges, filed by tail part in page-sized chunks. Each half of
// the parts has its own arena, unmapped once that half's CSR is written.
static constexpr u32 CR = 682, G = 2; // records in a 4 KiB page, arenas
struct alignas(4096) Chunk {
  u32 x[CR];       // head | weight bits, as in the CSR
  uint16_t lo[CR]; // tail offset in its part
};
struct Bin {
  Chunk *arena[G];
  size_t cap;
  std::vector<std::vector<Chunk *>> chunks;
  std::vector<std::vector<u32>> heavy; // weights >= WM
  std::vector<u32> len;
  template <class F> void each(u32 p, F &&f) const {
    for (u32 left = len[p]; Chunk *c : chunks[p])
      for (u32 i = 0, k = std::min(left, CR); i < k; ++i, --left)
        f(c->x[i], c->lo[i]);
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
  const u32 PB = 14, PM = (1u << PB) - 1, parts = (n - 1) / (PM + 1) + 1;
  auto cut = [&](unsigned t) { // thread t's first line
    const char *q = p + size_t(text + size - p) * t / NT;
    while (t && q < text + size && q[-1] != '\n')
      ++q;
    return q;
  };
  std::vector<Bin> bins(NT);
  par([&](unsigned t) {
    Bin &b = bins[t];
    b.cap = size_t(cut(t + 1) - cut(t)) / 6 / CR + parts + 1;
    Chunk *next[G];
    for (u32 a = 0; a < G; ++a)
      next[a] = b.arena[a] = static_cast<Chunk *>(
          mmap(nullptr, b.cap * sizeof(Chunk), PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    b.chunks.resize(parts), b.heavy.resize(parts), b.len.assign(parts, 0);
    std::vector<u32> place(parts);
    std::vector<Chunk *> cur(parts);
    u32 *__restrict at = place.data(), *__restrict len = b.len.data();
    for (const char *q = cut(t), *end = cut(t + 1); q < end;) {
      u32 u = num(q), v = num(q), w = num(q), k = u >> PB;
      if (u == v)
        continue; // a self-loop never shortens a path
      if (len[k]++ % CR == 0)
        b.chunks[k].push_back(cur[k] = next[k * G / parts]++), at[k] = 0;
      if (w >= WM)
        b.heavy[k].push_back(w), w = WM;
      cur[k]->x[at[k]] = v | w << HB, cur[k]->lo[at[k]++] = uint16_t(u & PM);
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
  // One group of parts at a time; its bins are unmapped between groups, when
  // no other thread is faulting pages in (munmap stalls them all).
  for (u32 a = 0; a < G; ++a) {
    std::atomic<u32> next{(a * parts + G - 1) / G};
    u32 end = ((a + 1) * parts + G - 1) / G;
    par([&](unsigned t) {
      std::vector<u32> cnt(PM + 1);
      for (u32 k; (k = next++) < end;) {
        u32 lo = k << PB, len = std::min(n - lo, PM + 1), at = base[k];
        std::fill(cnt.begin(), cnt.end(), 0);
        for (auto &b : bins)
          b.each(k, [&](u32, uint16_t i) { ++cnt[i]; });
        for (u32 i = 0, u = lo; i < len; ++i, ++u) {
          bool skip = cnt[i] <= 2 && !(term[u >> 6] >> (u & 63) & 1);
          bypass[u >> 6] |= u64(skip) << (u & 63); // parts are word aligned
          g.off[u] = at, at += cnt[i], cnt[i] = g.off[u];
        }
        for (auto &b : bins) {
          auto h = b.heavy[k].begin();
          b.each(k, [&](u32 x, uint16_t i) {
            u32 s = cnt[i]++;
            g.adj[s] = x >> HB == WM ? pack(x & HM, *h++, s, esc[t]) : x;
          });
        }
      }
    });
    for (auto &b : bins)
      munmap(b.arena[a], b.cap * sizeof(Chunk));
  }
  merge(g.esc, esc);
  return g;
}

// Fills the transpose. Thread t owns the heads in [cut[t], cut[t + 1]); the
// rename pass counted the edges into its slice from inside and passed the
// others through inboxes, so no counter is shared.
struct In {
  u32 v, x, e; // head, tail | weight bits, forward slot
};
using Boxes = std::vector<std::vector<std::vector<In>>>;
static void reverse(const Csr &f, Csr &r, const std::vector<u32> &cut,
                    const Boxes &box) {
  std::vector<u32> tot(NT + 1, 0);
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
}

// New ids: each thread grows compact BFS blobs over unclaimed nodes, seeding
// the next blob from the last one's frontier, so consecutive ids are close.
// Visiting a node also copies its edges, so the forward graph in the new
// numbering is then a streaming pass that only renames heads.
struct Region {
  std::vector<u32> ord, wide; // wide: copied degrees >= 255, in order
  std::vector<uint8_t> deg;
  Buf<u32> out;
  u32 m = 0;
  Esc esc;
};
static Csr reorder(Csr &&g, u32 &n, std::vector<std::pair<u32, u32>> &qs,
                   const std::vector<u64> &bypass, Csr &rv) {
  auto skip = [&](u32 v) { return bypass[v >> 6] >> (v & 63) & 1; };
  // Claims race without locked instructions, so a racing store can lose a bit
  // and a node can be claimed twice. The extra copy is harmless (heads resolve
  // through one id entry, so no edge reaches it), unless the copies push the
  // node count past the head field or a thread's edge buffer: then the pass
  // reruns with atomic claims.
  std::vector<std::atomic<u64>> taken((n >> 6) + 1);
  std::vector<Region> reg(NT);
  std::vector<u32> vs(NT + 1, 0), es(NT + 1, 0);
  for (bool exact = false, full = false;; exact = true, full = false) {
  par([&](unsigned t) {
    auto claim = [&](u32 v) {
      u64 bit = u64(1) << (v & 63), w = taken[v >> 6].load(std::memory_order_relaxed);
      if (w & bit)
        return false;
      if (exact)
        return !(taken[v >> 6].fetch_or(bit, std::memory_order_relaxed) & bit);
      return taken[v >> 6].store(w | bit, std::memory_order_relaxed), true;
    };
    Region &r = reg[t];
    r.ord.reserve(n / NT + n / NT / 4), r.deg.reserve(r.ord.capacity());
    r.out = buf<u32>(g.off[n]);
    // Locals, so the stores into out cannot alias them.
    u32 *__restrict out = r.out.get(), j = 0;
    const u32 *__restrict off = g.off.get(), *__restrict adj = g.adj.get();
    std::vector<u32> seeds;
    struct Walk {
      u32 slot, root, prev, at, steps;
      u64 w;
    };
    std::vector<Walk> walks;
    for (u32 scan = share(n, t);;) {
      u32 seed = ~0u;
      for (; seed == ~0u && !seeds.empty(); seeds.pop_back())
        if (claim(seeds.back()))
          seed = seeds.back();
      for (; seed == ~0u && scan < share(n, t + 1); ++scan)
        if (!skip(scan) && claim(scan))
          seed = scan;
      if (seed == ~0u)
        return void(r.m = j);
      size_t blob = r.ord.size();
      auto visit = [&](u32 v) {
        if (r.ord.size() - blob < BLOB) {
          if (claim(v))
            r.ord.push_back(v), __builtin_prefetch(&off[v]);
        } else if (!(taken[v >> 6] >> (v & 63) & 1))
          seeds.push_back(v);
      };
      auto put = [&](u32 slot, u32 v, u64 w) {
        if (w >= WM)
          r.esc.push_back({slot, w});
        out[slot] = v | u32(std::min<u64>(w, WM)) << HB;
      };
      r.ord.push_back(seed);
      for (size_t lo = blob, hi; lo < r.ord.size(); lo = hi) {
        hi = std::min(r.ord.size(), lo + BATCH);
        for (size_t i = lo; i < hi; ++i) {
          if (i + BATCH < r.ord.size())
            __builtin_prefetch(&adj[off[r.ord[i + BATCH]]]);
          u32 from = j;
          if (j + off[r.ord[i] + 1] - off[r.ord[i]] > off[n])
            return void(full = true); // copies overran the edge buffer
          for (u32 u = r.ord[i], e = off[u]; e < off[u + 1]; ++e) {
            u32 x = adj[e], v = x & HM;
            if (v == u)
              continue;
            if (skip(v)) // walked below; its slot is filled when it ends
              walks.push_back({j++, u, u, v, 0, weight(x, e, g.esc)}),
                  __builtin_prefetch(&off[v]);
            else
              put(j++, v, weight(x, e, g.esc)), visit(v);
          }
          r.deg.push_back(uint8_t(std::min(j - from, 255u)));
          if (j - from >= 255)
            r.wide.push_back(j - from);
        }
        // Chain walks of this batch advance one hop per round, so their
        // misses overlap. A branch ends a walk at a node that stays; a dead
        // end or a way back to the root leaves a useless self-loop.
        while (!walks.empty()) {
          size_t kept = 0;
          for (Walk &k : walks) // the offsets were fetched last round
            __builtin_prefetch(&adj[off[k.at]]);
          for (Walk k : walks) {
            u32 ahead = 0, next = 0, end = ~0u;
            u64 nw = 0;
            for (u32 e = off[k.at]; e < off[k.at + 1]; ++e)
              if (u32 h = adj[e] & HM; h != k.prev && h != k.at)
                ++ahead, next = h, nw = weight(adj[e], e, g.esc);
            if (ahead > 1)
              end = k.at;
            else if (ahead == 0 || next == k.root)
              end = k.root, k.w = 0;
            else if (k.w += nw, k.prev = k.at, k.at = next;
                     !skip(next) || ++k.steps == 64)
              end = next;
            if (end == ~0u)
              __builtin_prefetch(&off[next]), walks[kept++] = k;
            else if (put(k.slot, end, k.w); end != k.root)
              visit(end);
          }
          walks.resize(kept);
        }
      }
    }
  });
  for (unsigned t = 0; t < NT; ++t)
    vs[t + 1] = vs[t] + u32(reg[t].ord.size()),
           es[t + 1] = es[t] + reg[t].m;
  if (vs[NT] <= HM + 1 && !full)
    break;
  for (auto &w : taken)
    w = 0;
  for (auto &r : reg)
    r = {};
  }
  g = {};
  auto id = buf<u32>(n);
  par([&](unsigned t) {
    for (u32 i = 0; i < reg[t].ord.size(); ++i)
      id[reg[t].ord[i]] = vs[t] + i;
    reg[t].ord = {};
  });
  n = vs[NT];
  Csr f;
  f.off = buf<u32>(n + 1), f.adj = buf<u32>(es[NT]);
  rv.off = buf<u32>(n + 1), rv.adj = buf<u32>(es[NT]);
  f.off[n] = rv.off[n] = es[NT];
  std::vector<Esc> esc(NT);
  Boxes box(NT, std::vector<std::vector<In>>(NT));
  par([&](unsigned t) {
    Region &r = reg[t];
    auto w = r.wide.begin();
    u32 lo = vs[t], hi = vs[t + 1], m = es[t + 1] - es[t];
    u32 *__restrict a = &f.adj[es[t]], *__restrict ro = rv.off.get();
    const u32 *__restrict out = r.out.get(), *__restrict ids = id.get();
    std::fill(&ro[lo], &ro[hi], 0);
    // Renames heads and counts the reverse degrees of this thread's slice.
    for (u32 u = lo, j = 0; u < hi; ++u) {
      f.off[u] = es[t] + j;
      for (u32 k = r.deg[u - lo] == 255 ? *w++ : r.deg[u - lo]; k--; ++j) {
        if (j + 32 < m)
          __builtin_prefetch(&ids[out[j + 32] & HM]);
        u32 x = ids[out[j] & HM] | (out[j] & ~HM), v = x & HM;
        a[j] = x;
        if (v - lo < hi - lo)
          ++ro[v];
        else
          box[t][std::upper_bound(vs.begin(), vs.end(), v) - vs.begin() - 1]
              .push_back({v, u | (x & ~HM), es[t] + j});
      }
    }
    for (auto [j, w] : r.esc)
      esc[t].push_back({es[t] + j, w});
    r = {};
  });
  merge(f.esc, esc);
  for (auto &[s, t] : qs)
    s = id[s], t = id[t];
  id.reset();
  reverse(f, rv, vs, box);
  return f;
}

// One search direction: a ring of distance buckets (far jumps wait in a heap)
// and distances in pages materialized on first write, recycled per query.
struct Side {
  static constexpr unsigned SH = 6, NB = 1024, PS = 7, PG = 1u << PS, BS = 62;
  const Csr &g;
  std::vector<u64 *> dir;
  std::vector<u32> used;
  Buf<u64> pool;
  u64 blank[PG];
  // Bucket entries live in blocks chained per bucket and recycled through a
  // free list, so memory follows the live queue, not each bucket's record.
  struct Blk {
    u32 next, n, v[BS];
  };
  std::vector<Blk> blk;
  u32 head[NB], tail[NB], free = ~0u;
  std::priority_queue<std::pair<u64, u32>, std::vector<std::pair<u64, u32>>,
                      std::greater<>>
      far;
  u64 cur = 0;
  size_t near = 0;
  Side(const Csr &c, u32 n)
      : g(c), dir((n >> PS) + 1), pool(buf<u64>(dir.size() * PG)) {
    std::fill(blank, blank + PG, INF);
    std::fill(dir.begin(), dir.end(), +blank);
    clear();
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
  // Writes the entry either way and counts it only when `take`, so a
  // relaxation needs no unpredictable branch.
  void put(u32 i, u32 v, bool take = true) {
    u32 t = tail[i];
    if (t == ~0u || blk[t].n == BS) {
      u32 b = free;
      if (b != ~0u)
        free = blk[b].next;
      else
        b = u32(blk.size()), blk.emplace_back();
      blk[b].next = ~0u, blk[b].n = 0;
      (t == ~0u ? head[i] : blk[t].next) = b;
      tail[i] = t = b;
    }
    blk[t].v[blk[t].n] = v, blk[t].n += take, near += take;
  }
  void push(u64 d, u32 v, bool take = true) {
    if ((d >> SH) - cur < NB)
      put((d >> SH) & (NB - 1), v, take);
    else if (take)
      far.push({d, v});
  }
  void clear() {
    for (u32 pg : used)
      dir[pg] = blank;
    used.clear(), blk.clear();
    std::fill(head, head + NB, ~0u), std::fill(tail, tail + NB, ~0u);
    free = ~0u, far = {}, cur = near = 0;
  }
  void start(u32 s) { clear(), at(s) = 0, push(0, s); }
  // Moves to the first nonempty bucket; returns its smallest possible key.
  u64 lower() {
    u64 fb = far.empty() ? ~u64(0) : far.top().first >> SH;
    if (!near && far.empty())
      return INF;
    if (!near)
      cur = fb;
    while (cur < fb && head[cur & (NB - 1)] == ~0u)
      ++cur;
    for (; !far.empty() && (far.top().first >> SH) < cur + NB; far.pop())
      put((far.top().first >> SH) & (NB - 1), far.top().second);
    return cur << SH;
  }
  // Settles the current bucket (label-correcting inside it); returns the count.
  // Entries pushed into it meanwhile land in its last block and are seen too.
  size_t drain(const Side &o, u64 &best) {
    u32 i = cur & (NB - 1);
    size_t done = 0;
    // Locals, so the label and queue stores cannot alias them.
    const u32 *__restrict off = g.off.get(), *__restrict adj = g.adj.get();
    u64 bst = best;
    for (u32 x = head[i]; x != ~0u; x = blk[x].next)
      for (u32 k = 0; k < blk[x].n; ++k) {
        const u32 *b = blk[x].v;
        if (k + 8 < blk[x].n)
          __builtin_prefetch(&off[b[k + 8]]),
              __builtin_prefetch(&adj[off[b[k + 4]]]),
              __builtin_prefetch(&get(b[k + 4]));
        u32 u = b[k];
        u64 d = get(u);
        --near;
        if (d >> SH != cur)
          continue; // stale: u has since moved to an earlier bucket
        ++done;
        for (u32 e = off[u], end = off[u + 1]; e < end; ++e) {
          u32 y = adj[e], v = y & HM;
          u64 nd = d + weight(y, e, g.esc), &dv = at(v), old = dv;
          dv = std::min(nd, old), push(nd, v, nd < old);
          bst = std::min(bst, nd + o.get(v));
        }
      }
    best = bst;
    blk[tail[i]].next = free, free = head[i], head[i] = tail[i] = ~0u;
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
  std::string data = argc > 2 ? argv[1] : "/data",
              out = argc > 2 ? argv[2] : "/out";
  NT = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
  gate = new std::barrier<>(NT);
  for (unsigned t = 1; t < NT; ++t)
    std::thread([t] {
      for (;;)
        gate->arrive_and_wait(), job(t), gate->arrive_and_wait();
    }).detach();
  size_t len;
  std::vector<std::pair<u32, u32>> qs;
  for (const char *q = map(data + "/queries.txt", len), *e = q + len; q < e;) {
    u32 s = num(q);
    qs.push_back({s, num(q)});
  }
  u32 n;
  std::vector<u64> bypass;
  Csr rev, fwd = read_graph(data + "/graph.txt", n, qs, bypass);
  fwd = reorder(std::move(fwd), n, qs, bypass, rev);
  std::vector<u32> order(qs.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](u32 a, u32 b) { return qs[a] < qs[b]; });
  std::vector<int64_t> ans(qs.size());
  std::atomic<size_t> next{0};
  par([&](unsigned) {
    // Never freed: unmapping while other threads search would stall them.
    Solver &solver = *new Solver(fwd, rev, n);
    for (size_t k; (k = next++) < qs.size();)
      ans[order[k]] = solver.solve(qs[order[k]].first, qs[order[k]].second);
  });
  FILE *file = fopen((out + "/distances.txt").c_str(), "wb");
  for (int64_t a : ans)
    if (!file || fprintf(file, "%lld\n", (long long)a) < 0)
      return 1;
  if (fclose(file))
    return 1;
  _exit(0);
}
