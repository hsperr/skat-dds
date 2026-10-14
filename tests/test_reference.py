"""Fixed answers for 300 random positions (50 per game type: 10 full deals, 40 mid-game).

The expected values were produced by tools/compare.py with the solver this repository was
extracted from; the extracted build agreed on every answer and every node count.
"""
import json
import os

import pytest

import skatdds as S

ROWS = json.load(open(os.path.join(os.path.dirname(__file__), "data", "reference.json")))


def pos(r, base=None):
    return S.Position(hands=r["hands"], trick=r["trick"], game=r["game"], declarer=r["declarer"],
                      leader=r["leader"], base=r["base"] if base is None else base)


@pytest.fixture(scope="module")
def positions():
    S.reset()
    return [pos(r) for r in ROWS]


def test_exact(positions):
    assert S.solve_batch(positions) == [r["exact"] for r in ROWS]


def test_threshold(positions):
    t61 = [1 if r["game"] == "null" else 61 for r in ROWS]
    assert S.solve_batch(positions, "threshold", threshold=t61) == [r["thr61"] for r in ROWS]
    tr = [r["thr_rand"] for r in ROWS]
    assert S.solve_batch(positions, "threshold", threshold=tr) == [r["thr_rand_result"] for r in ROWS]
    # consistent with the exact values
    assert [r["exact"] >= r["thr_rand"] for r in ROWS] == [r["thr_rand_result"] for r in ROWS]


def test_move_values():
    rows = [r for r in ROWS if "move_values" in r]
    got = S.solve_batch([pos(r) for r in rows], "move_values")
    assert got == [r["move_values"] for r in rows]
    for r, mv in zip(rows, got):  # the position's value is the best move value of the player to act
        p = pos(r)
        best = max(mv.values()) if p.to_act == p.declarer else min(mv.values())
        assert best == r["exact"]


def test_tricks_objective():
    rows = [r for r in ROWS if "schwarz" in r]
    ps = [pos(r, base=0) for r in rows]
    assert S.solve_batch(ps, objective="tricks") == [r["tricks_exact"] for r in rows]
    got = S.solve_batch(ps, "threshold", threshold=[r["schwarz_target"] for r in rows], objective="tricks")
    assert got == [r["schwarz"] for r in rows]


def test_threads_same_answers(positions):
    sub = positions[::5]
    assert S.solve_batch(sub, threads=2) == [r["exact"] for r in ROWS[::5]]
