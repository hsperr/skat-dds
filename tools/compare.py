"""Compare two builds of libskatdds on the same random positions (values and node counts).

    python tools/compare.py --ref /path/to/other/libskatdds.dylib [--n 50] [--fixture out.json]

Per game type (diamonds..null): n/5 full deals at the first lead and 4n/5 mid-game positions
(6..24 cards played). Modes: exact, threshold (61 and a random target), move values
(mid-game only), tricks objective (threshold 10 = schwarz, suit/Grand). Both libraries run
single-threaded from an empty table with the same call sequence, so identical code gives
identical node counts too. --fixture writes positions + reference answers as JSON.
"""
import argparse
import json
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python"))
import skatdds as S  # noqa: E402
from skatdds import cards as C, sim  # noqa: E402
from skatdds.solver import library_path  # noqa: E402


def make_positions(n, seed):
    rng = random.Random(seed)
    out = []
    for g in range(6):
        for i in range(n):
            played = 0 if i < n // 5 else rng.randrange(6, 25)
            out.append(sim.random_position(rng, g, played))
    return out


def ncards(p):
    return sum(bin(h).count("1") for h in p._enc[0]) + len(p._enc[1])


def remaining_tricks(p):
    return (ncards(p) + 2) // 3


def run_all(lib, pos, thr_rand):
    lib.skat_reset()
    full = pos
    mid = [p for p in pos if ncards(p) < 30]
    sg = [S.Position(hands=p._enc[0], game=p.game, declarer=p.declarer, leader=p.leader,
                     trick=p.trick) for p in pos if p._enc[2] != C.NULL]  # base 0 = tricks so far
    res = {}
    nodes = {}
    def go(key, ps, **kw):
        res[key], nodes[key] = S.solve_batch(ps, return_nodes=True, _lib=lib, **kw)
    go("exact", full)
    go("thr61", full, mode="threshold", threshold=[1 if p._enc[2] == C.NULL else 61 for p in full])
    go("thr_rand", full, mode="threshold", threshold=thr_rand)
    go("move_values", mid, mode="move_values")
    go("move_thr61", mid, mode="move_threshold", threshold=[1 if p._enc[2] == C.NULL else 61 for p in mid])
    # schwarz: the declarer takes every remaining trick
    go("schwarz", sg, mode="threshold", threshold=[remaining_tricks(p) for p in sg], objective="tricks")
    go("tricks_exact", sg, objective="tricks")
    return res, nodes, mid, sg


def pos_json(p):
    hs, tr, g, b, sk = p._enc
    return dict(hands=[C.cards_str(h) for h in hs], trick=C.cards_str(tr), game=C.GAME_NAMES[g],
                declarer=p.declarer, leader=p.leader, base=b + sk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="reference library")
    ap.add_argument("--ours", default=None, help="library under test (default: the usual search)")
    ap.add_argument("--n", type=int, default=50, help="positions per game type")
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--fixture", help="write positions + reference answers here")
    args = ap.parse_args()
    ours = S.load(args.ours or next(p for p in library_path() if os.path.exists(p)))
    ref = S.load(args.ref)
    pos = make_positions(args.n, args.seed)
    rng = random.Random(args.seed + 1)
    thr_rand = [rng.randrange(2) if p._enc[2] == C.NULL else rng.randrange(0, 121) for p in pos]
    r_ref, n_ref, mid, sg = run_all(ref, pos, thr_rand)
    r_our, n_our, _, _ = run_all(ours, pos, thr_rand)
    total = agree = node_eq = 0
    for k in r_ref:
        a = sum(x == y for x, y in zip(r_ref[k], r_our[k]))
        ne = sum(x == y for x, y in zip(n_ref[k], n_our[k]))
        print(f"{k:<13} {a}/{len(r_ref[k])} answers agree, {ne}/{len(r_ref[k])} node counts equal")
        total += len(r_ref[k])
        agree += a
        node_eq += ne
    print(f"TOTAL {agree}/{total} answers agree, {node_eq}/{total} node counts equal "
          f"({len(pos)} positions)")
    if args.fixture:
        idx_mid = {id(p) for p in mid}
        idx_sg = {id(p) for p in sg}
        mi = iter(range(len(mid)))
        si = iter(range(len(sg)))
        rows = []
        for i, p in enumerate(pos):
            d = pos_json(p)
            d.update(exact=r_ref["exact"][i], thr61=r_ref["thr61"][i], thr_rand=thr_rand[i],
                     thr_rand_result=r_ref["thr_rand"][i])
            if id(p) in idx_mid:
                j = next(mi)
                d["move_values"] = r_ref["move_values"][j]
            if id(p) in idx_sg:
                j = next(si)
                d["schwarz_target"] = remaining_tricks(p)
                d["schwarz"] = r_ref["schwarz"][j]
                d["tricks_exact"] = r_ref["tricks_exact"][j]
            rows.append(d)
        with open(args.fixture, "w") as f:
            f.write("[\n" + ",\n".join(json.dumps(r, separators=(",", ":")) for r in rows) + "\n]\n")
        print("wrote", args.fixture)
    return 0 if agree == total else 1


if __name__ == "__main__":
    sys.exit(main())
