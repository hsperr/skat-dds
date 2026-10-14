"""ctypes binding of libskatdds."""
import ctypes
import os
import sys
from dataclasses import dataclass, field

from . import cards as C

EXACT, THRESHOLD, MOVE_VALUES, MOVE_THRESHOLD = 0, 1, 2, 3
_MODES = {"exact": EXACT, "threshold": THRESHOLD, "move_values": MOVE_VALUES,
          "move_threshold": MOVE_THRESHOLD}
_OBJECTIVES = {"points": 0, "tricks": 1}

# Ablation flags (speed only, never results); see include/skatdds.h.
OPT_NO_TT, OPT_NO_EQUIV, OPT_NO_ORDER, OPT_NO_NORM, OPT_NO_BOUNDS = 1, 2, 4, 16, 32
OPT_MTDF, OPT_NO_PVS, OPT_NO_RUN, OPT_NO_LAST2, OPT_NO_RCACHE = 64, 128, 256, 512, 1024
OPT_NO_HIST, OPT_NO_LAST3, OPT_NO_QSYM = 2048, 4096, 8192

_LIB = None
_EXT = "dylib" if sys.platform == "darwin" else "so"


def library_path():
    """Where the shared library is looked up: $SKATDDS_LIB, the package dir, <repo>/build."""
    env = os.environ.get("SKATDDS_LIB")
    if env:
        return [os.path.abspath(env)]
    here = os.path.dirname(os.path.abspath(__file__))
    name = "libskatdds." + _EXT
    return [os.path.join(here, name),
            os.path.join(os.path.dirname(os.path.dirname(here)), "build", name)]


def load(path=None):
    """Load the library (once). `path` overrides the search."""
    global _LIB
    if _LIB is not None and path is None:
        return _LIB
    paths = [path] if path else library_path()
    found = next((p for p in paths if os.path.exists(p)), None)
    if found is None:
        raise OSError(f"libskatdds not found (tried {paths}); run `make` in the repository root "
                      "or set SKATDDS_LIB")
    lib = ctypes.CDLL(found)
    P = ctypes.c_void_p
    lib.skat_solve_batch.restype = ctypes.c_uint64
    lib.skat_solve_batch.argtypes = [ctypes.c_int, P, P, P, P, P, P, ctypes.c_int, P,
                                     ctypes.c_int, P, P, ctypes.c_int]
    lib.skat_legal.restype = ctypes.c_uint32
    lib.skat_legal.argtypes = [P, P, ctypes.c_int8, ctypes.c_int8, ctypes.c_int8]
    lib.skat_set_options.argtypes = [ctypes.c_int]
    lib.skat_get_options.restype = ctypes.c_int
    lib.skat_set_tt_bits.argtypes = [ctypes.c_int]
    lib.skat_set_tt_min_cards.argtypes = [ctypes.c_int]
    if path is None:
        _LIB = lib
    return lib


@dataclass
class Position:
    """A position in the card play.

    hands     three hands (players 0, 1, 2 in playing order): 'CJ SA H10 ...', lists of card
              ids/strings, or masks. Cards already played are not in the hands.
    game      'diamonds' | 'hearts' | 'spades' | 'clubs' | 'grand' | 'null' (or 0..5)
    declarer  0..2
    leader    player who led the current trick (at the first trick: 0, forehand)
    trick     cards already played to the current trick, in play order (0..2 cards)
    skat      the declarer's two skat cards / discards (their points count for the declarer;
              ignored with objective='tricks')
    base      declarer points already won in earlier tricks. With objective='tricks':
              the number of tricks the declarer already won.
    """
    hands: list
    game: object
    declarer: int = 0
    leader: int = 0
    trick: object = ()
    skat: object = ()
    base: int = 0
    _enc: tuple = field(default=None, init=False, repr=False, compare=False)

    def __post_init__(self):
        if len(self.hands) != 3:
            raise ValueError("need exactly three hands")
        g = C.game_id(self.game)
        hs = [C.to_cards(h) for h in self.hands]
        tr = C.to_cards(self.trick)
        sk = C.to_cards(self.skat)
        allc = [c for h in hs for c in h] + tr + sk
        if len(set(allc)) != len(allc):
            raise ValueError("a card appears twice")
        if len(tr) > 2:
            raise ValueError("at most two cards on the table (a full trick is resolved)")
        if not (0 <= self.declarer <= 2 and 0 <= self.leader <= 2):
            raise ValueError("players are 0, 1, 2")
        n = len(hs[self.leader]) + (1 if tr else 0)  # cards per hand at the trick start
        for k in range(3):
            p = (self.leader + k) % 3
            want = n - 1 if k < len(tr) else n
            if len(hs[p]) != want:
                raise ValueError(f"player {p} holds {len(hs[p])} cards, expected {want}")
        if n == 0:
            raise ValueError("no cards left")
        self._enc = ([C.to_mask(h) for h in hs], tr, g, self.base, C.points(sk))

    @property
    def to_act(self):
        return (self.leader + len(self._enc[1])) % 3


def _arrays(positions, obj=0):
    n = len(positions)
    hands = (ctypes.c_uint32 * (3 * n))()
    trick = (ctypes.c_int8 * (2 * n))(*([-1] * (2 * n)))
    leader = (ctypes.c_int8 * n)()
    decl = (ctypes.c_int8 * n)()
    game = (ctypes.c_int8 * n)()
    base = (ctypes.c_int16 * n)()
    for i, p in enumerate(positions):
        hs, tr, g, b, skat_pts = p._enc
        if obj == 0:
            b += skat_pts
        for k in range(3):
            hands[3 * i + k] = hs[k]
        for k, c in enumerate(tr):
            trick[2 * i + k] = c
        leader[i], decl[i], game[i], base[i] = p.leader, p.declarer, g, b
    return hands, trick, leader, decl, game, base


def solve_batch(positions, mode="exact", threshold=None, objective="points", threads=1,
                return_nodes=False, _lib=None):
    """Solve many positions in one C call.

    mode 'exact'          -> list of values
         'threshold'      -> list of bools (value >= threshold)
         'move_values'    -> list of {card: value} for the legal cards of the player to act
         'move_threshold' -> list of {card: bool}
    threshold: int or one int per position (threshold modes).
    objective: 'points' (card points) or 'tricks' (declarer tricks; suit/Grand only).
    threads:   1 = this thread (its table is reused across calls); k > 1 = k worker threads.
    Cards in the move dicts are strings ('CJ').
    """
    lib = _lib or load()
    m = _MODES[mode] if isinstance(mode, str) else int(mode)
    obj = _OBJECTIVES[objective] if isinstance(objective, str) else int(objective)
    positions = list(positions)
    n = len(positions)
    if n == 0:
        return ([], []) if return_nodes else []
    arrs = _arrays(positions, obj)
    thr = None
    if m in (THRESHOLD, MOVE_THRESHOLD):
        if threshold is None:
            raise ValueError("threshold modes need `threshold`")
        ts = [threshold] * n if isinstance(threshold, int) else list(threshold)
        if len(ts) != n:
            raise ValueError("one threshold per position")
        thr = (ctypes.c_int16 * n)(*ts)
    stride = 32 if m >= MOVE_VALUES else 1
    out = (ctypes.c_int16 * (stride * n))()
    nodes = (ctypes.c_uint64 * n)()
    lib.skat_solve_batch(n, *arrs, m, thr, obj, out, nodes, int(threads))
    if m == EXACT:
        res = list(out)
    elif m == THRESHOLD:
        res = [bool(v) for v in out]
    else:
        conv = int if m == MOVE_VALUES else bool
        res = [{C.card_str(c): conv(out[32 * i + c]) for c in range(32) if out[32 * i + c] >= 0}
               for i in range(n)]
    return (res, list(nodes)) if return_nodes else res


def solve(position, objective="points"):
    """Declarer's final card points with optimal play by everyone (Null: 1 = declarer wins).
    With objective='tricks': the declarer's final number of tricks (base + future tricks)."""
    return solve_batch([position], objective=objective)[0]


def can_reach(position, target, objective="points"):
    """True iff the declarer can force a final value >= target (one null-window search,
    usually much faster than `solve`). Typical targets: 61 (win), 90 (schneider),
    objective='tricks' with target 10 (schwarz), Null with target 1 (win)."""
    return solve_batch([position], "threshold", threshold=target, objective=objective)[0]


def move_values(position, objective="points"):
    """{card: final value if the player to act plays it}, for each legal card."""
    return solve_batch([position], "move_values", objective=objective)[0]


def move_thresholds(position, target, objective="points"):
    """{card: can the declarer still reach >= target after it}, for each legal card."""
    return solve_batch([position], "move_threshold", threshold=target, objective=objective)[0]


def legal_moves(position):
    """Legal cards of the player to act, as strings."""
    lib = load()
    hands, trick, leader, decl, game, _ = _arrays([position])
    m = lib.skat_legal(hands, trick, leader[0], decl[0], game[0])
    return [C.card_str(c) for c in range(32) if m >> c & 1]


def reset():
    """Clear the calling thread's transposition table."""
    load().skat_reset()


def set_options(flags=0):
    """Ablation flags (OPT_*): change speed only, never results."""
    load().skat_set_options(int(flags))
