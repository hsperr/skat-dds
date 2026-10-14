"""skatdds: double-dummy (all cards visible) solver for Skat.

    >>> from skatdds import Position, solve, can_reach
    >>> pos = Position(hands=["CJ SJ HJ DJ CA CT CK CQ C9 C8", "...", "..."],
    ...                skat="SA ST", game="clubs", declarer=0)
    >>> solve(pos)        # declarer card points with optimal play
"""
from .cards import (GAMES, card_str, cards_str, legal, parse_card, points, random_deal,
                    to_cards, to_mask, trick_winner)
from .solver import (Position, can_reach, legal_moves, load, move_thresholds, move_values,
                     reset, set_options, solve, solve_batch)

__version__ = "0.1.0"
__all__ = ["Position", "solve", "can_reach", "move_values", "move_thresholds", "legal_moves",
           "solve_batch", "reset", "set_options", "load", "GAMES", "card_str", "cards_str",
           "parse_card", "points", "to_cards", "to_mask", "legal", "trick_winner", "random_deal"]
