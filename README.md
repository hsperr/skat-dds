# skatdds

A double-dummy solver for the card game [Skat](https://en.wikipedia.org/wiki/Skat_(card_game)).

"Double-dummy" means all three hands are visible to every player. Given a position in the
card play, the solver computes the exact result when declarer and both defenders play
perfectly:

- **Suit and Grand games**: the declarer's final card points (0..120).
- **Null**: whether the declarer can avoid taking any trick.

It also answers threshold questions ("can the declarer reach 61 / 90 points?", "can the
declarer take every trick?"), which are much faster than exact values, and gives the value of
every legal card for the player to act.

The core is one C++17 file with a small C API. A ctypes-based Python package
(no dependencies) is included.

## Build

```sh
make                 # build/libskatdds.dylib (macOS) or build/libskatdds.so (Linux)
make PORTABLE=1      # without -march=native / -mcpu=native
```

or with CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Python package (compiles the library into the package; needs a C++17 compiler):

```sh
pip install .
```

Without installing, `PYTHONPATH=python` works after `make` (the package looks for the library
in its own directory, then in `build/`, or wherever `$SKATDDS_LIB` points).

## Python usage

Cards are written as suit letter + rank: suits `C` (clubs), `S` (spades), `H` (hearts),
`D` (diamonds); ranks `7 8 9 10 J Q K A` (`T` also works for 10). Players are 0, 1, 2 in
playing order; forehand (0) leads the first trick.

```python
from skatdds import Position, solve, can_reach, move_values, solve_batch

pos = Position(
    hands=["D10 H7 HQ HA S9 S10 SK C8 CQ CA",   # player 0 (forehand)
           "D7 D8 DJ DQ DK DA H8 HJ HK CJ",     # player 1 (declarer)
           "H9 S7 S8 SJ SQ SA C7 C9 C10 CK"],   # player 2
    skat="D9 H10",          # the declarer's discards, counted for the declarer
    game="diamonds",        # diamonds | hearts | spades | clubs | grand | null
    declarer=1)

solve(pos)                  # 91: declarer card points with optimal play
can_reach(pos, 61)          # True   (one null-window search, usually much faster)
can_reach(pos, 90)          # True   (schneider)
can_reach(pos, 10, objective="tricks")   # False: no schwarz
move_values(pos)            # value of each legal lead for forehand:
# {'D10': 91, 'H7': 98, 'HQ': 98, 'HA': 100, 'S9': 91, 'S10': 91, 'SK': 91, 'C8': 91, 'CQ': 91, 'CA': 91}

# mid-trick: forehand has led D10, the declarer is to play
p2 = Position(hands=["H7 HQ HA S9 S10 SK C8 CQ CA",
                     "D7 D8 DJ DQ DK DA H8 HJ HK CJ",
                     "H9 S7 S8 SJ SQ SA C7 C9 C10 CK"],
              trick="D10", skat="D9 H10", game="diamonds", declarer=1)
move_values(p2)
# {'D7': 79, 'D8': 79, 'DJ': 77, 'DQ': 76, 'DK': 75, 'DA': 68, 'HJ': 77, 'CJ': 91}

solve_batch([pos, p2], threads=2)            # [91, 91]
solve_batch([pos, p2], "threshold", threshold=[61, 90])
```

Later in the game, put only the cards still in hand into `hands`, the cards of the current
trick into `trick` (with `leader` = who led it), and the points the declarer already won in
`base` (plus the skat in `skat`). Values are always final totals (`base` + skat + what is
still to come). For Null, `solve` returns 1 (declarer wins) or 0.

`skatdds.cards` has the notation helpers and the trick rules (`legal`, `trick_winner`,
`points`); `skatdds.sim` makes random positions.

## C API

See [`include/skatdds.h`](include/skatdds.h). Cards are ints `suit * 8 + rank` (suits D H S C
= 0..3, ranks 7 8 9 10 J Q K A = 0..7), hands are 32-bit masks. One call solves a batch:

```c
uint64_t skat_solve_batch(int n, const uint32_t* hands, const int8_t* trick,
                          const int8_t* leader, const int8_t* decl, const int8_t* game,
                          const int16_t* base, int mode, const int16_t* thr, int obj,
                          int16_t* out, uint64_t* nodes, int nthreads);
```

Modes: `SKAT_EXACT`, `SKAT_THRESHOLD` (1 if value >= thr), `SKAT_MOVE_VALUES` and
`SKAT_MOVE_THRESHOLD` (32 outputs per position, indexed by card, -1 = illegal). Objective
`SKAT_OBJ_POINTS` or `SKAT_OBJ_TRICKS`. A full example is in
[`examples/example.c`](examples/example.c) (`make build/example && ./build/example`).

## How it works

What is in [`src/skatdds.cpp`](src/skatdds.cpp):

- **Search.** Declarer (max) against the two defenders as one team (min). Fail-soft
  alpha-beta with principal variation search: the first move gets the full window, the
  others a null-window probe and a re-search only if they might be better. Exact values come
  from one full-window search (MTD(f) is available as an option, but slower here); threshold
  queries are a single null-window search. All pruning is exact.
- **Card mapping.** Per game type, cards are renumbered so that each effective suit (trumps
  including the jacks, or the plain suits) is a contiguous bit range ordered by trick-taking
  power. "Beats" becomes an index compare, legal moves a mask operation.
- **Equivalent cards.** Cards of one hand that are neighbours in power (all cards between
  them already played) and have the same point value are searched once.
- **Transposition table.** At trick starts (two or more cards per hand), 4-way buckets of
  64 bytes, 4 MB per thread, storing lower/upper bounds of the value of the *remaining*
  tricks and the best move. The key is normalized: per effective suit, the sequence of
  (owner, point value) of the remaining cards in power order, so positions that differ only
  in which equal-valued cards were already played share an entry. Since only remaining
  value is stored, entries are valid across different move orders and across solves (the
  table persists between calls on the same thread).
- **Bounds at trick starts.** Remaining points (upper bound); the top trump run (the
  highest trumps that all belong to one side are sure for that side, whoever leads); the
  leader's sure tricks (top trumps in sequence, then top side cards once the opponents have
  no trumps left, plus the trumps the opponents must follow with).
- **Quasi-symmetry bounds.** Two rank-neighbouring cards of one hand lead to the same trick
  outcomes, so their values differ by at most their point difference (cf. Kupferschmid &
  Helmert's work on Skat). A searched card's result bounds its neighbour, which is often
  enough to skip it.
- **Move ordering.** Hand-written heuristics per trick position (cash sure winners, pull
  trumps as declarer, lead to partner's top card, smear points on partner's trick, duck
  low...), the TT move first, and a history heuristic.
- **Endgame.** The last three tricks are solved by lean nested-loop alpha-beta routines
  without table or move ordering; when a trick ends and a bound already decides the rest,
  the search returns without entering the next trick.
- **Threshold queries per card** reuse a root-move bounds cache, so asking the same position
  at several targets (61, then 90, then 31) does not repeat work already decided.
- **Batches** can be split over several threads, each with its own table.

The ablation flags in the header (`skat_set_options`) switch each of these off for
benchmarking; they never change results.

## Benchmarks

Apple M2 Max (12 cores), macOS (Darwin 25.4), Apple clang 21.0.0,
`-O3 -march=native -std=c++17`, called from Python 3.9 via ctypes. **One thread.** The machine
was busy with other jobs during the run (load average 23-30 on 12 cores), so these numbers are
on the pessimistic side.

Positions: random deals with a plausible contract (declarer = the seat whose hand + skat is
strongest for the game class; weak deals redrawn; the skat picked up and two cards discarded
by a simple rule). Forehand leads. Mid-game positions are reached by random legal play from
the same deals. One position per C call; the table is cleared at the start of each row.
Reproduce with `python bench/bench.py --n 200 --movevals 50 --ablate 30` (about 90 s).

| positions | n | mean ms | median ms | p95 ms | solves/s | nodes (mean) | result |
|---|---:|---:|---:|---:|---:|---:|---|
| Suit, full deal, exact | 200 | 74.20 | 30.60 | 294.97 | 13 | 569,082 | mean 76.6 pts, wins 85% |
| Suit, full deal, threshold >= 61 | 200 | 25.39 | 5.96 | 106.64 | 39 | 207,886 | yes 85% |
| Suit, full deal, threshold >= 90 | 200 | 4.40 | 0.09 | 26.64 | 227 | 38,329 | yes 22% |
| Suit, full deal, schwarz (10 tricks) | 200 | 0.03 | 0.01 | 0.04 | 33,159 | 45 | yes 0% |
| Suit, full deal, value of each lead | 50 | 159.47 | 92.78 | 638.67 | 6 | 1,256,622 |  |
| Suit, full deal, each lead >= 61 | 50 | 45.50 | 18.50 | 156.71 | 22 | 377,882 |  |
| Suit, after 3 tricks, exact | 200 | 0.71 | 0.35 | 2.43 | 1,413 | 4,007 |  |
| Suit, after 6 tricks, exact | 200 | 0.01 | 0.01 | 0.02 | 91,070 | 12 |  |
| Grand, full deal, exact | 200 | 65.69 | 31.26 | 249.12 | 15 | 534,165 | mean 65.1 pts, wins 55% |
| Grand, full deal, threshold >= 61 | 200 | 8.74 | 0.70 | 61.54 | 114 | 70,957 | yes 55% |
| Grand, full deal, threshold >= 90 | 200 | 0.99 | 0.06 | 4.73 | 1,013 | 9,016 | yes 7% |
| Grand, full deal, schwarz (10 tricks) | 200 | 0.03 | 0.01 | 0.11 | 37,177 | 199 | yes 0% |
| Grand, full deal, value of each lead | 50 | 183.10 | 63.50 | 913.60 | 5 | 1,460,782 |  |
| Grand, full deal, each lead >= 61 | 50 | 39.01 | 6.07 | 193.60 | 26 | 247,871 |  |
| Grand, after 3 tricks, exact | 200 | 0.53 | 0.26 | 1.93 | 1,876 | 3,114 |  |
| Grand, after 6 tricks, exact | 200 | 0.02 | 0.01 | 0.04 | 65,524 | 12 |  |
| Null, full deal, exact | 200 | 2.07 | 0.28 | 9.86 | 483 | 20,160 | declarer wins 13% |
| Null, full deal, value of each lead | 50 | 2.19 | 0.86 | 9.68 | 457 | 25,021 |  |
| Null, after 3 tricks, exact | 200 | 0.07 | 0.02 | 0.32 | 14,228 | 566 |  |
| Null, after 6 tricks, exact | 191 | 0.01 | 0.01 | 0.01 | 115,147 | 6 |  |

Suit full deals, exact, one batch on 2 threads: 24.6 solves/s (vs 13 on one thread).

Takeaways: a full suit or Grand deal takes tens of milliseconds for the exact value, a
"does the declarer win?" query is 3-8x faster, and from the middle of the game on solves are
sub-millisecond. Run times are skewed: the median is well below the mean.

Ablation, 30 suit deals, exact values (identical results in every variant):

| variant | mean ms | nodes (mean) |
|---|---:|---:|
| all on | 39.99 | 364,046 |
| no equal-card merging | 40.60 | 373,895 |
| no move-ordering heuristic | 273.95 | 2,021,598 |
| no history heuristic | 56.90 | 487,025 |
| TT keyed by exact cards (not normalized) | 40.07 | 408,560 |
| no sure-points / trump-run bounds | 43.10 | 376,094 |
| no quasi-symmetry bounds | 43.33 | 443,762 |
| no last-3 / last-2 endgame solvers | 61.43 | 512,433 |
| MTD(f) instead of PVS | 137.40 | 1,238,112 |

## Tests

```sh
make test PYTHON=python3        # needs pytest
```

- `tests/test_bruteforce.py`: 240 random endgames (2-4 tricks left, also mid-trick, all six
  game types) against a plain Python minimax: exact values, value of every card, legal
  moves, threshold answers.
- `tests/test_reference.py`: 300 random positions (full deals and mid-game, all game types)
  with fixed answers for exact, threshold, per-card values and the tricks objective.
- `tests/test_basic.py`: notation, trick rules, input checks and small hand-checkable cases.

`tools/compare.py --ref other/libskatdds.dylib` checks two builds against each other
(answers and node counts).

## Limitations

- **Double-dummy is not real play.** Real players do not see each other's cards. Double-dummy
  values are a strong signal (and a standard tool for analysis and for Monte Carlo players
  that sample the hidden cards), but they are not what a human or a real bot achieves.
- **Card points, not game score.** The value is card points (or tricks). Game value, bids,
  matadors, overbidding, schneider/schwarz announcements and the Seeger-Fabian score are not
  computed. Schneider is `can_reach(pos, 90)`; schwarz is a separate query
  (`objective="tricks"`, target = all tricks), and the points objective does not prefer
  schwarz over a non-schwarz line with the same points.
- **Card play only.** No bidding, no skat pick-up or discard decisions: the caller gives the
  declarer's hand after discarding and the discards as `skat`. Ramsch and other variants with
  no single declarer are not supported. Null ouvert plays like Null (all cards are visible
  anyway).
- **Null positions must still be alive.** If the Null declarer has already taken a trick the
  game is over; the solver only looks at the remaining tricks.
- The C API does not validate input (the Python `Position` does): overlapping hands or wrong
  hand sizes give undefined results.
- The ablation flags are process-global; set them only while no other thread is solving.

## License

MIT, see [LICENSE](LICENSE). Some design ideas (null-window search, a bucketed table with
bounds and best move, equal-card merging) are standard in bridge double-dummy solvers such as
[DDS](https://github.com/dds-bridge/dds) by Bo Haglund and Soren Hein; no code is taken from
DDS or any other project.
