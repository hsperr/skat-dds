"""Card notation and Skat trick rules (pure Python, used for input checking and tests).

A card is an int 0..31: card = suit * 8 + rank.
  suits: 0 D (Diamonds / Karo), 1 H (Hearts), 2 S (Spades), 3 C (Clubs)
  ranks: 0 '7', 1 '8', 2 '9', 3 '10', 4 'J', 5 'Q', 6 'K', 7 'A'
Text notation: suit letter + rank, e.g. "CJ", "SA", "H10" (or "HT"), "D7".
A set of cards is a 32-bit mask (bit c set = card c present).
"""
import random

SUITS = "DHSC"
RANKS = "789TJQKA"
DIAMONDS, HEARTS, SPADES, CLUBS, GRAND, NULL = range(6)
GAMES = {"diamonds": 0, "hearts": 1, "spades": 2, "clubs": 3, "grand": 4, "null": 5}
GAME_NAMES = ["diamonds", "hearts", "spades", "clubs", "grand", "null"]
TRUMP = 4  # effective-suit id of trumps

RANK_POINTS = [0, 0, 0, 10, 2, 3, 4, 11]
POINTS = [RANK_POINTS[c % 8] for c in range(32)]
_PLAIN_POWER = [0, 1, 2, 5, -1, 3, 4, 6]  # 7 8 9 Q K 10 A (jacks are trumps)


def parse_card(s):
    """'CJ' / 'c10' / 'HT' -> card id."""
    t = s.strip().upper()
    if len(t) < 2 or t[0] not in SUITS:
        raise ValueError(f"bad card {s!r}")
    r = t[1:]
    if r == "10":
        r = "T"
    if len(r) != 1 or r not in RANKS:
        raise ValueError(f"bad card {s!r}")
    return SUITS.index(t[0]) * 8 + RANKS.index(r)


def card_str(c):
    r = RANKS[c % 8]
    return SUITS[c // 8] + ("10" if r == "T" else r)


def to_cards(x):
    """Cards from 'CJ SA H10', an iterable of ids/strings, or a mask -> sorted list of ids."""
    if x is None:
        return []
    if isinstance(x, int):
        return [c for c in range(32) if x >> c & 1]
    if isinstance(x, str):
        x = x.replace(",", " ").split()
    out = [parse_card(c) if isinstance(c, str) else int(c) for c in x]
    for c in out:
        if not 0 <= c < 32:
            raise ValueError(f"bad card id {c}")
    if len(set(out)) != len(out):
        raise ValueError(f"duplicate card in {x!r}")
    return out


def to_mask(x):
    m = 0
    for c in to_cards(x):
        m |= 1 << c
    return m


def cards_str(x):
    return " ".join(card_str(c) for c in to_cards(x))


def points(x):
    """Card points of a set of cards."""
    return sum(POINTS[c] for c in to_cards(x))


def game_id(g):
    if isinstance(g, str):
        try:
            return GAMES[g.strip().lower()]
        except KeyError:
            raise ValueError(f"unknown game {g!r}; one of {list(GAMES)}") from None
    g = int(g)
    if not 0 <= g <= 5:
        raise ValueError(f"game id must be 0..5, got {g}")
    return g


def effective_suit(c, game):
    if game == NULL:
        return c // 8
    if c % 8 == 4 or c // 8 == game:
        return TRUMP
    return c // 8


def power(c, game):
    """Trick-taking power inside the card's effective suit (higher wins)."""
    if game == NULL:
        return c % 8
    if c % 8 == 4:
        return 100 + c // 8  # jacks: D < H < S < C, above all other trumps
    return _PLAIN_POWER[c % 8]


def legal(hand, trick, game):
    """Legal cards (ids) for a hand given the cards already in the current trick."""
    hand = to_cards(hand)
    if trick:
        lead = effective_suit(trick[0], game)
        follow = [c for c in hand if effective_suit(c, game) == lead]
        if follow:
            return follow
    return hand


def trick_winner(trick, game):
    """Index (0..2) of the winning card in a complete trick (play order)."""
    w = 0
    for i in (1, 2):
        a, b = trick[i], trick[w]
        ea, eb = effective_suit(a, game), effective_suit(b, game)
        if (ea == eb and power(a, game) > power(b, game)) or (ea == TRUMP and eb != TRUMP):
            w = i
    return w


def random_deal(rng=random):
    """Shuffle and deal: ([hand0, hand1, hand2], skat) as lists of card ids."""
    deck = list(range(32))
    rng.shuffle(deck)
    return [sorted(deck[0:10]), sorted(deck[10:20]), sorted(deck[20:30])], sorted(deck[30:32])
