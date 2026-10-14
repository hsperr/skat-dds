"""Random positions for tests and benchmarks (uniform random deals, random legal play)."""
import random

from . import cards as C
from .solver import Position


def apply_card(state, card):
    """Play `card` for the player to act. state: dict(hands, trick, leader, declarer, game,
    base, tricks) with hands as lists of ids; updates it in place."""
    p = (state["leader"] + len(state["trick"])) % 3
    state["hands"][p].remove(card)
    state["trick"].append(card)
    if len(state["trick"]) == 3:
        w = (state["leader"] + C.trick_winner(state["trick"], state["game"])) % 3
        if w == state["declarer"]:
            state["base"] += C.points(state["trick"])
            state["tricks"] += 1
        state["leader"] = w
        state["trick"] = []


def random_state(rng=random, game=None, declarer=None):
    """Fresh deal at the first lead: random (or given) declarer and game; the skat is the
    declarer's (as if picked up and the same two cards discarded); forehand (0) leads."""
    hands, skat = C.random_deal(rng)
    g = rng.randrange(6) if game is None else C.game_id(game)
    d = rng.randrange(3) if declarer is None else declarer
    return dict(hands=hands, trick=[], leader=0, declarer=d, game=g, base=C.points(skat),
                tricks=0, skat=skat)


def to_position(state):
    return Position(hands=[list(h) for h in state["hands"]], game=state["game"],
                    declarer=state["declarer"], leader=state["leader"], trick=list(state["trick"]),
                    base=state["base"])


def random_position(rng=random, game=None, played=0, declarer=None):
    """A position after `played` random legal cards from a random deal. For Null, deals in
    which the declarer takes a trick before that are redrawn."""
    while True:
        s = random_state(rng, game, declarer)
        ok = True
        for _ in range(played):
            p = (s["leader"] + len(s["trick"])) % 3
            apply_card(s, rng.choice(C.legal(s["hands"][p], s["trick"], s["game"])))
            if s["game"] == C.NULL and s["tricks"]:
                ok = False
                break
        if ok:
            return to_position(s)
