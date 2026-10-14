"""Endgames (2-4 tricks left, also mid-trick) against a plain Python minimax."""
import random

import pytest

import skatdds as S
from skatdds import cards as C, sim


def brute(state):
    """(value, {card: value}) by full minimax; value = declarer's final points (Null: 1/0)."""
    g = state["game"]
    if g == C.NULL and state["tricks"]:
        return 0, None
    p = (state["leader"] + len(state["trick"])) % 3
    if not state["hands"][p]:
        return (1 if g == C.NULL else state["base"]), None
    vals = {}
    for c in C.legal(state["hands"][p], state["trick"], g):
        s = dict(state, hands=[list(h) for h in state["hands"]], trick=list(state["trick"]))
        sim.apply_card(s, c)
        vals[c] = brute(s)[0]
    best = max(vals.values()) if p == state["declarer"] else min(vals.values())
    return best, vals


def endgame(rng, game):
    while True:
        s = sim.random_state(rng, game)
        played = 30 - 3 * rng.choice([2, 2, 3, 3, 4]) + rng.randrange(3)
        for _ in range(played):
            p = (s["leader"] + len(s["trick"])) % 3
            sim.apply_card(s, rng.choice(C.legal(s["hands"][p], s["trick"], s["game"])))
        if not (game == C.NULL and s["tricks"]):
            return s


@pytest.mark.parametrize("game", range(6))
def test_endgames(game):
    rng = random.Random(100 + game)
    for _ in range(40):
        s = endgame(rng, game)
        p = sim.to_position(s)
        want, mv = brute(s)
        assert S.solve(p) == want
        assert S.move_values(p) == {C.card_str(c): v for c, v in mv.items()}
        assert sorted(S.legal_moves(p)) == sorted(C.card_str(c) for c in mv)
        for t in ((0, 1) if game == C.NULL else (want, want + 1, 61, 31)):
            assert S.can_reach(p, t) == (want >= t)
