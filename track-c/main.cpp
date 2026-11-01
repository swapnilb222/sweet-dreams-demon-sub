// Speed Demon 2026, Track C (Join).
//
// One pool of threads runs three phases; each is cut into pieces that any
// thread claims, and the next starts once every piece of the last is done.
//  1. regions.csv, split by lines: flag the country codes present and the
//     largest region id; fault in the shared tables. Countries are numbered
//     in code order, so the output needs no sort.
//  2. Fill a direct table region_id -> country number (0 = no such region).
//  3. records.csv, in ~1 MB pieces. Each piece's thread finds its own bounds:
//     the first record start (an LF followed by a digit or a BOM; per spec 2.2
//     an LF inside quotes is always followed by [a-z"]) at or after its share
//     of the file. Fields are found with AVX2 masks, numbers decoded with SWAR.
//     A name is keyed by a 64-bit hash of its raw bytes between the quotes
//     (escaping is one to one, so equal bytes mean equal values, and both empty
//     forms are the empty key); its slot in a lock-free open-addressed table is
//     its id. Each country has a bitset over ids; an atomic OR that sets a new
//     bit is a new distinct name, so no merge pass is needed. The region row,
//     the table slot and the bitset word are prefetched by a 3-stage pipeline.
//     Pages of a parsed piece are released at once, keeping RSS small.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

[[noreturn]] static void die(const char* m) {
    fprintf(stderr, "track-c: %s\n", m);
    _exit(1);
}

// Zero-filled anonymous memory; pages are charged only once touched.
template <class T>
static T* zeroed(size_t n) {
    void* p = mmap(nullptr, n * sizeof(T) + 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1, 0);
    if (p == MAP_FAILED) die("mmap");
    return static_cast<T*>(p);
}

// Fault in share t of n of a table up front, one call per thread, instead of
// page by page from every thread at once (page-table lock contention).
static void populate_share(void* p, size_t bytes, int t, int n) {
    size_t pages = (bytes + 4095) / 4096, from = pages * t / n, to = pages * (t + 1) / n;
    if (to > from) madvise(static_cast<char*>(p) + from * 4096, (to - from) * 4096, MADV_POPULATE_WRITE);
}

// Map a file followed by a spare zero page, so 64-byte loads past its end
// are safe.
static const char* map_file(const std::string& path, size_t& size) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) die(path.c_str());
    size = lseek(fd, 0, SEEK_END);
    auto* p = static_cast<char*>(mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (p == MAP_FAILED || (size && mmap(p, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED))
        die("mmap");
    close(fd);
    return p;
}

static inline u64 load8(const char* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

// Bit i set when byte i of the 32 (64) at p equals c.
static inline u32 match32(const char* p, char c) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}
static inline u64 match64(const char* p, char c) { return match32(p, c) | u64(match32(p + 32, c)) << 32; }

// The first c at or after p (one is known to follow).
[[gnu::always_inline]] static inline const char* find(const char* p, char c) {
    for (;; p += 64)
        if (u64 m = match64(p, c)) return p + __builtin_ctzll(m);
}

// Value of the last n (1..8) digit bytes of the little-endian word v.
static inline u64 fold(u64 v, size_t n) {
    v &= 0x0F0F0F0F0F0F0F0Full & ~u64(0) << 8 * (8 - n);
    v = (v * 10 + (v >> 8)) & 0x00FF00FF00FF00FFull;
    v = (v * 100 + (v >> 16)) & 0x0000FFFF0000FFFFull;
    return (v * 10000 + (v >> 32)) & 0xFFFFFFFF;
}

// Decimal digits [s, e); at least 8 mapped bytes precede e.
static inline u64 digits(const char* s, const char* e) {
    if (__builtin_expect(e - s <= 8, 1)) return fold(load8(e - 8), e - s);
    u64 v = 0;
    for (; s < e; ++s) v = v * 10 + u64(*s - '0');
    return v;
}

// Cents of an amount whose digits are [s, e), the dot third from the end:
// the 8 bytes ending at e, with the dot's byte squeezed out, are one number.
static inline u64 money(const char* s, const char* e) {
    if (__builtin_expect(e - s <= 9, 1)) {
        u64 w = load8(e - 8);
        return fold((w & 0xFFFFFFFFFFull) << 8 | (w & 0xFFFF000000000000ull), e - s - 1);
    }
    return digits(s, e - 3) * 100 + u64(e[-2] - '0') * 10 + u64(e[-1] - '0');
}

// A text field at p; moves p past its comma. at/raw = bytes between the
// quotes (still escaped), length = value length (one less per "" pair).
struct Text {
    const char* at;
    u32 raw, length;
};

[[gnu::always_inline]] static inline Text text(const char*& p) {
    const char* s = p;
    if (*s != '"') {
        p = find(s, ',') + 1;
        return {s, u32(p - 1 - s), u32(p - 1 - s)};
    }
    // Step through the quotes after the opening one on their bit mask: a
    // quote followed by another is a "" pair; the first lone one closes.
    u32 pairs = 0;
    const char* q = s + 1;
    for (u64 m = match64(q, '"');;) {
        u32 i = m ? __builtin_ctzll(m) : 64;
        if (i >= 63) {  // none, or its successor is out of view
            q += i;
            m = match64(q, '"');
            continue;
        }
        if (!(m >> (i + 1) & 1)) {
            q += i;
            break;
        }
        ++pairs;
        m &= m - 1;
        m &= m - 1;
    }
    p = q + 2;
    return {s + 1, u32(q - s - 1), u32(q - s - 1 - pairs)};
}

alignas(64) static const char KEEP[64] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

// 64-bit key of a name's raw bytes and length (never 0, which marks a free
// slot). Among ~100k names a shared key has a chance of about 3 in 10^10.
static inline u64 name_key(const char* s, u32 len) {
    __m128i h16 = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s)),
                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(KEEP + 32 - std::min(len, 16u))));
    u64 h = (u64(_mm_cvtsi128_si64(h16)) * 0x9E3779B97F4A7C15ull ^ len) +
            u64(_mm_extract_epi64(h16, 1)) * 0xBF58476D1CE4E5B9ull;
    for (u32 i = 16; i < len; i += 8) {
        u64 w = load8(s + i);
        if (len - i < 8) w &= (u64(1) << 8 * (len - i)) - 1;
        h = (h ^ w) * 0x94D049BB133111EBull;
    }
    h = (h ^ h >> 31) * 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h + (h == 0);
}

// ------------------------------------------------------------- name table
// Open addressing over aligned groups of four 8-byte slots (one 32-byte
// compare per group). A key's slot index is the name's id. Free slots are
// claimed with a CAS; slots are never freed, so no key sits past a free one.
struct Names {
    std::atomic<u64>* slots;
    std::atomic<u64>* seen;  // [group][id / 64]
    u32 bits, words;
    u64 limit;
    alignas(64) std::atomic<u64> used{0};  // written only on inserts
    alignas(64) std::atomic<bool> full{false};

    explicit Names(u32 b) : bits(b), words((1u << b) / 64), limit((u64(1) << b) / 8 * 7) {
        slots = zeroed<std::atomic<u64>>(size_t(1) << b);
        seen = zeroed<std::atomic<u64>>(size_t(256) * words);
    }
};

// The id of key k, inserting it if absent (UINT32_MAX when the table is full).
static inline u32 name_id(std::atomic<u64>* slots, u32 bits, u64 k, Names& nm) {
    const u32 mask = (1u << bits) - 1;
    const __m256i want = _mm256_set1_epi64x(i64(k));
    for (u32 i = u32(k >> (64 - bits)) & ~3u;; i = (i + 4) & mask) {
        __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(&slots[i]));
        u32 hit = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, want)));
        if (__builtin_expect(hit != 0, 1)) return i + __builtin_ctz(hit);
        u32 free = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, _mm256_setzero_si256())));
        // Past the limit the table may fill up completely: give up rather
        // than probe forever (the run is redone with a larger table).
        if (!free && nm.full.load(std::memory_order_relaxed)) return UINT32_MAX;
        for (; free; free &= free - 1) {
            u32 j = i + __builtin_ctz(free);
            u64 cur = 0;
            if (slots[j].compare_exchange_strong(cur, k)) {
                if (nm.used.fetch_add(1, std::memory_order_relaxed) >= nm.limit) {
                    nm.full.store(true, std::memory_order_relaxed);
                    return UINT32_MAX;
                }
                return j;
            }
            if (cur == k) return j;
        }
    }
}

// ------------------------------------------------------------- aggregates
struct alignas(64) Totals {
    u64 count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0, distinct = 0;
    i64 amount_sum = 0, amount_min = INT64_MAX, amount_max = INT64_MIN;
};
static constexpr u32 UNMATCHED = 255;  // group of records with no region

// Parse every record that starts in [p, end). Record n prefetches its region
// row and name slot; at n+LAG its totals are settled, its name gets its id
// and the bitset word is prefetched for writing; at n+2*LAG it is marked.
static void parse_records(const char* p, const char* end, const char* file_end, const u8* country_of, size_t nreg,
                          Names& nm, Totals* totals) {
    constexpr u64 LAG = 16;
    std::atomic<u64>* const slots = nm.slots;
    std::atomic<u64>* const seen = nm.seen;
    const u32 bits = nm.bits, words = nm.words;
    struct Pending {
        u64 key;
        u32 region, id, name_bytes, note_bytes;
        i64 cents;
    } pending[LAG];
    struct Mark {
        u32 group, id;
    } marks[LAG];
    u64 n = 0, last = UINT64_MAX;
    auto advance = [&]() __attribute__((always_inline)) {
        u32 k = n % LAG;
        if (n >= 2 * LAG && n - 2 * LAG < last && marks[k].id != UINT32_MAX) {
            u32 g = marks[k].group, id = marks[k].id;
            u64 bit = u64(1) << (id & 63);
            totals[g].distinct += !(seen[size_t(g) * words + id / 64].fetch_or(bit, std::memory_order_relaxed) & bit);
        }
        if (n >= LAG && n - LAG < last) {
            const Pending& o = pending[k];
            u32 g = u8(country_of[o.region] - 1);  // 0 (no region) -> 255
            Totals& t = totals[g];
            ++t.count;
            t.id_sum += o.id;
            t.amount_sum += o.cents;
            t.amount_min = std::min(t.amount_min, o.cents);
            t.amount_max = std::max(t.amount_max, o.cents);
            t.name_bytes += o.name_bytes;
            t.note_bytes += o.note_bytes;
            u32 id = name_id(slots, bits, o.key, nm);
            marks[k] = {g, id};
            __builtin_prefetch(&seen[size_t(g) * words + (id & ((1u << bits) - 1)) / 64], 1);
        }
    };

    while (p < end) {
        _mm_prefetch(p + 2048, _MM_HINT_T0);
        if (u8(*p) == 0xEF) p += 3;  // BOM before the id
        const char* f1 = p + __builtin_ctz(match32(p, ','));
        u64 id = digits(p, f1);
        p = f1 + 1;
        Text name = text(p), note = text(p);
        // region (<= 7 digits) and amount (<= 9 bytes) end within 32 bytes.
        u32 lf = match32(p, '\n');
        const char* f4 = p + __builtin_ctz(match32(p, ','));
        u64 region = digits(p, f4);
        const char* amount = f4 + 1;
        const char* eol = lf ? p + __builtin_ctz(lf) : static_cast<const char*>(memchr(p, '\n', file_end - p));
        if (!eol) eol = file_end;
        const char* amount_end = eol - (eol[-1] == '\r');
        bool negative = *amount == '-';
        i64 cents = i64(money(amount + negative, amount_end));
        if (negative) cents = -cents;

        u32 r = region < nreg ? u32(region) : 0;  // row 0 is never a region
        _mm_prefetch(reinterpret_cast<const char*>(country_of + r), _MM_HINT_T0);
        advance();
        u64 key = name_key(name.at, name.raw);
        pending[n++ % LAG] = {key, r, u32(id), name.length, note.length, cents};
        _mm_prefetch(reinterpret_cast<const char*>(&slots[u32(key >> (64 - bits)) & ~3u]), _MM_HINT_T0);
        p = eol + 1;
    }
    for (last = n; n < last + 2 * LAG; ++n) advance();
}

// ------------------------------------------------------------- the pool
// A phase of work in pieces: every thread claims pieces until none are left,
// then waits until all are done.
struct Phase {
    std::atomic<int> claimed{0}, done{0};
    int pieces = 0;
    template <class F>
    void run(F&& f) {
        for (int k; (k = claimed.fetch_add(1)) < pieces;)
            if (f(k), done.fetch_add(1) + 1 == pieces) done.notify_all();
        for (int d; (d = done.load()) < pieces;) done.wait(d);
    }
};

// The first record start at or after share k of n of [from, size): piece k
// begins there and piece k - 1 ends there.
static size_t record_cut(const char* d, size_t from, size_t size, size_t k, size_t n) {
    if (k == 0) return from;
    if (k >= n) return size;
    for (size_t q = from + (size - from) * k / n;;) {
        const char* lf = static_cast<const char*>(memchr(d + q, '\n', size - q));
        if (!lf) return size;
        q = lf - d + 1;
        if (q >= size || u8(d[q] - '0') < 10 || u8(d[q]) == 0xEF) return q;
    }
}

static std::vector<size_t> split_lines(const char* d, size_t from, size_t size, size_t n) {
    std::vector<size_t> cut(n + 1, size);
    cut[0] = from;
    for (size_t t = 1; t < n; ++t) {
        size_t q = std::max(cut[t - 1], from + (size - from) * t / n);
        const char* lf = q < size ? static_cast<const char*>(memchr(d + q, '\n', size - q)) : nullptr;
        cut[t] = lf ? lf - d + 1 : size;
    }
    return cut;
}

static inline u32 code_of(const char* c) { return u32(c[0] - 'A') * 676 + u32(c[1] - 'A') * 26 + u32(c[2] - 'A'); }

static void put_money(std::string& out, i64 c) {
    u64 a = c < 0 ? -u64(c) : u64(c);
    out += c < 0 ? " -" : " ";
    out += std::to_string(a / 100);
    out += {'.', char('0' + a % 100 / 10), char('0' + a % 10)};
}

int main(int argc, char** argv) {
    std::string data_dir = argc > 1 ? argv[1] : "/data", out_dir = argc > 2 ? argv[2] : "/out";
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);  // tables stay at 4 KB-page granularity
    int T = std::thread::hardware_concurrency();
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0) T = CPU_COUNT(&cs);
    T = std::clamp(T, 1, 64);

    size_t rsize, size;
    const char* regions = map_file(data_dir + "/regions.csv", rsize);
    const char* records = map_file(data_dir + "/records.csv", size);

    // regions.csv: a header, then "id,ABC" lines.
    const char* rh = static_cast<const char*>(memchr(regions, '\n', rsize));
    std::vector<size_t> rcut = split_lines(regions, rh ? rh - regions + 1 : rsize, rsize, T);
    std::vector<std::atomic<u8>> present(17576);
    std::atomic<u64> max_region{0};
    u8 index_of[17576] = {};
    std::vector<u32> codes;  // country number - 1 -> code
    u8* country_of = nullptr;
    size_t nreg = 0;
    std::once_flag numbered;

    // records.csv: skip the BOM and header line; ~1 MB pieces.
    const char* lf0 = static_cast<const char*>(memchr(records, '\n', size));
    const size_t start = lf0 ? lf0 - records + 1 : size;
    const size_t npieces = std::clamp<size_t>((size - start) >> 20, std::min<size_t>(T, (size - start) / 4096 + 1), 4096);

#ifndef NAME_BITS
#define NAME_BITS 17
#endif
    u32 bits = NAME_BITS;  // 1 MB table: room for 114k names (the generator's pool holds 100k)
    Names* names = new Names(bits);
    std::vector<Totals> totals(size_t(T) * 256);

    auto parse_piece = [&](int k, Names& nm, Totals* tot) {
        if (nm.full.load(std::memory_order_relaxed)) return;
        size_t b = record_cut(records, start, size, k, npieces), e = record_cut(records, start, size, k + 1, npieces);
        if (b >= e) return;
        uintptr_t from = uintptr_t(records + b) & ~uintptr_t(4095);
        madvise(reinterpret_cast<void*>(from), uintptr_t(records + e) - from, MADV_POPULATE_READ);
        parse_records(records + b, records + e, records + size, country_of, nreg, nm, tot);
        uintptr_t to = uintptr_t(records + e) & ~uintptr_t(4095);
        if (to > from) madvise(reinterpret_cast<void*>(from), to - from, MADV_DONTNEED);
    };
    Phase flag, fill, parse;
    flag.pieces = fill.pieces = T;
    parse.pieces = int(npieces);
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t)
        pool.emplace_back([&, t] {
            flag.run([&](int k) {
                populate_share(names->slots, (size_t(1) << bits) * 8, k, T);
                u64 mx = 0;
                for (const char *p = regions + rcut[k], *e = regions + rcut[k + 1]; p < e;) {
                    const char* comma = p + __builtin_ctz(match32(p, ','));
                    mx = std::max(mx, digits(p, comma));
                    present[code_of(comma + 1)].store(1, std::memory_order_relaxed);
                    p = static_cast<const char*>(memchr(comma, '\n', e - comma)) + 1;
                }
                for (u64 cur = max_region.load(); mx > cur && !max_region.compare_exchange_weak(cur, mx);) {}
            });
            std::call_once(numbered, [&] {
                for (u32 c = 0; c < 17576; ++c)
                    if (present[c].load()) codes.push_back(c), index_of[c] = u8(codes.size());
                if (codes.size() > 254) die("too many countries");
                nreg = max_region.load() + 1;
                country_of = zeroed<u8>(nreg);
            });
            fill.run([&](int k) {
                populate_share(country_of, nreg, k, T);
                for (u32 g = 0; g <= codes.size(); ++g)  // the countries' bitsets, then _UNMATCHED's
                    populate_share(&names->seen[size_t(g < codes.size() ? g : UNMATCHED) * names->words],
                                   size_t(names->words) * 8, k, T);
                for (const char *p = regions + rcut[k], *e = regions + rcut[k + 1]; p < e;) {
                    const char* comma = p + __builtin_ctz(match32(p, ','));
                    country_of[digits(p, comma)] = index_of[code_of(comma + 1)];
                    p = static_cast<const char*>(memchr(comma, '\n', e - comma)) + 1;
                }
            });
            parse.run([&](int k) { parse_piece(k, *names, &totals[size_t(t) * 256]); });
        });
    for (auto& th : pool) th.join();

    // More distinct names than room (never on spec data): rerun, 8x larger.
    while (names->full.load()) {
        if (bits >= 26) die("too many distinct names");
        bits += 3;
        names = new Names(bits);
        totals.assign(size_t(T) * 256, Totals{});
        Phase again;
        again.pieces = int(npieces);
        std::vector<std::thread> rerun;
        for (int t = 0; t < T; ++t)
            rerun.emplace_back([&, t] { again.run([&](int k) { parse_piece(k, *names, &totals[size_t(t) * 256]); }); });
        for (auto& th : rerun) th.join();
    }

    std::string out;
    for (u32 i = 0; i <= codes.size(); ++i) {
        u32 g = i < codes.size() ? i : UNMATCHED;
        Totals s;
        for (int t = 0; t < T; ++t) {
            const Totals& x = totals[size_t(t) * 256 + g];
            s.count += x.count;
            s.id_sum += x.id_sum;
            s.name_bytes += x.name_bytes;
            s.note_bytes += x.note_bytes;
            s.distinct += x.distinct;
            s.amount_sum += x.amount_sum;
            s.amount_min = std::min(s.amount_min, x.amount_min);
            s.amount_max = std::max(s.amount_max, x.amount_max);
        }
        if (!s.count) continue;
        if (g == UNMATCHED) {
            out += "_UNMATCHED";
        } else {
            u32 c = codes[i];
            out += {char('A' + c / 676), char('A' + c / 26 % 26), char('A' + c % 26)};
        }
        for (u64 v : {s.count, s.id_sum}) out += ' ' + std::to_string(v);
        for (i64 v : {s.amount_sum, s.amount_min, s.amount_max}) put_money(out, v);
        for (u64 v : {s.distinct, s.name_bytes, s.note_bytes}) out += ' ' + std::to_string(v);
        out += '\n';
    }
    FILE* f = fopen((out_dir + "/aggregates.txt").c_str(), "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size() || fclose(f)) die("cannot write output");
    _exit(0);
}
