"""Benchmark: solve times for full deals from the first lead and for mid-game positions.

    python bench/bench.py [--n 100] [--seed 1] [--ablate 0]

Deals are random, with a plausible contract: the declarer is the seat whose 12 cards (hand +
skat) are strongest for the game class, weak deals are redrawn (suit: >= 7 trumps, Grand:
2 * jacks + aces >= 8, Null: few suits without a low card), the skat is picked up and two
cards discarded by a simple rule. Forehand leads. Each position is solved by its own C call
on one thread; the table is cleared once per row (entries from earlier deals of the same row
may be reused, as in real use).
"""
import argparse
import os
import platform
import random
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python"))
import skatdds as S  # noqa: E402
from skatdds import cards as C, sim  # noqa: E402

J = 4  # jack rank


def strength(cls, cards):
    jacks = sum(c % 8 == J for c in cards)
    if cls == "suit":
        return max(jacks + sum(c // 8 == s and c % 8 != J for c in cards) for s in range(4))
    if cls == "grand":
        return 2 * jacks + sum(c % 8 == 7 for c in cards)
    risk = 0  # null: suits whose lowest card is above the 8
    for s in range(4):
        r = [c % 8 for c in cards if c // 8 == s]
        if r and min(r) > 1:
            risk += 1 + (min(r) > 3)
    return -risk


MIN_STRENGTH = {"suit": 7, "grand": 8, "null": 0}


def make_deal(rng, cls):
    while True:
        hands, skat = C.random_deal(rng)
        decl = max(range(3), key=lambda p: strength(cls, hands[p] + skat))
        if strength(cls, hands[decl] + skat) >= MIN_STRENGTH[cls]:
            break
    m = hands[decl] + skat
    if cls == "suit":
        def side(s):
            return [c for c in m if c // 8 == s and c % 8 != J]
        g = max(range(4), key=lambda s: (len(side(s)), C.points(side(s))))
    else:
        g = C.GRAND if cls == "grand" else C.NULL
    if g == C.NULL:
        key = lambda c: -(c % 8)  # noqa: E731  discard the highest cards
    else:
        def key(c):  # keep trumps and aces, discard the most valuable other cards
            if C.effective_suit(c, g) == C.TRUMP:
                return 100
            return 50 if c % 8 == 7 else -C.POINTS[c]
    order = sorted(m, key=key)
    discard, hand = order[:2], sorted(order[2:])
    hands[decl] = hand
    return dict(hands=hands, trick=[], leader=0, declarer=decl, game=g, base=C.points(discard),
                tricks=0)


def advance(rng, state, cards):
    """Random legal play of `cards` cards; None if a Null declarer takes a trick."""
    s = dict(state, hands=[list(h) for h in state["hands"]], trick=[])
    for _ in range(cards):
        p = (s["leader"] + len(s["trick"])) % 3
        sim.apply_card(s, rng.choice(C.legal(s["hands"][p], s["trick"], s["game"])))
        if s["game"] == C.NULL and s["tricks"]:
            return None
    return s


def timed(positions, **kw):
    S.reset()
    ms, nodes, vals = [], [], []
    for p in positions:
        t0 = time.perf_counter()
        v, n = S.solve_batch([p], return_nodes=True, **kw)
        ms.append((time.perf_counter() - t0) * 1e3)
        nodes.append(n[0])
        vals.append(v[0])
    return ms, nodes, vals


def row(name, ms, nodes, extra=""):
    q = sorted(ms)
    p95 = q[min(len(q) - 1, int(0.95 * len(q)))]
    mean = statistics.mean(ms)
    print(f"| {name} | {len(ms)} | {mean:.2f} | {statistics.median(ms):.2f} | {p95:.2f} | "
          f"{1000 / mean:,.0f} | {statistics.mean(nodes):,.0f} | {extra} |", flush=True)


def machine():
    try:
        cpu = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
    except Exception:
        cpu = platform.processor() or platform.machine()
    return f"{cpu}, {platform.system()} {platform.release()}, Python {platform.python_version()}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100, help="deals per game class")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--movevals", type=int, default=30, help="deals for the per-card rows")
    ap.add_argument("--ablate", type=int, default=0, help="suit deals for the ablation table")
    args = ap.parse_args()
    S.load()
    print(f"machine: {machine()}; load avg {os.getloadavg()[0]:.1f}; 1 thread\n")
    print("| positions | n | mean ms | median ms | p95 ms | solves/s | nodes (mean) | result |")
    print("|---|---:|---:|---:|---:|---:|---:|---|")
    deals = {}
    for cls in ("suit", "grand", "null"):
        rng = random.Random(args.seed)
        states = [make_deal(rng, cls) for _ in range(args.n)]
        deals[cls] = states
        ps = [sim.to_position(s) for s in states]
        if cls == "null":
            ms, nd, v = timed(ps)
            row("Null, full deal, exact", ms, nd, f"declarer wins {sum(v) / len(v):.0%}")
        else:
            ms, nd, v = timed(ps)
            row(f"{cls.capitalize()}, full deal, exact", ms, nd,
                f"mean {statistics.mean(v):.1f} pts, wins {sum(x >= 61 for x in v) / len(v):.0%}")
            for t in (61, 90):
                ms, nd, v = timed(ps, mode="threshold", threshold=t)
                row(f"{cls.capitalize()}, full deal, threshold >= {t}", ms, nd, f"yes {sum(v) / len(v):.0%}")
            pt = [sim.to_position(dict(s, base=0)) for s in states]  # base = tricks won so far
            ms, nd, v = timed(pt, mode="threshold", threshold=10, objective="tricks")
            row(f"{cls.capitalize()}, full deal, schwarz (10 tricks)", ms, nd, f"yes {sum(v) / len(v):.0%}")
        k = min(args.movevals, len(ps))
        ms, nd, _ = timed(ps[:k], mode="move_values")
        row(f"{cls.capitalize()}, full deal, value of each lead", ms, nd)
        if cls != "null":
            ms, nd, _ = timed(ps[:k], mode="move_threshold", threshold=61)
            row(f"{cls.capitalize()}, full deal, each lead >= 61", ms, nd)
        for tricks in (3, 6):
            rng = random.Random(args.seed + tricks)
            mid = []
            for s in states:
                for _ in range(20):
                    a = advance(rng, s, 3 * tricks + rng.randrange(3))
                    if a is not None:
                        mid.append(sim.to_position(a))
                        break
            ms, nd, _ = timed(mid)
            row(f"{cls.capitalize()}, after {tricks} tricks (random play), exact", ms, nd)
    # throughput with two worker threads (one batch call)
    ps = [sim.to_position(s) for s in deals["suit"]]
    t0 = time.perf_counter()
    S.solve_batch(ps, threads=2)
    dt = time.perf_counter() - t0
    print(f"\nSuit full deals, exact, one batch on 2 threads: {len(ps) / dt:.1f} solves/s")
    if args.ablate:
        print("\nAblation (suit full deals, exact; results are identical, only speed changes)\n")
        print("| variant | n | mean ms | median ms | p95 ms | solves/s | nodes (mean) | |")
        print("|---|---:|---:|---:|---:|---:|---:|---|")
        ps = [sim.to_position(s) for s in deals["suit"][:args.ablate]]
        ms, nd, base = timed(ps)
        row("all on", ms, nd)
        for name, o in (("no equal-card merging", S.solver.OPT_NO_EQUIV),
                        ("no move-ordering heuristic", S.solver.OPT_NO_ORDER),
                        ("no history heuristic", S.solver.OPT_NO_HIST),
                        ("TT keyed by exact cards", S.solver.OPT_NO_NORM),
                        ("no sure-points / trump-run bounds", S.solver.OPT_NO_BOUNDS | S.solver.OPT_NO_RUN),
                        ("no quasi-symmetry bounds", S.solver.OPT_NO_QSYM),
                        ("no last-3/last-2 solvers", S.solver.OPT_NO_LAST3 | S.solver.OPT_NO_LAST2),
                        ("MTD(f) instead of PVS", S.solver.OPT_MTDF)):
            S.set_options(o)
            ms, nd, v = timed(ps)
            S.set_options(0)
            assert v == base, name
            row(name, ms, nd)


if __name__ == "__main__":
    main()
