#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using U32 = uint32_t;
using U64 = uint64_t;
using List = std::vector<U32>;

// Every hash hit is checked against the complete token, including its length.
struct Dictionary {
  struct Slot {
    U64 hash = 0;
    std::string word;
    U32 id = 0;
  };
  std::vector<Slot> slots;
  std::array<U32, 37 * 37 * 37> short_words{};
  static U32 short_key(const char *p, size_t n) {
    U32 key = 0;
    for (size_t i = 0; i < n; ++i)
      key = key * 37 + (p[i] >= 'a' ? p[i] - 'a' + 1 : p[i] - '0' + 27);
    return key;
  }
  static U64 hash(const char *p, size_t n) {
    U64 h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i)
      h = (h ^ (unsigned char)p[i]) * 1099511628211ULL;
    return h;
  }
  explicit Dictionary(const std::unordered_map<std::string, U32> &words) {
    slots.resize(std::bit_ceil(words.size() * 2 + 1));
    for (const auto &[word, id] : words) {
      if (word.size() <= 3)
        short_words[short_key(word.data(), word.size())] = id;
      U64 h = hash(word.data(), word.size());
      size_t i = h & (slots.size() - 1);
      while (slots[i].id)
        i = (i + 1) & (slots.size() - 1);
      slots[i] = {h, word, id};
    }
  }
  U32 find(const char *p, size_t n, U64 h) const {
    if (n <= 3)
      return short_words[short_key(p, n)];
    size_t i = h & (slots.size() - 1);
    while (slots[i].id) {
      const auto &s = slots[i];
      if (s.hash == h && s.word.size() == n && !memcmp(s.word.data(), p, n))
        return s.id;
      i = (i + 1) & (slots.size() - 1);
    }
    return 0;
  }
};

// Edges are exact (state, token) pairs; zero denotes a missing transition.
struct Edges {
  struct Slot {
    U64 key = 0;
    U32 value = 0;
  };
  std::vector<Slot> slots;
  explicit Edges(size_t n) : slots(std::bit_ceil(n * 2 + 1)) {}
  size_t bucket(U64 key) const {
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    return (key ^ (key >> 31)) & (slots.size() - 1);
  }
  U32 &at(U64 key) {
    size_t i = bucket(key);
    while (slots[i].key && slots[i].key != key)
      i = (i + 1) & (slots.size() - 1);
    slots[i].key = key;
    return slots[i].value;
  }
  U32 find(U32 state, U32 token) const {
    U64 key = (U64(state) << 32) | token;
    size_t i = bucket(key);
    while (slots[i].key) {
      if (slots[i].key == key)
        return slots[i].value;
      i = (i + 1) & (slots.size() - 1);
    }
    return 0;
  }
};

struct Query {
  List terms;
  std::vector<char> ops;
  U32 phrase = 0;
};
struct Result {
  U64 count = 0, sum = 0;
  std::array<U32, 10> first{};
  U32 last = UINT32_MAX;
  void add(U32 id) {
    if (last == id)
      return;
    if (count < 10)
      first[count] = id;
    ++count;
    sum += id;
    last = id;
  }
  void merge(const Result &r) {
    std::array<U32, 20> ids;
    auto finish =
        std::merge(first.begin(), first.begin() + std::min<U64>(count, 10),
                   r.first.begin(),
                   r.first.begin() + std::min<U64>(r.count, 10), ids.begin());
    std::copy_n(ids.begin(), std::min<ptrdiff_t>(10, finish - ids.begin()),
                first.begin());
    count += r.count;
    sum += r.sum;
  }
};

// Sparse sorted IDs promote to a bitmap once the bitmap is smaller.
template <class Word> struct Posting {
  using Data = std::vector<Word>;
  static constexpr unsigned width = sizeof(Word) * 8;
  Data data;
  bool dense = false;
  U32 last = UINT32_MAX;
  bool contains(U32 id) const { return (data[id / width] >> (id % width)) & 1; }
  void densify(size_t words) {
    Data bits(words);
    for (U32 id : data)
      bits[id / width] |= U32(1) << (id % width);
    data.swap(bits);
    dense = true;
  }
  void add(U32 id, size_t words) {
    if (last == id)
      return;
    last = id;
    if (dense)
      data[id / width] |= U32(1) << (id % width);
    else {
      data.push_back(id);
      if (data.size() >= words)
        densify(words);
    }
  }
  void combine(const Posting &b, char op, size_t words, Data &scratch) {
    if (op == 'O' && !dense && b.dense)
      densify(words);
    if (dense && b.dense) {
      for (size_t i = 0; i < words; ++i) {
        if (op == 'A')
          data[i] &= b.data[i];
        if (op == 'O')
          data[i] |= b.data[i];
        if (op == 'N')
          data[i] &= ~b.data[i];
      }
    } else if (dense && op != 'A') {
      for (U32 id : b.data) {
        if (op == 'O')
          data[id / width] |= U32(1) << (id % width);
        else
          data[id / width] &= ~(U32(1) << (id % width));
      }
    } else if (dense || b.dense) {
      const Posting &bits = dense ? *this : b;
      const Data &ids = dense ? b.data : data;
      scratch.clear();
      for (U32 id : ids)
        if (bits.contains(id) == (op == 'A'))
          scratch.push_back(id);
      data.swap(scratch);
      dense = false;
    } else {
      scratch.resize(data.size() + b.data.size());
      auto finish = scratch.begin();
      if (op == 'A')
        finish = std::set_intersection(data.begin(), data.end(), b.data.begin(),
                                       b.data.end(), scratch.begin());
      if (op == 'O')
        finish = std::set_union(data.begin(), data.end(), b.data.begin(),
                                b.data.end(), scratch.begin());
      if (op == 'N')
        finish = std::set_difference(data.begin(), data.end(), b.data.begin(),
                                     b.data.end(), scratch.begin());
      scratch.resize(finish - scratch.begin());
      data.swap(scratch);
      if (data.size() >= words)
        densify(words);
    }
  }
};

int main(int argc, char **argv) {
  try {
    std::string data = argc > 1 ? argv[1] : "/data";
    std::string out = argc > 2 ? argv[2] : "/out";
    std::ifstream qfile(data + "/queries.txt");
    if (!qfile)
      throw std::runtime_error("cannot open queries.txt");
    std::unordered_map<std::string, U32> words;
    std::vector<Query> queries;
    List needed(1);
    U32 boolean_terms = 0;
    size_t edge_count = 0;
    for (std::string line; std::getline(qfile, line);) {
      bool phrase = line[0] == 'P';
      std::istringstream in(phrase ? line.substr(8, line.size() - 9)
                                   : line.substr(5));
      Query q;
      q.phrase = phrase;
      for (std::string word; in >> word;) {
        auto [it, fresh] = words.try_emplace(word, U32(words.size() + 1));
        if (fresh)
          needed.push_back(false);
        q.terms.push_back(it->second);
        if (!phrase) {
          if (!needed[it->second])
            needed[it->second] = ++boolean_terms;
          if (in >> word)
            q.ops.push_back(word[0]);
        }
      }
      edge_count += phrase ? q.terms.size() : 0;
      queries.push_back(std::move(q));
    }
    Dictionary dictionary(words);
    Edges edges(edge_count);
    List terminals(1), roots(words.size() + 1);
    U32 phrase_count = 0;
    for (auto &q : queries)
      if (q.phrase) {
        U32 state = 0;
        for (U32 term : q.terms) {
          U32 &next = edges.at((U64(state) << 32) | term);
          if (!next) {
            next = terminals.size();
            terminals.push_back(0);
          }
          if (!state)
            roots[term] = next;
          state = next;
        }
        if (!terminals[state])
          terminals[state] = ++phrase_count;
        q.phrase = terminals[state];
      }
    words.clear();
    words.rehash(0);
    auto path = data + "/docs.txt";
    U64 bytes = std::filesystem::file_size(path);
    unsigned workers = 8;
    if (const char *s = getenv("SD_THREADS"))
      workers = std::max(1, atoi(s));
    workers = std::min<U64>(workers, std::max<U64>(1, bytes / 131072));
    std::vector<U64> offsets(workers + 1);
    List bounds(workers + 1);
    std::ifstream boundary(path, std::ios::binary);
    for (unsigned w = 0; w < workers; ++w) {
      U64 pos = bytes * w / workers;
      boundary.seekg(pos ? pos - 1 : 0);
      std::string line;
      if (pos)
        std::getline(boundary, line);
      offsets[w] = boundary.tellg();
      std::getline(boundary, line);
      bounds[w] = std::stoul(line);
    }
    offsets[workers] = bytes;
    for (U64 length = std::min<U64>(bytes, 4096);;) {
      std::string tail(length, '\0');
      boundary.clear();
      boundary.seekg(bytes - length);
      boundary.read(tail.data(), length);
      auto pos = tail.rfind('\n', length - 2);
      if (pos != std::string::npos || length == bytes) {
        bounds[workers] =
            std::stoul(tail.substr(pos == std::string::npos ? 0 : pos + 1)) + 1;
        break;
      }
      length = std::min<U64>(bytes, length * 2);
    }
    std::vector<Result> answers(queries.size());
    std::mutex merge_mutex;
    std::vector<std::thread> threads;
    auto process = [&]<class Word>(unsigned w) {
      using P = Posting<Word>;
      std::ifstream docs(path, std::ios::binary);
      U64 begin = offsets[w], end = offsets[w + 1];
      std::string line;
      docs.seekg(begin);
      U64 offset = begin;
      size_t bitmap_words =
          (bounds[w + 1] - bounds[w] + P::width - 1) / P::width;
      std::vector<P> postings(boolean_terms + 1);
      std::vector<Result> phrases(phrase_count + 1);
      while (offset < end && std::getline(docs, line)) {
        offset += line.size() + 1;
        const unsigned char *p = (const unsigned char *)line.data();
        const unsigned char *limit = p + line.size();
        U32 doc = 0;
        while (*p != '\t')
          doc = doc * 10 + *p++ - '0';
        ++p;
        std::array<U32, 8> active{};
        unsigned count = 0;
        while (p < limit) {
          auto lower = [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') ? c + 32 : c;
          };
          auto token = [](unsigned c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
          };
          while (p < limit && !token(lower(*p)))
            ++p;
          if (p == limit)
            break;
          char buffer[32];
          size_t n = 0;
          U64 hash = 14695981039346656037ULL;
          while (p < limit && token(lower(*p))) {
            unsigned char c = lower(*p++);
            buffer[n++] = c;
            hash = (hash ^ c) * 1099511628211ULL;
          }
          U32 term = dictionary.find(buffer, n, hash);
          if (!term) {
            count = 0;
            continue;
          }
          if (needed[term]) {
            postings[needed[term]].add(doc - bounds[w], bitmap_words);
          }
          unsigned next_count = 0;
          for (unsigned i = 0; i < count; ++i) {
            U32 state = edges.find(active[i], term);
            if (state) {
              if (terminals[state])
                phrases[terminals[state]].add(doc);
              active[next_count++] = state;
            }
          }
          if (roots[term])
            active[next_count++] = roots[term];
          count = next_count;
        }
      }
      P current;
      typename P::Data scratch;
      std::array<Result, 64> batch;
      for (size_t qi = 0; qi < queries.size(); ++qi) {
        const auto &q = queries[qi];
        auto &result = batch[qi % batch.size()];
        result = {};
        if (q.phrase)
          result = phrases[q.phrase];
        else {
          current = postings[needed[q.terms[0]]];
          for (size_t i = 0; i < q.ops.size(); ++i)
            current.combine(postings[needed[q.terms[i + 1]]], q.ops[i],
                            bitmap_words, scratch);
          for (size_t i = 0; i < current.data.size(); ++i) {
            U32 value = current.data[i];
            if (!current.dense)
              result.add(value + bounds[w]);
            else
              while (value) {
                result.add(U32(i * P::width + std::countr_zero(value)) +
                           bounds[w]);
                value &= value - 1;
              }
          }
        }
        if (qi % batch.size() == batch.size() - 1 || qi + 1 == queries.size()) {
          std::lock_guard lock(merge_mutex);
          size_t start = qi / batch.size() * batch.size();
          for (size_t i = start; i <= qi; ++i)
            answers[i].merge(batch[i - start]);
        }
      }
    };
    for (unsigned w = 0; w < workers; ++w)
      threads.emplace_back([&, w] {
        if (bounds[w + 1] - bounds[w] <= 65536)
          process.template operator()<uint16_t>(w);
        else
          process.template operator()<U32>(w);
      });
    for (auto &thread : threads)
      thread.join();
    std::filesystem::create_directories(out);
    std::ofstream output(out + "/results.txt", std::ios::binary);
    for (size_t qi = 0; qi < queries.size(); ++qi) {
      const Result &result = answers[qi];
      output << result.count << ' ' << result.sum;
      for (size_t i = 0; i < std::min<U64>(10, result.count); ++i)
        output << ' ' << result.first[i];
      output << '\n';
    }
    if (!output)
      throw std::runtime_error("cannot write results.txt");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
