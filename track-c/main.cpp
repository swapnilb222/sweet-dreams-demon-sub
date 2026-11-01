// Speed Demon 2026, Track C (Join): multi-threaded C++ solution.
//
//   /app/solution [data_dir] [out_dir]      (defaults: /data /out)
//
// 1. regions.csv -> dense array region_id -> group index.
// 2. records.csv is mmapped and cut into chunks at safe record starts: an LF
//    followed by a digit or a BOM. Per spec 2.2 an LF inside a quoted field is
//    always followed by [a-z"] (names never hold an LF, note lines start with
//    a lowercase word or a quote), and BOMs only appear at record starts.
// 3. Worker threads pull chunks and parse them with a hand-written scanner.
//    Numeric aggregates go to per-thread per-group accumulators. Each name is
//    interned in one shared lock-free hash table, keyed on its raw (still
//    escaped) bytes, which map one-to-one onto the unescaped value; the empty
//    unquoted and empty quoted names both give the empty key. The name's id is
//    appended to a per-thread, per-group stream.
// 4. Per group (in parallel): OR the id streams into a small bitset and count.
// 5. Sort group keys, print.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

[[noreturn]] static void die(const char* msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

struct Mapped {
    const u8* p = nullptr;
    size_t n = 0;
};

static Mapped map_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) die(("cannot open " + path).c_str());
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat");
    Mapped m;
    m.n = (size_t)st.st_size;
    if (m.n) {
        void* p = mmap(nullptr, m.n, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) die("mmap");
        m.p = (const u8*)p;
    }
    close(fd);
    return m;
}

static void* reserve(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) die("mmap anon");
    return p;
}

// ----------------------------------------------------------------- hashing
// Reads up to 7 bytes past p+n. Safe here: a name is always followed by at
// least `",x,1,0.00` worth of bytes inside the mapping.
static inline u64 hash_bytes(const u8* p, u32 n) {
    u64 h = 0x9E3779B97F4A7C15ull * (n + 1);
    while (n >= 8) {
        u64 w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
        h = (h << 31) | (h >> 33);
        p += 8;
        n -= 8;
    }
    if (n) {
        u64 w;
        memcpy(&w, p, 8);
        w &= ~0ull >> (64 - 8 * n);
        h = (h ^ w) * 0x94d049bb133111ebull;
    }
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ull;
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ull;
    h ^= h >> 32;
    return h;
}

// --------------------------------------------------- shared name table
// Slot: high 32 bits = hash tag, low 32 bits = arena offset / 8 + 1.
// 0 = empty, BUSY = being written. Arena entry: [u32 id][u32 len][bytes].
struct NameTable {
    static constexpr u64 BUSY = ~0ull;
    std::atomic<u64>* slots = nullptr;
    u64 mask = 0;
    u64 limit = 0;
    u8* arena = nullptr;
    std::atomic<u64> arena_top{0};
    std::atomic<u32> next_id{0};
    std::atomic<bool> full{false};

    void init(u64 cap, size_t arena_bytes) {
        slots = (std::atomic<u64>*)reserve(cap * sizeof(u64));
        mask = cap - 1;
        limit = cap / 4 * 3;
        arena = (u8*)reserve(arena_bytes);
    }

    // Returns the name's dense id. On overflow sets `full` and returns 0; the
    // caller then reruns everything with a bigger table.
    inline void prefetch_slot(u64 h) const { __builtin_prefetch(&slots[h & mask]); }

    // Stage two of the pipeline: the slot is (probably) cached by now; pull in
    // the arena entry it points at so the byte compare does not stall.
    inline void prefetch_entry(u64 h) const {
        u64 e = slots[h & mask].load(std::memory_order_relaxed);
        if (e != 0 && e != BUSY) {
            const u8* a = arena + ((e & 0xFFFFFFFFull) - 1) * 8;
            __builtin_prefetch(a);
            __builtin_prefetch(a + 40);
        }
    }

    inline u32 intern(const u8* p, u32 len, u64 h) {
        u64 tag = h & 0xFFFFFFFF00000000ull;
        u64 s = h & mask;
        for (;;) {
            u64 e = slots[s].load(std::memory_order_acquire);
            if (e == 0) {
                if (slots[s].compare_exchange_strong(e, BUSY, std::memory_order_acquire)) {
                    u32 id = next_id.fetch_add(1, std::memory_order_relaxed);
                    if (id >= limit) {
                        full.store(true, std::memory_order_relaxed);
                        slots[s].store(0, std::memory_order_release);
                        return 0;
                    }
                    u64 off = arena_top.fetch_add((8 + len + 7) & ~7ull, std::memory_order_relaxed);
                    u8* a = arena + off;
                    memcpy(a, &id, 4);
                    memcpy(a + 4, &len, 4);
                    memcpy(a + 8, p, len);
                    slots[s].store(tag | (off / 8 + 1), std::memory_order_release);
                    return id;
                }
                continue;  // lost the race: re-read this slot
            }
            if (e == BUSY) continue;
            if ((e & 0xFFFFFFFF00000000ull) == tag) {
                const u8* a = arena + ((e & 0xFFFFFFFFull) - 1) * 8;
                u32 alen;
                memcpy(&alen, a + 4, 4);
                if (alen == len && memcmp(a + 8, p, len) == 0) {
                    u32 id;
                    memcpy(&id, a, 4);
                    return id;
                }
            }
            s = (s + 1) & mask;
        }
    }
};

// ------------------------------------------------------------- aggregates
struct Agg {
    i64 count = 0, id_sum = 0, amt_sum = 0;
    i64 amt_min = INT64_MAX, amt_max = INT64_MIN;
    i64 name_bytes = 0, note_bytes = 0;
};

struct Local {
    std::vector<Agg> agg;
    std::vector<std::vector<u32>> ids;  // [group] -> name ids seen
    void init(size_t groups) {
        agg.assign(groups, Agg{});
        ids.assign(groups, {});
    }
};

// ---------------------------------------------------------------- parsing
static inline bool is_digit(u8 c) { return (u8)(c - '0') < 10; }

// Scans a text field at p (quoted or unquoted) and returns the pointer just
// past it (at the following ','). raw/rawlen = bytes between the quotes,
// still escaped; esc = number of "" pairs inside.
static inline const u8* scan_text(const u8* p, const u8* end, const u8*& raw, u32& rawlen, u32& esc) {
    esc = 0;
    if (*p == '"') {
        ++p;
        raw = p;
        for (;;) {
            const u8* q = (const u8*)memchr(p, '"', (size_t)(end - p));
            if (!q) die("unterminated quoted field");
            if (q + 1 < end && q[1] == '"') {
                ++esc;
                p = q + 2;
                continue;
            }
            rawlen = (u32)(q - raw);
            return q + 1;
        }
    }
    raw = p;
    while (*p != ',') ++p;
    rawlen = (u32)(p - raw);
    return p;
}

struct Ctx {
    const u8* data;
    size_t n;
    const u32* region_group;
    u64 region_size;
    u32 unmatched;
    NameTable* names;
};

// Name lookups are software-pipelined over a ring of recent records: record i
// prefetches its table slot, record i-D prefetches the arena entry that slot
// points at, and record i-2D does the actual lookup, by then from cache.
static constexpr u32 PIPE_D = 8;
static constexpr u32 RING = 2 * PIPE_D;  // power of two
struct Pending {
    const u8* p;
    u64 h;
    u32 len;
    u32 g;
};

static void parse_chunk(const Ctx& c, Local& L, const u8* p, const u8* stop) {
    const u8* end = c.data + c.n;
    NameTable& nt = *c.names;
    Pending ring[RING];
    u64 k = 0;  // records parsed in this chunk
    while (p < stop) {
        if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
        // id
        u64 id = 0;
        while (*p != ',') id = id * 10 + (u64)(*p++ - '0');
        ++p;
        // name
        const u8* nraw;
        u32 nlen, nesc;
        p = scan_text(p, end, nraw, nlen, nesc);
        ++p;
        // note
        const u8* oraw;
        u32 olen, oesc;
        p = scan_text(p, end, oraw, olen, oesc);
        ++p;
        // region_id
        u64 rid = 0;
        u32 rdigits = 0;
        while (*p != ',') {
            rid = rid * 10 + (u64)(*p++ - '0');
            ++rdigits;
        }
        ++p;
        // amount
        bool neg = *p == '-';
        p += neg;
        i64 v = 0;
        while (*p != '.') v = v * 10 + (*p++ - '0');
        v = v * 100 + (p[1] - '0') * 10 + (p[2] - '0');
        p += 3;
        if (neg) v = -v;
        // terminator
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;

        u32 g = (rdigits <= 18 && rid < c.region_size) ? c.region_group[rid] : c.unmatched;
        Agg& a = L.agg[g];
        a.count++;
        a.id_sum += (i64)id;
        a.amt_sum += v;
        if (v < a.amt_min) a.amt_min = v;
        if (v > a.amt_max) a.amt_max = v;
        a.name_bytes += nlen - nesc;
        a.note_bytes += olen - oesc;

        u64 h = hash_bytes(nraw, nlen);
        nt.prefetch_slot(h);
        if (k >= RING) {  // retire record k-RING, whose slot ring[k%RING] we reuse
            const Pending& r = ring[k % RING];
            L.ids[r.g].push_back(nt.intern(r.p, r.len, r.h));
        }
        ring[k % RING] = {nraw, h, nlen, g};
        if (k >= PIPE_D) nt.prefetch_entry(ring[(k - PIPE_D) % RING].h);
        ++k;
    }
    for (u64 j = k > RING ? k - RING : 0; j < k; j++) {
        const Pending& r = ring[j % RING];
        L.ids[r.g].push_back(nt.intern(r.p, r.len, r.h));
    }
}

// ------------------------------------------------------------------ output
static void put_money(std::string& s, i64 c) {
    char buf[32];
    u64 a = c < 0 ? (u64)(-c) : (u64)c;
    int n = snprintf(buf, sizeof buf, "%s%llu.%02llu", c < 0 ? "-" : "", (unsigned long long)(a / 100),
                     (unsigned long long)(a % 100));
    s.append(buf, n);
}

static void put_int(std::string& s, i64 v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    s.append(buf, n);
}

template <class F>
static void run_parallel(int T, F&& f) {
    std::vector<std::thread> th;
    th.reserve(T);
    for (int t = 0; t < T; t++) th.emplace_back([&f, t] { f(t); });
    for (auto& x : th) x.join();
}

int main(int argc, char** argv) {
    std::string data_dir = argc > 1 ? argv[1] : "/data";
    std::string out_dir = argc > 2 ? argv[2] : "/out";

    // ---- regions.csv
    Mapped rg = map_file(data_dir + "/regions.csv");
    std::vector<std::string> keys;  // group index -> key
    std::vector<int> code_group(26 * 26 * 26, -1);
    std::vector<std::pair<u64, u32>> rows;
    rows.reserve(rg.n / 10 + 1);
    u64 max_rid = 0;
    {
        const u8* p = rg.p;
        const u8* e = rg.p + rg.n;
        if (e - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
        while (p < e && *p != '\n') ++p;  // header
        if (p < e) ++p;
        while (p < e) {
            u64 rid = 0;
            while (p < e && *p != ',') rid = rid * 10 + (u64)(*p++ - '0');
            ++p;
            const u8* k = p;
            while (p < e && *p != '\n' && *p != '\r') ++p;
            if (p - k != 3) die("unexpected country code");
            int code = (k[0] - 'A') * 676 + (k[1] - 'A') * 26 + (k[2] - 'A');
            if (code < 0 || code >= 26 * 26 * 26) die("bad country code");
            if (code_group[code] < 0) {
                code_group[code] = (int)keys.size();
                keys.emplace_back((const char*)k, 3);
            }
            rows.emplace_back(rid, (u32)code_group[code]);
            if (rid > max_rid) max_rid = rid;
            while (p < e && (*p == '\r' || *p == '\n')) ++p;
        }
    }
    const u32 unmatched = (u32)keys.size();
    keys.emplace_back("_UNMATCHED");
    const size_t G = keys.size();
    std::vector<u32> region_group(max_rid + 1, unmatched);
    for (auto& r : rows) region_group[r.first] = r.second;
    rows.clear();
    rows.shrink_to_fit();

    // ---- records.csv
    Mapped rc = map_file(data_dir + "/records.csv");
    const u8* d = rc.p;
    const size_t n = rc.n;
    size_t hs = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) hs = 3;
    while (hs < n && d[hs] != '\n') ++hs;
    if (hs < n) ++hs;

    int T = (int)std::thread::hardware_concurrency();
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0) T = CPU_COUNT(&cs);  // respects cpusets
    if (T < 1) T = 1;
    if (T > 64) T = 64;

    // chunk boundaries at safe record starts
    std::vector<size_t> chunks;
    {
        size_t body = n - hs;
        size_t nch = std::max<size_t>(1, std::min<size_t>((size_t)T * 8, body / (1 << 18)));
        chunks.push_back(hs);
        for (size_t i = 1; i < nch; i++) {
            size_t q = std::max(hs + body * i / nch, chunks.back());
            for (;;) {
                const u8* lf = (const u8*)memchr(d + q, '\n', n - q);
                if (!lf) {
                    q = n;
                    break;
                }
                q = (size_t)(lf - d) + 1;
                if (q >= n || is_digit(d[q]) || d[q] == 0xEF) break;
            }
            chunks.push_back(q);
        }
        chunks.push_back(n);
    }
    const size_t nch = chunks.size() - 1;
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);

    // Name table sized for the generator's ~100k-name pool; a dataset with more
    // distinct names reruns with a larger table (never needed on spec data).
#ifndef INIT_CAP_LOG
#define INIT_CAP_LOG 18
#endif
    u64 cap = 1ull << INIT_CAP_LOG;
    while (INIT_CAP_LOG == 18 && cap < n / 400) cap <<= 1;
    std::vector<Local> locals(T);
    NameTable* nt = nullptr;
    for (;;) {
        nt = new NameTable();
        nt->init(cap, n + (n / 11 + 1) * 16 + 64);
        Ctx ctx{d, n, region_group.data(), (u64)region_group.size(), unmatched, nt};
        std::atomic<size_t> next_chunk{0};
        run_parallel(T, [&](int t) {
            Local& L = locals[t];
            L.init(G);
            for (;;) {
                if (nt->full.load(std::memory_order_relaxed)) break;
                size_t i = next_chunk.fetch_add(1, std::memory_order_relaxed);
                if (i >= nch) break;
                parse_chunk(ctx, L, d + chunks[i], d + chunks[i + 1]);
                // Release the chunk's file pages from our RSS.
                size_t a = (chunks[i] + page - 1) / page * page;
                size_t b = chunks[i + 1] / page * page;
                if (b > a + page) madvise((void*)(d + a), b - a - page, MADV_DONTNEED);
            }
        });
        if (!nt->full.load()) break;
        cap <<= 3;  // leak the old table; this path is exceptional
    }
    const u32 total_names = nt->next_id.load();

    // ---- per-group merge: numeric sums and distinct names
    std::vector<Agg> agg(G);
    std::vector<i64> distinct(G, 0);
    {
        std::atomic<size_t> next{0};
        run_parallel(T, [&](int) {
            std::vector<u64> bs(total_names / 64 + 1);
            for (;;) {
                size_t g = next.fetch_add(1);
                if (g >= G) break;
                Agg a;
                for (auto& L : locals) {
                    const Agg& b = L.agg[g];
                    a.count += b.count;
                    a.id_sum += b.id_sum;
                    a.amt_sum += b.amt_sum;
                    a.amt_min = std::min(a.amt_min, b.amt_min);
                    a.amt_max = std::max(a.amt_max, b.amt_max);
                    a.name_bytes += b.name_bytes;
                    a.note_bytes += b.note_bytes;
                }
                agg[g] = a;
                if (!a.count) continue;
                std::fill(bs.begin(), bs.end(), 0);
                i64 cnt = 0;
                for (auto& L : locals) {
                    for (u32 id : L.ids[g]) {
                        u64 m = 1ull << (id & 63);
                        u64& w = bs[id >> 6];
                        cnt += !(w & m);
                        w |= m;
                    }
                }
                distinct[g] = cnt;
            }
        });
    }

    // ---- output
    std::vector<u32> order;
    for (u32 g = 0; g < G; g++)
        if (agg[g].count) order.push_back(g);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return keys[a] < keys[b]; });
    std::string out;
    out.reserve(order.size() * 96);
    for (u32 g : order) {
        const Agg& a = agg[g];
        out += keys[g];
        out += ' ';
        put_int(out, a.count);
        out += ' ';
        put_int(out, a.id_sum);
        out += ' ';
        put_money(out, a.amt_sum);
        out += ' ';
        put_money(out, a.amt_min);
        out += ' ';
        put_money(out, a.amt_max);
        out += ' ';
        put_int(out, distinct[g]);
        out += ' ';
        put_int(out, a.name_bytes);
        out += ' ';
        put_int(out, a.note_bytes);
        out += '\n';
    }
    std::string path = out_dir + "/aggregates.txt";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) die("cannot open output");
    if (fwrite(out.data(), 1, out.size(), f) != out.size()) die("write failed");
    if (fclose(f) != 0) die("close failed");
    _exit(0);  // skip destructors of large containers
}
