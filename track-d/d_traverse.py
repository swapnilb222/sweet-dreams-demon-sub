#!/usr/bin/env python3
"""Dataset generator for Speed Demon 2026, Track D (Traverse).

    python3.13 generators/d_traverse.py --seed 1 --scale full --out .data/d/full-seed1

Writes graph.txt and queries.txt, exactly as specs/d-traverse.md describes them.
The same seed and scale give byte-identical files on any machine: all randomness
comes from one random.Random instance, and every number is computed with integer
arithmetic (floats are only ever compared against probabilities).

The graph is a synthetic road network.

* Main body. Nodes sit on a jittered lattice. Arterial roads run along every 8th
  row and column, highway links join every 6th node along every 48th row and
  column, and a random spanning forest of local streets ties every other node to
  them. All of that is two-way, so the main body is strongly connected. Optional
  local streets and diagonals are then added, 15% of them one-way, and the
  optional ones are trimmed at random until the edge budget is met exactly.
  A weight is a travel time: straight-line length divided by the road's speed,
  with independent noise per direction.
* Special nodes, injected at fixed rates: nodes with no edges, nodes with no
  in-edges, nodes with no out-edges, and small islands that nothing connects to.
  The islands carry the hand-built edge cases (weight-0 path, parallel edges,
  self-loop, a chain of 2^32 - 1 weights).
* Node ids are relabelled with a random permutation and edge lines are shuffled,
  so neither ids nor file order carry any locality.

Queries: most pick s uniformly from the main body and t by Dijkstra rank from s
(see QUERY_KINDS and rank() below); the rest are injected edge cases.
"""

import argparse
import hashlib
import heapq
import math
import os
import random

UINT32_MAX = 2**32 - 1

# scale: (nodes, directed edges, queries)
SCALES = {
    "full": (1_740_512, 5_000_000, 2_000),
    "sample": (174_051, 500_000, 200),
    "tiny": (348, 1_000, 40),
}

# Main body.
CELL = 1000  # lattice spacing, in coordinate units
JITTER = 350  # each node moves up to this far in x and in y
ART_EVERY = 8  # arterial roads on every 8th row and column
HWY_EVERY = 48  # highways on every 48th row and column ...
HWY_SKIP = 6  # ... with a junction at every 6th node
LOCAL, ARTERIAL, HIGHWAY = 0, 1, 2
SPEED = (800, 2000, 4000)  # base weight = length * 100 // speed
P_DIAG = 0.12  # chance a lattice cell gets a diagonal street
P_SLOW = 0.03  # chance a local street is slow: base weight x3 to x12
P_TWO_WAY = 0.85  # chance an optional street is two-way
EXCESS = 1.02  # build ~2% more optional edges than needed, then drop some at random

# Special structures, per scale: max(minimum, total // per).
ISOLATED = (100_000, 2)  # per nodes, minimum
SOURCES = (100_000, 2)  # no in-edges; two out-edges of weight >= 2^32 - 2^20
SINKS = (100_000, 2)  # no out-edges; two in-edges from the main body
ISLANDS = (200_000, 1)
PARALLEL = (1_000, 2)  # per edges: extra edge, same (u, v), different weight
DUPLICATE_LINES = (5_000, 1)  # extra edge line identical to another one
SELF_LOOPS = (10_000, 1)
ZERO_WEIGHT = (2_000, 1)  # main-body edges whose weight is set to 0

# One island: a two-way ring of RING nodes plus a two-node spur.
RING = 12
ISLAND_NODES = RING + 2
ISLAND_ARCS = 2 * RING + 2 + 4 + 1 + 2  # ring, zero chain, 2 parallel pairs, loop, spur

# Normal queries: t is the node a Dijkstra search from s settles r-th (s itself is
# the 1st), with octave o uniform in [1, MAX_OCTAVE] and r uniform in [2^o, 2^(o+1)).
MAX_OCTAVE = 16
INF = 1 << 62  # larger than any distance: at most (n - 1) * (2^32 - 1) < 2^53

# Injected queries: (kind, per 1000 queries, minimum per scale). Every other query
# is "normal".
QUERY_KINDS = (
    ("same_node", 2, 1),  # s == t in the main body: 0
    ("same_isolated", 1, 1),  # s == t on a node with no edges: 0
    ("zero_path", 1, 1),  # s != t joined by weight-0 edges: 0
    ("parallel_min", 2, 2),  # answer is the lighter of two parallel edges, both file orders
    ("max_weight", 1, 1),  # answer is exactly 2^32 - 1
    ("over_uint32", 1, 1),  # answer is above 2^32 - 1
    ("from_source", 1, 1),  # s has no in-edges and only heavy out-edges
    ("to_sink", 1, 1),  # t has no out-edges
    ("unreachable_island", 2, 1),  # s in an island, t in the main body: -1
    ("unreachable_sink", 1, 1),  # s has no out-edges: -1
    ("unreachable_isolated", 1, 1),  # s has no edges at all: -1
    ("unreachable_source", 1, 1),  # t has no in-edges, s in the main body: -1
    ("duplicate", 10, 1),  # an exact repeat of another query line
)


def derive_seed(seed, scale):
    """Seed for one (seed, scale) pair, so sample is not a prefix of full."""
    digest = hashlib.sha256(f"speed-demon-2026/d-traverse/{scale}/{seed}".encode()).digest()
    return int.from_bytes(digest[:8], "big")


def rate(total, per_min):
    per, minimum = per_min
    return max(minimum, total // per)


def distinct(rng, population, k):
    """k distinct ints from range(population), in the order drawn."""
    seen, out = set(), []
    while len(out) < k:
        x = rng.randrange(population)
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def main_network(rng, n_main, needed):
    """Arcs (u, v, w) of the strongly connected main body: exactly `needed` of them."""
    rnd = rng.random
    width = math.isqrt(n_main - 1) + 1  # ceil(sqrt(n_main)); the last row may be partial
    span = 2 * JITTER + 1
    xs = [(k % width) * CELL + int(rnd() * span) - JITTER for k in range(n_main)]
    ys = [(k // width) * CELL + int(rnd() * span) - JITTER for k in range(n_main)]

    backbone = []  # (a, b, class): always present, always two-way
    local = []  # (a, b): local lattice streets, spanning-forest candidates
    diagonal = []  # (a, b)
    for k in range(n_main):
        j, i = divmod(k, width)
        if i + 1 < width and k + 1 < n_main:
            if j % ART_EVERY == 0:
                backbone.append((k, k + 1, ARTERIAL))
            else:
                local.append((k, k + 1))
        if k + width < n_main:
            if i % ART_EVERY == 0:
                backbone.append((k, k + width, ARTERIAL))
            else:
                local.append((k, k + width))
        if i + 1 < width and k + width + 1 < n_main and rnd() < P_DIAG:
            if rnd() < 0.5:
                diagonal.append((k, k + width + 1))
            else:
                diagonal.append((k + 1, k + width))
        if j % HWY_EVERY == 0 and i % HWY_SKIP == 0 and i + HWY_SKIP < width and k + HWY_SKIP < n_main:
            backbone.append((k, k + HWY_SKIP, HIGHWAY))
        if i % HWY_EVERY == 0 and j % HWY_SKIP == 0 and k + HWY_SKIP * width < n_main:
            backbone.append((k, k + HWY_SKIP * width, HIGHWAY))

    # Random spanning forest (Kruskal over shuffled local streets) on top of the
    # arterials and highways: the lattice is connected, so the result spans it.
    parent = list(range(n_main))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b, _ in backbone:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    rng.shuffle(local)
    optional = []
    for a, b in local:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
            backbone.append((a, b, LOCAL))
        else:
            optional.append((a, b))
    optional += diagonal
    del local, diagonal, parent

    def road(a, b, cls):
        dx, dy = xs[a] - xs[b], ys[a] - ys[b]
        base = math.isqrt(dx * dx + dy * dy) * 100 // SPEED[cls]
        if cls == LOCAL and rnd() < P_SLOW:
            base *= 3 + int(rnd() * 10)
        return base

    def noisy(base):  # one direction of a road: +-12%, at least 1
        return max(1, base * (88 + int(rnd() * 25)) // 100)

    arcs = []
    for a, b, cls in backbone:
        base = road(a, b, cls)
        arcs.append((a, b, noisy(base)))
        arcs.append((b, a, noisy(base)))
    remaining = needed - len(arcs)
    if remaining < 0:
        raise ValueError("edge budget below the backbone size")

    p_keep = min(1.0, remaining * EXCESS / (len(optional) * (1 + P_TWO_WAY)))
    extra, reserve = [], []
    for a, b in optional:
        if rnd() < p_keep:
            base = road(a, b, LOCAL)
            if rnd() < P_TWO_WAY:
                extra.append((a, b, noisy(base)))
                extra.append((b, a, noisy(base)))
            elif rnd() < 0.5:
                extra.append((a, b, noisy(base)))
            else:
                extra.append((b, a, noisy(base)))
        else:
            reserve.append((a, b))
    if len(extra) >= remaining:
        rng.shuffle(extra)
        del extra[remaining:]
    else:  # rare: top up with one-way streets that were not kept
        rng.shuffle(reserve)
        short = remaining - len(extra)
        if short > len(reserve):
            raise ValueError("edge budget above what the lattice can hold")
        for a, b in reserve[:short]:
            base = road(a, b, LOCAL)
            extra.append((a, b, noisy(base)) if rnd() < 0.5 else (b, a, noisy(base)))
    arcs += extra
    return arcs


def island(rng, first):
    """Arcs of one island whose nodes are first .. first + ISLAND_NODES - 1, and its named nodes."""
    r = list(range(first, first + RING))
    h1, h2 = first + RING, first + RING + 1
    arcs = []
    for k in range(RING):  # two-way ring, weights 50..500 each way
        a, b = r[k], r[(k + 1) % RING]
        arcs.append((a, b, 50 + rng.randrange(451)))
        arcs.append((b, a, 50 + rng.randrange(451)))
    arcs.append((r[0], r[3], 0))  # d(r0, r6) = 0
    arcs.append((r[3], r[6], 0))
    # Two parallel pairs. The light edge (1..20) beats every other path (>= 100);
    # the heavy one (>= 2000) loses to the ring (<= 1000).
    pairs = []
    for a, b in ((r[8], r[10]), (r[9], r[11])):
        light = len(arcs)
        arcs.append((a, b, 1 + rng.randrange(20)))
        arcs.append((a, b, 2000 + rng.randrange(1000)))
        pairs.append((light, light + 1))
    arcs.append((r[5], r[5], 1 + rng.randrange(100)))  # self-loop
    arcs.append((r[11], h1, UINT32_MAX))  # d(r11, h1) = 2^32 - 1
    arcs.append((h1, h2, UINT32_MAX - rng.randrange(1000)))  # d(r11, h2) > 2^32 - 1
    assert len(arcs) == ISLAND_ARCS
    named = {"ring": r, "zero": (r[0], r[6]), "parallel": ((r[8], r[10]), (r[9], r[11])),
             "max_weight": (r[11], h1), "over_uint32": (r[11], h2)}
    return arcs, pairs, named


def rank_target(adj, dist, s, r):
    """The node a Dijkstra search from s settles r-th (s is the 1st), or -1 if fewer are reachable."""
    dist[s] = 0
    touched = [s]
    heap = [(0, s)]
    settled = 0
    found = -1
    pop, push = heapq.heappop, heapq.heappush
    while heap:
        d, u = pop(heap)
        if d > dist[u]:
            continue
        settled += 1
        if settled == r:
            found = u
            break
        for _, v, w in adj[u]:
            nd = d + w
            if nd < dist[v]:
                if dist[v] == INF:
                    touched.append(v)
                dist[v] = nd
                push(heap, (nd, v))
    for v in touched:
        dist[v] = INF
    return found


def build(seed, scale):
    """Return (n, arcs in file order, queries, kinds, manifest), ids already relabelled."""
    n, m, q = SCALES[scale]
    rng = random.Random(derive_seed(seed, scale))

    n_iso, n_src, n_snk = rate(n, ISOLATED), rate(n, SOURCES), rate(n, SINKS)
    n_isl = rate(n, ISLANDS)
    n_main = n - n_iso - n_src - n_snk - n_isl * ISLAND_NODES
    isolated = list(range(n_main, n_main + n_iso))
    sources = list(range(isolated[-1] + 1, isolated[-1] + 1 + n_src))
    sinks = list(range(sources[-1] + 1, sources[-1] + 1 + n_snk))
    first_island = sinks[-1] + 1

    n_par, n_dup = rate(m, PARALLEL), rate(m, DUPLICATE_LINES)
    n_loop, n_zero = rate(m, SELF_LOOPS), rate(m, ZERO_WEIGHT)
    n_special = 2 * n_src + 2 * n_snk + ISLAND_ARCS * n_isl
    n_base = m - n_par - n_dup - n_loop - n_special

    arcs = main_network(rng, n_main, n_base)
    for i in distinct(rng, n_base, n_zero):
        u, v, _ = arcs[i]
        arcs[i] = (u, v, 0)
    pairs = []  # (light arc index, heavy arc index, light edge listed first?)
    for k, i in enumerate(distinct(rng, n_base, n_par)):
        u, v, w = arcs[i]
        if w > 0 and rng.random() < 0.5:
            w2 = rng.randrange(w)  # lighter than the original
        else:
            w2 = w + 1 + rng.randrange(w + 1)  # heavier
        arcs.append((u, v, w2))
        light, heavy = (len(arcs) - 1, i) if w2 < w else (i, len(arcs) - 1)
        pairs.append((light, heavy, k % 2 == 0))
    for i in distinct(rng, n_base, n_dup):
        arcs.append(arcs[i])
    for _ in range(n_loop):
        u = rng.randrange(n_main)
        arcs.append((u, u, 1 + rng.randrange(300)))

    for s in sources:
        for y in distinct(rng, n_main, 2):
            arcs.append((s, y, UINT32_MAX - rng.randrange(1 << 20)))
    sink_tails = {}
    for t in sinks:
        sink_tails[t] = distinct(rng, n_main, 2)
        for y in sink_tails[t]:
            arcs.append((y, t, 1 + rng.randrange(500)))
    islands = []
    for k in range(n_isl):
        isl_arcs, isl_pairs, named = island(rng, first_island + k * ISLAND_NODES)
        offset = len(arcs)
        arcs += isl_arcs
        pairs.append((offset + isl_pairs[0][0], offset + isl_pairs[0][1], True))
        pairs.append((offset + isl_pairs[1][0], offset + isl_pairs[1][1], False))
        islands.append(named)
    assert len(arcs) == m, (len(arcs), m)

    # ---- relabel ids, shuffle edge lines ------------------------------------
    # Drawn before the queries, so graph.txt does not depend on query settings.
    perm = list(range(n))
    rng.shuffle(perm)
    order = list(range(m))
    rng.shuffle(order)
    watch = {}
    for light, heavy, _ in pairs:
        watch[light] = watch[heavy] = -1
    for p, i in enumerate(order):
        if i in watch:
            watch[i] = p
    for light, heavy, light_first in pairs:  # fix the file order of each parallel pair
        pl, ph = watch[light], watch[heavy]
        if (pl < ph) != light_first:
            order[pl], order[ph] = heavy, light
            watch[light], watch[heavy] = ph, pl

    # ---- queries, on original ids -------------------------------------------
    adj = [[] for _ in range(n)]
    for arc in arcs:
        adj[arc[0]].append(arc)
    dist = [INF] * n
    rank_cap = n_main - 1  # only binds at tiny scale

    def rank():
        o = 1 + rng.randrange(MAX_OCTAVE)
        return min(rank_cap, (1 << o) + rng.randrange(1 << o))

    def main_node():
        return rng.randrange(n_main)

    def pick(seq):
        return seq[rng.randrange(len(seq))]

    counts = {kind: max(minimum, q * per_mille // 1000) for kind, per_mille, minimum in QUERY_KINDS}
    counts["normal"] = q - sum(counts.values())
    if counts["normal"] < 1:
        raise ValueError("too few queries for the injected cases")

    plan = []  # (s, t, kind, rank or 0)
    for k in range(counts["normal"]):
        r = rank()
        while True:  # the main body is strongly connected, so this never loops in practice
            s = main_node()
            t = rank_target(adj, dist, s, r)
            if t >= 0:
                break
        plan.append((s, t, "normal", r))
    for kind, _, _ in QUERY_KINDS:
        for k in range(counts[kind] if kind != "duplicate" else 0):
            r = 0
            if kind == "same_node":
                s = t = main_node()
            elif kind == "same_isolated":
                s = t = pick(isolated)
            elif kind == "zero_path":
                s, t = pick(islands)["zero"]
            elif kind == "parallel_min":
                s, t = pick(islands)["parallel"][k % 2]
            elif kind == "max_weight":
                s, t = pick(islands)["max_weight"]
            elif kind == "over_uint32":
                s, t = pick(islands)["over_uint32"]
            elif kind == "from_source":
                s, r = pick(sources), rank()
                t = rank_target(adj, dist, s, r)
                assert t >= 0
            elif kind == "to_sink":
                t = pick(sinks)
                s = pick(sink_tails[t])
            elif kind == "unreachable_island":
                s, t = pick(pick(islands)["ring"]), main_node()
            elif kind == "unreachable_sink":
                s, t = pick(sinks), main_node()
            elif kind == "unreachable_isolated":
                s, t = pick(isolated), main_node()
            elif kind == "unreachable_source":
                s, t = main_node(), pick(sources)
            plan.append((s, t, kind, r))
    rng.shuffle(plan)
    repeatable = [p for p in plan if p[2] != "unreachable_source"]
    for _ in range(counts["duplicate"]):
        s, t, _, r = pick(repeatable)
        plan.insert(rng.randrange(len(plan) + 1), (s, t, "duplicate", r))
    del adj, dist

    out_arcs = [(perm[u], perm[v], w) for u, v, w in (arcs[i] for i in order)]
    queries = [(perm[s], perm[t]) for s, t, _, _ in plan]
    kinds = [kind for _, _, kind, _ in plan]
    manifest = {
        "n": n, "m": m, "q": q, "n_main": n_main,
        "isolated": [perm[x] for x in isolated],
        "sources": [perm[x] for x in sources],
        "sinks": [perm[x] for x in sinks],
        "islands": n_isl, "injected_parallel_pairs": len(pairs),
        "injected_duplicate_lines": n_dup, "injected_self_loops": n_loop + n_isl,
        "injected_zero_weight_edges": n_zero + 2 * n_isl,
        "query_counts": counts, "ranks": [r for _, _, _, r in plan],
    }
    return n, out_arcs, queries, kinds, manifest


def write(out_dir, n, arcs, queries):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "graph.txt"), "w", encoding="ascii", newline="\n") as f:
        f.write(f"{n} {len(arcs)}\n")
        step = 1 << 16
        for start in range(0, len(arcs), step):
            f.write("".join(f"{u} {v} {w}\n" for u, v, w in arcs[start:start + step]))
    with open(os.path.join(out_dir, "queries.txt"), "w", encoding="ascii", newline="\n") as f:
        f.write("".join(f"{s} {t}\n" for s, t in queries))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--scale", choices=sorted(SCALES), required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    n, arcs, queries, _, _ = build(args.seed, args.scale)
    write(args.out, n, arcs, queries)


if __name__ == "__main__":
    main()
