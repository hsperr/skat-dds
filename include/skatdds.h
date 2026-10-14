/* skatdds: double-dummy (perfect information) solver for Skat.
 * Copyright (c) 2026 Henning Sperr. MIT License, see LICENSE.
 *
 * Card encoding: card = suit * 8 + rank, a set of cards is a uint32 bit mask.
 *   suit 0 Diamonds, 1 Hearts, 2 Spades, 3 Clubs
 *   rank 0 '7', 1 '8', 2 '9', 3 '10', 4 'J', 5 'Q', 6 'K', 7 'A'
 * Players are 0, 1, 2 in playing order (player p is followed by (p + 1) % 3).
 * Games: 0..3 suit game with that trump suit, 4 Grand, 5 Null.
 *
 * Values are from the declarer's point of view:
 *   suit / Grand: declarer card points at the end of the game = base + points the declarer
 *                 wins from now on with optimal play by all three players (0..120);
 *   Null:         1 if the declarer takes no further trick, else 0.
 *
 * Every solving thread keeps its own transposition table (4 MB) that survives between
 * calls; entries stay valid across positions, so solving related positions in a row is
 * faster. Results never depend on the table contents.
 */
#ifndef SKATDDS_H
#define SKATDDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SKAT_EXACT = 0, SKAT_THRESHOLD = 1, SKAT_MOVE_VALUES = 2, SKAT_MOVE_THRESHOLD = 3 };
enum { SKAT_OBJ_POINTS = 0, SKAT_OBJ_TRICKS = 1 };

/* Solve n positions.
 *   hands   [n*3]  card masks of players 0, 1, 2 (cards still in hand)
 *   trick   [n*2]  cards already played to the current trick, in play order, -1 padded
 *   leader  [n]    player who led the current trick
 *   decl    [n]    declarer
 *   game    [n]    0..5 (see above)
 *   base    [n]    declarer points already secured (skat + tricks taken), NULL = all 0;
 *                  with obj SKAT_OBJ_TRICKS: tricks already taken. Ignored for Null.
 *   mode    SKAT_EXACT          out[i] = value
 *           SKAT_THRESHOLD      out[i] = 1 if value >= thr[i] else 0 (faster than exact)
 *           SKAT_MOVE_VALUES    out[32*i + card] = value after the player to act plays card,
 *                               -1 for cards that are not legal
 *           SKAT_MOVE_THRESHOLD out[32*i + card] = 1/0 (value after card >= thr[i]), -1 illegal
 *   thr     [n]    thresholds for the threshold modes (NULL = 0)
 *   obj     SKAT_OBJ_POINTS (card points) or SKAT_OBJ_TRICKS (declarer tricks, suit/Grand)
 *   out     [n] or [32*n]
 *   nodes   [n]    optional (NULL): search nodes per position
 *   nthreads  1 = calling thread (keeps its table between calls); k > 1 = k worker threads,
 *             each with a fresh 4 MB table
 * Returns the total number of search nodes. */
uint64_t skat_solve_batch(int n, const uint32_t* hands, const int8_t* trick, const int8_t* leader,
                          const int8_t* decl, const int8_t* game, const int16_t* base, int mode,
                          const int16_t* thr, int obj, int16_t* out, uint64_t* nodes, int nthreads);

/* Legal cards (mask) of the player to act. trick: 2 entries, -1 padded. */
uint32_t skat_legal(const uint32_t* hands, const int8_t* trick, int8_t leader, int8_t decl, int8_t game);

/* Clear the calling thread's transposition table. */
void skat_reset(void);

/* Resize (and clear) the calling thread's table: 2^bits buckets of 64 bytes (default 16). */
void skat_set_tt_bits(int bits);

/* Smallest hand size (cards per player at a trick start) that uses the table (default 2). */
void skat_set_tt_min_cards(int n);

/* Ablation flags for benchmarking (global, default 0 = all on). They change speed only,
 * never results. Set them while no other thread is solving. */
enum {
  SKAT_OPT_NO_TT = 1,         /* no transposition table */
  SKAT_OPT_NO_EQUIV = 2,      /* no merging of equivalent cards */
  SKAT_OPT_NO_ORDER = 4,      /* no heuristic move ordering */
  SKAT_OPT_NO_NORM = 16,      /* TT keyed by exact cards instead of the normalized key */
  SKAT_OPT_NO_BOUNDS = 32,    /* no sure-points bounds */
  SKAT_OPT_MTDF = 64,         /* exact values by MTD(f) instead of one PVS search */
  SKAT_OPT_NO_PVS = 128,      /* plain alpha-beta (no null-window probes) */
  SKAT_OPT_NO_RUN = 256,      /* no top-trump-run bound */
  SKAT_OPT_NO_LAST2 = 512,    /* no special solver for the last two tricks */
  SKAT_OPT_NO_RCACHE = 1024,  /* no root-move bounds cache across threshold calls */
  SKAT_OPT_NO_HIST = 2048,    /* no history heuristic */
  SKAT_OPT_NO_LAST3 = 4096,   /* no special alpha-beta for the last three tricks */
  SKAT_OPT_NO_QSYM = 8192     /* no quasi-symmetry bounds */
};
void skat_set_options(int flags);
int skat_get_options(void);

#ifdef __cplusplus
}
#endif

#endif /* SKATDDS_H */
