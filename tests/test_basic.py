"""Small hand-checkable cases, notation and input checks."""
import pytest

import skatdds as S
from skatdds import cards as C


def test_notation():
    assert C.parse_card("D7") == 0
    assert C.parse_card("CJ") == 3 * 8 + 4
    assert C.parse_card("h10") == C.parse_card("HT") == 8 + 3
    assert C.card_str(C.parse_card("SA")) == "SA"
    assert C.cards_str("CJ SA H10") == "CJ SA H10"
    assert C.cards_str(C.to_mask("CJ SA H10")) == "H10 SA CJ"  # masks list by card id
    assert C.points("CJ SA H10 D7") == 2 + 11 + 10
    assert C.points(C.to_mask(range(32))) == 120
    with pytest.raises(ValueError):
        C.parse_card("X7")
    with pytest.raises(ValueError):
        C.to_cards("SA SA")


def test_trick_winner():
    def tw(trick, game):
        return C.trick_winner(C.to_cards(trick), C.game_id(game))

    assert tw("SA S10 SK", "hearts") == 0
    assert tw("SA S10 H7", "hearts") == 2       # ruff
    assert tw("SA DJ S10", "hearts") == 1       # jacks are trumps
    assert tw("DJ CJ HA", "hearts") == 1        # CJ highest
    assert tw("S10 SK SA", "grand") == 2
    assert tw("SA S7 DJ", "grand") == 2
    assert tw("S10 SJ SQ", "null") == 2         # Null: 7 8 9 10 J Q K A, no trumps
    assert tw("S10 SA HA", "null") == 1


def test_all_trumps_takes_everything():
    # declarer (forehand) holds all ten clubs trumps; plays first
    p = S.Position(hands=["CJ SJ HJ DJ CA C10 CK CQ C9 C8",
                          "C7 S10 SK SQ S9 HA H7 HK HQ H9",
                          "DA D10 DK DQ D9 D8 D7 S8 S7 H8"],
                   skat="SA H10", game="clubs", declarer=0)
    assert S.solve(p) == 120
    assert S.can_reach(p, 120)
    assert S.solve(p, objective="tricks") == 10  # schwarz; skat points do not count as tricks
    assert S.can_reach(p, 10, objective="tricks")


def test_last_trick():
    # one card each, middlehand (1) leads; declarer 2 trumps the ace
    p = S.Position(hands=["S7", "SA", "H7"], game="hearts", declarer=2, leader=1, base=50)
    assert S.solve(p) == 50 + 11
    p = S.Position(hands=["S7", "SA", "D7"], game="hearts", declarer=2, leader=1, base=50)
    assert S.solve(p) == 50  # no trump, declarer cannot win the trick


def test_mid_trick_and_choice():
    # forehand led SA; declarer (1) holds S10 and HJ: must follow with S10 (loses 21 points)
    p = S.Position(hands=["", "S10", "SK"], trick="SA", game="grand", declarer=1, leader=0)
    assert S.legal_moves(p) == ["S10"]
    assert S.solve(p) == 0
    # void in spades: may trump with the jack and wins 11 + 2 + 4
    p = S.Position(hands=["", "HJ", "SK"], trick="SA", game="grand", declarer=1, leader=0)
    assert S.solve(p) == 17


def test_null():
    # declarer (0) leads D7 and must take no trick; others hold higher diamonds
    p = S.Position(hands=["D7 S7", "D8 SA", "D9 SK"], game="null", declarer=0)
    assert S.solve(p) == 1
    assert S.can_reach(p, 1)
    # declarer only has an ace to lead: loses
    p = S.Position(hands=["DA S7", "D8 S8", "D9 S9"], game="null", declarer=0)
    assert S.solve(p) == 0
    assert S.move_values(p) == {"DA": 0, "S7": 0}


def test_validation():
    with pytest.raises(ValueError):  # card twice
        S.Position(hands=["SA", "SA", "S7"], game="grand")
    with pytest.raises(ValueError):  # hand sizes
        S.Position(hands=["SA S7", "S8", "S9"], game="grand")
    with pytest.raises(ValueError):
        S.Position(hands=["SA", "S8", "S9"], game="bogus")
    # mid-trick: the player who already played holds one card less
    S.Position(hands=["S7", "S8 H7", "S9 H8"], trick="SA", game="grand", leader=0)


def test_threshold_matches_exact():
    from skatdds import sim
    import random
    rng = random.Random(5)
    for g in range(5):
        p = sim.random_position(rng, g, played=12)
        v = S.solve(p)
        assert S.can_reach(p, v) and not S.can_reach(p, v + 1)
        mt = S.move_thresholds(p, v)
        assert mt == {c: x >= v for c, x in S.move_values(p).items()}
