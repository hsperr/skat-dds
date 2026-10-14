// skatdds: double-dummy (perfect information) solver for Skat.
// Copyright (c) 2026 Henning Sperr. MIT License, see LICENSE.
//
// Declarer (MAX) plays against both defenders as one team (MIN). The search value is the
// declarer's card points from the current node on (cards already on the table in the
// current trick included). For Null it is 1 if the declarer takes no further trick, else 0.
//
// Search: fail-soft alpha-beta with PVS (null-window probes for non-first moves); exact
// values by one full-window PVS search (MTD(f) optional), thresholds by one null-window
// test. All pruning is exact: no null-move or other unsound cuts. Quick bounds at trick
// starts: remaining points, the top trump run of either side, and the leader's sure tricks
// (top trumps in sequence, then top side cards once the opponents are out of trumps).
// Transposition table at trick starts (positions with >= 2 cards per hand), storing bounds of
// the value of the *remaining* tricks (so entries transfer between paths and between solves)
// plus the best move. Key: per effective suit, the sequence of (owner, card points) of the
// remaining cards in power order (played cards are gaps), + leader + game/declarer/objective.
//
// Further: the last three tricks by a lean alpha-beta without TT or ordering
// (last_three_ab / last_two_ab), history heuristic in move ordering, quasi-symmetry bounds
// between rank-interchangeable cards of one hand (values differ by at most their point
// difference, cf. Kupferschmid & Helmert), a root-move bounds cache so repeated threshold
// queries on one position reuse earlier per-card results, trick-end exits without entering
// ab, 4 MB TT per thread.
//
// Cards are remapped into an "ordered space" per game: each effective suit occupies a
// contiguous bit range, sorted by trick-taking power, trumps on top. So "beats" is an index
// compare and equal-card merging is a neighbour test.
//
// Several design ideas (null-window search, bucketed TT with lower/upper bound + best move,
// equal-card merging) are well known from bridge double-dummy solvers such as DDS by Bo
// Haglund and Soren Hein. No code is taken from DDS or any other project.
#include "skatdds.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

enum { GRAND = 4, NULLG = 5, TRUMP = 4 };
enum { OBJ_POINTS = 0, OBJ_TRICKS = 1 };

// option flags (ablations for benchmarking; 0 = everything on)
enum { OPT_NO_TT = 1, OPT_NO_EQUIV = 2, OPT_NO_ORDER = 4, OPT_NO_NORM = 16, OPT_NO_BOUNDS = 32,
       OPT_MTDF = 64,      // exact values by MTD(f) instead of one PVS full-window search
       OPT_NO_PVS = 128,   // plain alpha-beta in wide windows (no null-window probes)
       OPT_NO_RUN = 256,    // no lead-independent top-trump-run bound
       OPT_NO_LAST2 = 512,  // no dedicated solver for the last two tricks
       OPT_NO_RCACHE = 1024,  // no root-move bounds cache across threshold calls
       OPT_NO_HIST = 2048,    // no history heuristic in move ordering
       OPT_NO_LAST3 = 4096,   // no dedicated alpha-beta for the last three tricks
       OPT_NO_QSYM = 8192 };  // no quasi-symmetry bounds between rank-neighbour cards
int g_opts = 0;

inline int popc(uint32_t x) { return __builtin_popcount(x); }
inline int lsb(uint32_t x) { return __builtin_ctz(x); }
inline int msb(uint32_t x) { return 31 - __builtin_clz(x); }
inline uint32_t above(int c) { return ~((2u << c) - 1); }  // bits strictly above c

const int POINTS[8] = {0, 0, 0, 10, 2, 3, 4, 11};  // by rank 7 8 9 T J Q K A

struct GameTables {
  uint8_t to_ord[32], to_orig[32], esuit[32];
  uint32_t smask[5];
  int8_t pts[32];  // card points in ordered space
};
GameTables g_tab[6];

void build_tables() {
  static const int plain_order[7] = {0, 1, 2, 5, 6, 3, 7};      // 7 8 9 Q K T A
  static const int null_order[8] = {0, 1, 2, 3, 4, 5, 6, 7};    // 7 8 9 T J Q K A
  for (int g = 0; g < 6; g++) {
    GameTables& t = g_tab[g];
    memset(&t, 0, sizeof t);
    int idx = 0;
    auto add = [&](int card, int es) {
      t.to_ord[card] = (uint8_t)idx;
      t.to_orig[idx] = (uint8_t)card;
      t.esuit[idx] = (uint8_t)es;
      t.smask[es] |= 1u << idx;
      t.pts[idx] = (int8_t)POINTS[card % 8];
      idx++;
    };
    if (g == NULLG) {
      for (int s = 0; s < 4; s++)
        for (int r : null_order) add(s * 8 + r, s);
    } else {
      for (int s = 0; s < 4; s++) {
        if (s == g) continue;
        for (int r : plain_order) add(s * 8 + r, s);
      }
      if (g < 4)
        for (int r : plain_order) add(g * 8 + r, TRUMP);
      for (int s = 0; s < 4; s++) add(s * 8 + 4, TRUMP);  // jacks D H S C
    }
  }
}

uint64_t splitmix(uint64_t& x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
uint64_t Z[3][32], ZLEAD[3], ZGAME[6][3][2], ZSEQ[5][12][3][12];

inline uint64_t mix64(uint64_t h) {
  h ^= h >> 31;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 29;
  h *= 0x94D049BB133111EBull;
  return h ^ (h >> 32);
}

// compress(x, m): the bits of x at the set positions of m, packed (6-bit chunks by table)
uint8_t g_ctab[64][64];
inline uint32_t compress11(uint32_t x, uint32_t m) {
  return g_ctab[m & 63][x & 63] | (uint32_t)g_ctab[(m >> 6) & 63][(x >> 6) & 63] << popc(m & 63);
}
// g_seq[game][obj][suit][live mask within the suit]: hash of the points of the live cards
// in power order (position-dependent), for the normalized TT key.
std::vector<uint64_t> g_seq[6][2][5];

struct InitAll {
  InitAll() {
    build_tables();
    for (int m = 0; m < 64; m++)
      for (int x = 0; x < 64; x++) {
        int out = 0, k = 0;
        for (int b = 0; b < 6; b++)
          if (m >> b & 1) out |= ((x >> b) & 1) << k++;
        g_ctab[m][x] = (uint8_t)out;
      }
    for (int g = 0; g < 6; g++)
      for (int o = 0; o < 2; o++)
        for (int e = 0; e < 5; e++) {
          uint32_t sm = g_tab[g].smask[e];
          int len = __builtin_popcount(sm), lo = sm ? __builtin_ctz(sm) : 0;
          auto& v = g_seq[g][o][e];
          v.assign(size_t(1) << len, 0);
          for (uint32_t m = 0; m < (1u << len); m++) {
            uint64_t h = 0x1234567ull + (uint64_t)e;
            int pos = 0;
            for (int b = 0; b < len; b++)
              if (m >> b & 1) {
                int pts = (o == 0 && g != NULLG) ? g_tab[g].pts[lo + b] : 0;
                h = mix64(h ^ ((uint64_t)(pts + 1) << 8 | (uint64_t)pos++));
              }
            v[m] = h;
          }
        }
    uint64_t s = 12345;
    for (auto& a : Z)
      for (auto& v : a) v = splitmix(s);
    for (auto& v : ZLEAD) v = splitmix(s);
    for (auto& a : ZGAME)
      for (auto& b : a)
        for (auto& v : b) v = splitmix(s);
    for (auto& a : ZSEQ)
      for (auto& b : a)
        for (auto& c : b)
          for (auto& v : c) v = splitmix(s);
  }
} g_init;

// ------------------------------------------------------------------ transposition table
struct TTEntry {
  uint64_t key;
  int8_t lb, ub;
  int8_t best;    // ordered card index, -1 none
  uint8_t depth;  // tricks left
  uint32_t gen;
};
struct alignas(64) Bucket {
  TTEntry e[4];
};

struct Move {
  int card;
  int score;
};

struct Solver {
  // tables
  const GameTables* gt = nullptr;
  int8_t cval[32];
  int game = 0, decl = 0, obj = OBJ_POINTS, is_null = 0;
  uint64_t zg = 0;
  // state
  uint32_t hand[3] = {0, 0, 0};
  int leader = 0, to_act = 0;
  int tbl[3] = {0, 0, 0}, ntbl = 0;
  uint32_t tblmask = 0;
  uint64_t hash = 0;
  int rem = 0;  // sum of cval over hands
  uint8_t bpts[4][256];  // cval sum per byte of an ordered-space mask
  uint64_t shash[5];     // normalized key per effective suit (see norm_key)
  int slo[5];            // first ordered index of each effective suit
  uint32_t smaskn[5];    // (1 << suit length) - 1
  const uint64_t* seqh[5];  // g_seq tables of this game/objective
  uint32_t dirty = 31;   // suits whose shash must be recomputed
  // TT
  std::vector<Bucket> tt;
  uint32_t tt_mask = 0;
  uint32_t gen = 1;
  uint64_t nodes = 0;
  // Root-move bounds cache (threshold modes): per root position, bounds of the value after
  // each legal card from earlier threshold searches. A caller that asks the same position
  // at several thresholds (e.g. 61, then 90 and 31) does not search cards again that an
  // earlier fail-soft result already decided.
  struct RootEntry {
    uint64_t key;
    int8_t lb[32], ub[32];
  };
  std::vector<RootEntry> rcache = std::vector<RootEntry>(1 << 12);
  uint64_t root_key = 0;
  uint32_t hist[3][2][32] = {};
  int tt_min_cards = 2;  // no TT for positions with fewer cards per hand at trick start

  Solver(int log2_buckets = 16) { resize_tt(log2_buckets); }

  void resize_tt(int log2_buckets) {
    tt.assign(size_t(1) << log2_buckets, Bucket{});
    tt_mask = (1u << log2_buckets) - 1;
    gen = 1;
  }

  // hands in original encoding, trick cards in original encoding (count ntrick), leader
  void setup(const uint32_t* h, const int* trick, int ntrick, int lead, int dcl, int g, int objective) {
    game = g;
    is_null = g == NULLG;
    decl = dcl;
    obj = is_null ? OBJ_TRICKS : objective;
    gt = &g_tab[g];
    for (int i = 0; i < 32; i++) cval[i] = (obj == OBJ_POINTS) ? gt->pts[i] : 0;
    for (int k = 0; k < 4; k++)
      for (int b = 0; b < 256; b++) {
        int s = 0;
        for (int j = 0; j < 8; j++)
          if (b >> j & 1) s += cval[8 * k + j];
        bpts[k][b] = (uint8_t)s;
      }
    dirty = 31;
    for (int e = 0; e < 5; e++) {
      uint32_t sm = gt->smask[e];
      slo[e] = sm ? lsb(sm) : 0;
      smaskn[e] = (1u << popc(sm)) - 1;
      seqh[e] = g_seq[g][obj][e].data();
    }
    zg = ZGAME[g][dcl][obj];
    hash = 0;
    rem = 0;
    for (int p = 0; p < 3; p++) {
      hand[p] = 0;
      uint32_t m = h[p];
      while (m) {
        int c = lsb(m);
        m &= m - 1;
        int o = gt->to_ord[c];
        hand[p] |= 1u << o;
        hash ^= Z[p][o];
        rem += cval[o];
      }
    }
    leader = lead;
    ntbl = 0;
    tblmask = 0;
    for (int i = 0; i < ntrick; i++) {
      tbl[ntbl++] = gt->to_ord[trick[i]];
      tblmask |= 1u << tbl[i];
    }
    to_act = (leader + ntbl) % 3;
    gen++;
    memset(hist, 0, sizeof hist);
    uint64_t rk = hash ^ zg ^ ZLEAD[leader];
    for (int i = 0; i < ntbl; i++) rk = mix64(rk ^ (uint64_t)(tbl[i] + 1 + 64 * i));
    root_key = mix64(rk ^ 0x5bd1e995ull) | 1;
  }

  // value of a completed trick for the declarer
  inline int trick_val() const {
    if (obj == OBJ_TRICKS) return 1;
    return cval[tbl[0]] + cval[tbl[1]] + cval[tbl[2]];
  }
  inline int upper_rem() const {  // max remaining value at a trick start
    if (is_null) return 1;
    if (obj == OBJ_TRICKS) return popc(hand[0]);
    return rem;
  }
  // does card a beat card w given lead suit es_lead
  inline bool beats(int a, int w, int es_lead) const {
    int ea = gt->esuit[a], ew = gt->esuit[w];
    if (ea == ew) return a > w;
    if (ea == TRUMP) return true;
    (void)es_lead;
    return false;
  }
  inline int winner_idx() const {  // index in tbl of the trick winner (ntbl >= 1)
    int w = 0;
    for (int i = 1; i < ntbl; i++)
      if (beats(tbl[i], tbl[w], gt->esuit[tbl[0]])) w = i;
    return w;
  }
  inline uint32_t legal(int p) const {
    if (ntbl == 0) return hand[p];
    uint32_t f = hand[p] & gt->smask[gt->esuit[tbl[0]]];
    return f ? f : hand[p];
  }
  // can player q (following lead suit L) beat card w?
  inline bool can_beat(int q, int w, int L) const {
    uint32_t f = hand[q] & gt->smask[L];
    int ew = gt->esuit[w];
    if (f) {
      if (ew != L) return false;  // w is a trump, q must follow a plain suit
      return (f & ~((2u << w) - 1)) != 0;
    }
    uint32_t tr = hand[q] & gt->smask[TRUMP];
    if (ew == TRUMP) return (tr & ~((2u << w) - 1)) != 0;
    return tr != 0;
  }

  // ---------------------------------------------------------------- move generation
  // Legal moves of player p, equal cards merged (representative = lowest of a run).
  int gen_moves(int p, Move* mv, uint32_t L) const {
    int n = 0;
    uint32_t live = hand[0] | hand[1] | hand[2] | tblmask;
    bool merge = !(g_opts & OPT_NO_EQUIV);
    uint32_t m = L;
    while (m) {
      int c = lsb(m);
      m &= m - 1;
      if (merge) {
        uint32_t lower = live & gt->smask[gt->esuit[c]] & ((1u << c) - 1);
        if (lower) {
          int d = msb(lower);
          if ((hand[p] >> d & 1) && cval[d] == cval[c]) continue;
        }
      }
      mv[n].card = c;
      mv[n].score = 0;
      n++;
    }
    return n;
  }

  inline int side(int p) const { return p == decl ? 0 : 1; }

  // Trick context shared by the ordering heuristics of one node.
  struct TrickCtx {
    int L, w, nx;
    bool mine, nx_partner, nx_beats_w;
  };

  void order_moves(int p, Move* mv, int n, int ttbest) {
    if (n <= 1) return;
    bool heur = !(g_opts & OPT_NO_ORDER);
    uint32_t live = hand[0] | hand[1] | hand[2] | tblmask;
    TrickCtx tc{};
    if (heur && !is_null && ntbl > 0) {
      tc.L = gt->esuit[tbl[0]];
      int wi = winner_idx();
      tc.w = tbl[wi];
      tc.mine = side((leader + wi) % 3) == side(p);
      tc.nx = (p + 1) % 3;
      tc.nx_partner = side(tc.nx) == side(p);
      tc.nx_beats_w = ntbl == 1 && can_beat(tc.nx, tc.w, tc.L);
    }
    for (int i = 0; i < n; i++) {
      int c = mv[i].card;
      int s = 0;
      if (heur) s = is_null ? -(c & 7) : heur_pts(p, c, live, tc);
      if (!(g_opts & OPT_NO_HIST)) {
        // history: cards that refuted earlier siblings rank first at trick starts, and break
        // ties of the heuristic elsewhere
        if (ntbl == 0)
          s = (int)std::min(hist[0][side(p)][c], 100000u) * 256 + s * 4096 + std::min(hist[0][side(p)][c], 4095u) + (128 << 12);
        else
          s = s * 4096 + std::min(hist[ntbl][side(p)][c], 4095u);
      }
      if (c == ttbest) s = 1 << 30;
      mv[i].score = s;
    }
    for (int i = 1; i < n; i++) {
      Move x = mv[i];
      int j = i - 1;
      while (j >= 0 && mv[j].score < x.score) {
        mv[j + 1] = mv[j];
        j--;
      }
      mv[j + 1] = x;
    }
  }

  int heur_pts(int p, int c, uint32_t live, const TrickCtx& tc) const {
    int sp = side(p);
    int e = gt->esuit[c];
    int v = cval[c];
    if (ntbl == 0) {
      bool top = (live & gt->smask[e] & above(c) & ~hand[p]) == 0;
      if (top) {
        // could an opponent ruff?
        bool ruff = false;
        if (e != TRUMP)
          for (int q = 0; q < 3; q++)
            if (side(q) != sp && !(hand[q] & gt->smask[e]) && (hand[q] & gt->smask[TRUMP])) ruff = true;
        if (!ruff) return 60 + v;
        return 10 - v;
      }
      if (sp == 0 && e == TRUMP) return 20 - v;  // declarer pulls trumps
      if (sp == 1) {
        // defender: lead low to a partner who holds the top card of the suit
        int partner = 3 - p - decl;
        uint32_t s_live = live & gt->smask[e];
        bool ruff = e != TRUMP && !(hand[decl] & gt->smask[e]) && (hand[decl] & gt->smask[TRUMP]);
        if ((hand[partner] >> msb(s_live) & 1) && !ruff) return 40 - v;
      }
      return 20 - 2 * v - (c & 7);
    }
    bool b = beats(c, tc.w, tc.L);
    if (ntbl == 2) {
      if (b) return tc.mine ? 40 + v - 8 : 60 + v;  // overtaking partner wastes a winner
      if (tc.mine) return 50 + v;                     // smear onto partner's trick
      return -2 * v - (c & 7);                        // dump low
    }
    if (b) {
      if (tc.nx_partner || !can_beat(tc.nx, c, tc.L)) return 55 + v;  // trick is ours
      return 5 - v;
    }
    if (tc.nx_partner) return tc.nx_beats_w ? 35 + v : -2 * v - (c & 7);  // partner plays last
    if (tc.mine && !tc.nx_beats_w) return 45 + v;  // partner wins: smear
    return -2 * v - (c & 7);
  }

  inline int pts_of(uint32_t m) const {
    return bpts[0][m & 255] + bpts[1][(m >> 8) & 255] + bpts[2][(m >> 16) & 255] + bpts[3][m >> 24];
  }

  // Cheap upper limit of sure_points(L): the leader's cards plus the opponents' trumps.
  inline int sure_cap(int L) const {
    uint32_t opp = L == decl ? (hand[0] | hand[1] | hand[2]) & ~hand[L] : hand[decl];
    return pts_of(hand[L] | (opp & gt->smask[TRUMP]));
  }

  // Points of the top trump run: the highest live trumps that all belong to one side (no
  // matter who leads). Each of them wins the trick it is played in, or falls in a trick won
  // by its own side, so the run's points are sure for that side. side_out: 0 decl, 1 def.
  inline int trump_run(int& side_out) const {
    uint32_t T = (hand[0] | hand[1] | hand[2]) & gt->smask[TRUMP];
    side_out = -1;
    if (!T) return 0;
    uint32_t d = hand[decl] & T, other = T & ~d;
    if (d >> msb(T) & 1) {  // declarer owns the top trump
      side_out = 0;
      return pts_of(other ? d & ~((2u << msb(other)) - 1) : d);
    }
    side_out = 1;
    return pts_of(d ? other & ~((2u << msb(d)) - 1) : other);
  }

  // Points the leader's side surely wins from now on (trick start, leader L): the leader's
  // top trumps in sequence (each wins and keeps the lead), and, if that strips every opponent
  // of trumps, the leader's top cards in sequence of each plain suit. Counts the leader's
  // own cards plus the opponents' forced trump follows (their cheapest trumps).
  int sure_points(int L) const {
    const uint32_t T = gt->smask[TRUMP];
    uint32_t mine = hand[L];
    uint32_t live = hand[0] | hand[1] | hand[2];
    auto top_seq = [&](uint32_t suit) -> uint32_t {
      uint32_t others = live & suit & ~mine;
      uint32_t m = mine & suit;
      if (!others) return m;
      return m & ~((2u << msb(others)) - 1);
    };
    uint32_t st = top_seq(T);
    int k1 = popc(st);
    int pts = pts_of(st);
    bool stripped = true;
    for (int q = 0; q < 3; q++) {
      if (q == L || (side(q) == side(L))) continue;
      uint32_t tq = hand[q] & T;
      int n = popc(tq);
      if (n > k1) stripped = false;
      // the opponent follows k1 trump leads with (at least) its min(k1, n) cheapest trumps
      int need = std::min(k1, n);
      if (need) {
        int v[11], cnt = 0;
        for (uint32_t m = tq; m; m &= m - 1) v[cnt++] = cval[lsb(m)];
        std::sort(v, v + cnt);
        for (int i = 0; i < need; i++) pts += v[i];
      }
    }
    if (stripped)
      for (int e = 0; e < 4; e++) pts += pts_of(top_seq(gt->smask[e]));
    return pts;
  }

  // ---------------------------------------------------------------- TT
  // Key of the position up to renaming of cards: per effective suit, the sequence of
  // (owner, value) of the remaining cards in power order. Cards already played are gaps, so
  // positions that differ only in which equal-valued cards are gone share one entry.
  // Per-suit parts are cached in shash[] and recomputed only for suits touched since
  // (play/unplay mark a suit dirty).
  // Table version: per suit, (hash of the point sequence of the live cards) mixed with the
  // owner bits of hands 1 and 2 compressed to the live cards.
  uint64_t norm_key() {
    uint32_t h1 = hand[1], h2 = hand[2], live = hand[0] | h1 | h2;
    for (uint32_t d = dirty; d; d &= d - 1) {
      int e = lsb(d);
      int lo = slo[e];
      uint32_t lm = smaskn[e];
      uint32_t m = (live >> lo) & lm;
      uint32_t c1 = compress11(((h1 >> lo) & lm), m), c2 = compress11(((h2 >> lo) & lm), m);
      shash[e] = mix64(seqh[e][m] ^ ((uint64_t)c1 | (uint64_t)c2 << 11 | (uint64_t)e << 22) * 0x9E3779B97F4A7C15ull);
    }
    dirty = 0;
    return shash[0] ^ shash[1] ^ shash[2] ^ shash[3] ^ shash[4];
  }

  TTEntry* probe(uint64_t key) {
    Bucket& b = tt[key & tt_mask];
    for (auto& e : b.e)
      if (e.key == key) return &e;
    return nullptr;
  }
  void store(uint64_t key, int lb, int ub, int best, int depth) {
    Bucket& b = tt[key & tt_mask];
    TTEntry* victim = nullptr;
    int vscore = 1 << 30;
    for (auto& e : b.e) {
      if (e.key == key) {
        e.lb = (int8_t)std::max<int>(e.lb, lb);
        e.ub = (int8_t)std::min<int>(e.ub, ub);
        if (best >= 0) e.best = (int8_t)best;
        e.gen = gen;
        return;
      }
      int s = e.key == 0 ? -1 : (e.gen == gen ? 64 : 0) + e.depth;
      if (s < vscore) {
        vscore = s;
        victim = &e;
      }
    }
    victim->key = key;
    victim->lb = (int8_t)lb;
    victim->ub = (int8_t)ub;
    victim->best = (int8_t)best;
    victim->depth = (uint8_t)depth;
    victim->gen = gen;
  }

  // ---------------------------------------------------------------- search
  int last_trick() {
    // every hand holds exactly one card; ntbl == 0
    int l = leader;
    tbl[0] = lsb(hand[l]);
    tbl[1] = lsb(hand[(l + 1) % 3]);
    tbl[2] = lsb(hand[(l + 2) % 3]);
    ntbl = 3;
    int wi = winner_idx();
    ntbl = 0;
    int wp = (l + wi) % 3;
    if (is_null) return wp == decl ? 0 : 1;
    if (wp != decl) return 0;
    if (obj == OBJ_TRICKS) return 1;
    return cval[tbl[0]] + cval[tbl[1]] + cval[tbl[2]];
  }

  // Exact value of the last two tricks (two cards per hand, trick start): plain minimax over
  // at most 2x2x2 first tricks; the last trick is forced. No TT, no ordering.
  int last_two() {
    const int l = leader;
    const int pl[3] = {l, (l + 1) % 3, (l + 2) % 3};
    const uint32_t h0 = hand[pl[0]], h1 = hand[pl[1]], h2 = hand[pl[2]];
    const bool max0 = pl[0] == decl, max1 = pl[1] == decl, max2 = pl[2] == decl;
    int best0 = max0 ? -1 : 1000;
    for (uint32_t m0 = h0; m0; m0 &= m0 - 1) {
      int c0 = lsb(m0);
      uint32_t L = gt->smask[gt->esuit[c0]];
      uint32_t f1 = h1 & L ? h1 & L : h1;
      int best1 = max1 ? -1 : 1000;
      for (uint32_t m1 = f1; m1; m1 &= m1 - 1) {
        int c1 = lsb(m1);
        uint32_t f2 = h2 & L ? h2 & L : h2;
        int best2 = max2 ? -1 : 1000;
        for (uint32_t m2 = f2; m2; m2 &= m2 - 1) {
          int c2 = lsb(m2);
          int t[3] = {c0, c1, c2};
          int w = 0;
          if (beats(c1, c0, 0)) w = 1;
          if (beats(c2, t[w], 0)) w = 2;
          int r[3] = {lsb(h0 ^ (1u << c0)), lsb(h1 ^ (1u << c1)), lsb(h2 ^ (1u << c2))};
          int w2 = w;  // index into pl of the last trick's winner (leader is pl[w])
          if (beats(r[(w + 1) % 3], r[w2], 0)) w2 = (w + 1) % 3;
          if (beats(r[(w + 2) % 3], r[w2], 0)) w2 = (w + 2) % 3;
          bool d1 = pl[w] == decl, d2 = pl[w2] == decl;
          int v;
          if (is_null)
            v = !(d1 || d2);
          else if (obj == OBJ_TRICKS)
            v = d1 + d2;
          else
            v = (d1 ? cval[c0] + cval[c1] + cval[c2] : 0) + (d2 ? cval[r[0]] + cval[r[1]] + cval[r[2]] : 0);
          best2 = max2 ? std::max(best2, v) : std::min(best2, v);
        }
        best1 = max1 ? std::max(best1, best2) : std::min(best1, best2);
      }
      best0 = max0 ? std::max(best0, best1) : std::min(best0, best1);
    }
    return best0;
  }

  // Fail-soft alpha-beta over the last three tricks (three cards per hand, trick start),
  // last_two() for the rest. No ordering, no TT inside.
  int last_three_ab(int alpha, int beta) {
    const int l = leader;
    const int pl[3] = {l, (l + 1) % 3, (l + 2) % 3};
    const uint32_t h0 = hand[pl[0]], h1 = hand[pl[1]], h2 = hand[pl[2]];
    const bool max0 = pl[0] == decl, max1 = pl[1] == decl, max2 = pl[2] == decl;
    int best0 = max0 ? -1000 : 1000, a0 = alpha, b0 = beta;
    for (uint32_t m0 = h0; m0; m0 &= m0 - 1) {
      int c0 = lsb(m0);
      uint32_t L = gt->smask[gt->esuit[c0]];
      uint32_t f1 = h1 & L ? h1 & L : h1;
      int best1 = max1 ? -1000 : 1000, a1 = a0, b1 = b0;
      for (uint32_t m1 = f1; m1; m1 &= m1 - 1) {
        int c1 = lsb(m1);
        uint32_t f2 = h2 & L ? h2 & L : h2;
        int best2 = max2 ? -1000 : 1000, a2 = a1, b2 = b1;
        int w01 = beats(c1, c0, 0) ? 1 : 0;
        int cw = w01 ? c1 : c0;
        for (uint32_t m2 = f2; m2; m2 &= m2 - 1) {
          int c2 = lsb(m2);
          int w = beats(c2, cw, 0) ? 2 : w01;
          int wp = pl[w];
          int v;
          if (is_null && wp == decl) {
            v = 0;
          } else {
            int add = wp != decl ? 0 : obj == OBJ_TRICKS ? 1 : cval[c0] + cval[c1] + cval[c2];
            hand[pl[0]] = h0 ^ (1u << c0);
            hand[pl[1]] = h1 ^ (1u << c1);
            hand[pl[2]] = h2 ^ (1u << c2);
            leader = wp;
            v = add + last_two_ab(a2 - add, b2 - add);
          }
          if (max2) {
            if (v > best2) best2 = v, a2 = std::max(a2, v);
          } else if (v < best2) best2 = v, b2 = std::min(b2, v);
          if (a2 >= b2) break;
        }
        if (max1) {
          if (best2 > best1) best1 = best2, a1 = std::max(a1, best2);
        } else if (best2 < best1) best1 = best2, b1 = std::min(b1, best2);
        if (a1 >= b1) break;
      }
      if (max0) {
        if (best1 > best0) best0 = best1, a0 = std::max(a0, best1);
      } else if (best1 < best0) best0 = best1, b0 = std::min(b0, best1);
      if (a0 >= b0) break;
    }
    hand[pl[0]] = h0, hand[pl[1]] = h1, hand[pl[2]] = h2;
    leader = l;
    return best0;
  }

  // last_two() with fail-soft alpha-beta cutoffs.
  int last_two_ab(int alpha, int beta) {
    const int l = leader;
    const int pl[3] = {l, (l + 1) % 3, (l + 2) % 3};
    const uint32_t h0 = hand[pl[0]], h1 = hand[pl[1]], h2 = hand[pl[2]];
    const bool max0 = pl[0] == decl, max1 = pl[1] == decl, max2 = pl[2] == decl;
    int best0 = max0 ? -1000 : 1000, a0 = alpha, b0 = beta;
    for (uint32_t m0 = h0; m0; m0 &= m0 - 1) {
      int c0 = lsb(m0);
      int e0 = gt->esuit[c0];
      uint32_t L = gt->smask[e0];
      uint32_t f1 = h1 & L ? h1 & L : h1;
      int r0 = lsb(h0 ^ (1u << c0));
      int best1 = max1 ? -1000 : 1000, a1 = a0, b1 = b0;
      for (uint32_t m1 = f1; m1; m1 &= m1 - 1) {
        int c1 = lsb(m1);
        uint32_t f2 = h2 & L ? h2 & L : h2;
        int r1 = lsb(h1 ^ (1u << c1));
        int w01 = beats(c1, c0, 0) ? 1 : 0;
        int cw = w01 ? c1 : c0;
        int best2 = max2 ? -1000 : 1000, a2 = a1, b2 = b1;
        for (uint32_t m2 = f2; m2; m2 &= m2 - 1) {
          int c2 = lsb(m2);
          int w = beats(c2, cw, 0) ? 2 : w01;
          int r[3] = {r0, r1, lsb(h2 ^ (1u << c2))};
          int w2 = w;
          if (beats(r[(w + 1) % 3], r[w2], 0)) w2 = (w + 1) % 3;
          if (beats(r[(w + 2) % 3], r[w2], 0)) w2 = (w + 2) % 3;
          bool d1 = pl[w] == decl, d2 = pl[w2] == decl;
          int v;
          if (is_null)
            v = !(d1 || d2);
          else if (obj == OBJ_TRICKS)
            v = d1 + d2;
          else
            v = (d1 ? cval[c0] + cval[c1] + cval[c2] : 0) + (d2 ? cval[r[0]] + cval[r[1]] + cval[r[2]] : 0);
          if (max2) {
            if (v > best2) best2 = v, a2 = std::max(a2, v);
          } else if (v < best2) best2 = v, b2 = std::min(b2, v);
          if (a2 >= b2) break;
        }
        if (max1) {
          if (best2 > best1) best1 = best2, a1 = std::max(a1, best2);
        } else if (best2 < best1) best1 = best2, b1 = std::min(b1, best2);
        if (a1 >= b1) break;
      }
      if (max0) {
        if (best1 > best0) best0 = best1, a0 = std::max(a0, best1);
      } else if (best1 < best0) best0 = best1, b0 = std::min(b0, best1);
      if (a0 >= b0) break;
    }
    return best0;
  }

  // value after player to_act plays c
  int play(int c, int alpha, int beta) {
    int p = to_act;
    uint32_t bit = 1u << c;
    hand[p] ^= bit;
    hash ^= Z[p][c];
    rem -= cval[c];
    dirty |= 1u << gt->esuit[c];
    tbl[ntbl++] = c;
    tblmask |= bit;
    int v;
    if (ntbl < 3) {
      to_act = (p + 1) % 3;
      v = ab(alpha, beta);
      to_act = p;
    } else {
      int wi = winner_idx();
      int wp = (leader + wi) % 3;
      int t0 = tbl[0], t1 = tbl[1], t2 = tbl[2];
      int add = (wp == decl) ? trick_val() : 0;
      if (!is_null) {  // ab's first exits at the next trick start, without entering it
        int ub;
        if (hand[wp] == 0) v = add;
        else if ((ub = upper_rem()) <= alpha - add) v = add + ub;
        else if (beta - add <= 0) v = add;
        else goto full;
        nodes++;
        goto done;
      }
    full:
      {
      int old_leader = leader;
      uint32_t old_mask = tblmask;
      ntbl = 0;
      tblmask = 0;
      leader = to_act = wp;
      if (is_null)
        v = (wp == decl) ? 0 : ab(alpha, beta);
      else
        v = add + ab(alpha - add, beta - add);
      leader = old_leader;
      to_act = p;
      tblmask = old_mask;
      tbl[0] = t0, tbl[1] = t1, tbl[2] = t2;
      ntbl = 3;
      }
    }
  done:
    ntbl--;
    tblmask ^= bit;
    rem += cval[c];
    hash ^= Z[p][c];
    hand[p] ^= bit;
    dirty |= 1u << gt->esuit[c];
    return v;
  }

  // Cards of p's hand that are rank-interchangeable with c: the live cards of c's suit
  // adjacent to c in rank, as far as they are all in p's hand (c excluded).
  uint32_t qsym_run(int p, int c, uint32_t live) const {
    uint32_t sm = gt->smask[gt->esuit[c]] & live, run = 0;
    for (uint32_t m = sm & ((1u << c) - 1); m;) {
      int d = msb(m);
      if (!(hand[p] >> d & 1)) break;
      run |= 1u << d;
      m ^= 1u << d;
    }
    for (uint32_t m = sm & above(c); m; m &= m - 1) {
      int d = lsb(m);
      if (!(hand[p] >> d & 1)) break;
      run |= 1u << d;
    }
    return run;
  }

  int ab(int alpha, int beta) {
    nodes++;
    int p = to_act;
    uint64_t key = 0;
    int ttbest = -1;
    bool at_start = ntbl == 0;
    bool use_tt = at_start && !(g_opts & OPT_NO_TT);
    if (at_start) {
      if (hand[p] == 0) return is_null ? 1 : 0;
      int ub = upper_rem();
      if (ub <= alpha) return ub;
      if (beta <= 0) return 0;
      if ((hand[p] & (hand[p] - 1)) == 0) return last_trick();
      if (popc(hand[p]) == 2 && !(g_opts & OPT_NO_LAST2)) return last_two_ab(alpha, beta);
      use_tt = use_tt && popc(hand[p]) >= tt_min_cards;
      if (use_tt) {
        key = ((g_opts & OPT_NO_NORM) ? hash : norm_key()) ^ ZLEAD[leader] ^ zg;
        if (!key) key = 1;
        __builtin_prefetch(&tt[key & tt_mask]);
      }
      if (!is_null && obj == OBJ_POINTS && !(g_opts & OPT_NO_BOUNDS)) {
        int lb = 0, dsure = 0;  // sure points of declarer / defenders
        if (!(g_opts & OPT_NO_RUN)) {
          int rs, run = trump_run(rs);
          (rs == 0 ? lb : dsure) = run;
        }
        if (p == decl) {
          if (sure_cap(p) >= beta) lb = std::max(lb, sure_points(p));
        } else if (ub - sure_cap(p) <= alpha) {
          dsure = std::max(dsure, sure_points(p));
        }
        if (lb >= beta) return lb;
        if (ub - dsure <= alpha) return ub - dsure;
      }
      if (use_tt) {
        TTEntry* e = probe(key);
        if (e) {
          if (e->lb >= beta) return e->lb;
          if (e->ub <= alpha || e->lb == e->ub) return e->ub;
          if (e->lb > alpha) alpha = e->lb;
          if (e->ub < beta) beta = e->ub;
          // best move is stored as its rank among the live cards (survives renaming)
          if (e->best >= 0) {
            uint32_t live = hand[0] | hand[1] | hand[2];
            for (int r = e->best; r > 0 && live; r--) live &= live - 1;
            if (live) ttbest = lsb(live);
          }
        }
      }
    }
    if (at_start && !(g_opts & OPT_NO_LAST3) && popc(hand[p]) == 3) {
      int v = last_three_ab(alpha, beta);
      if (use_tt) store(key, v > alpha ? v : 0, v < beta ? v : 127, -1, 3);
      return v;
    }
    Move mv[12];
    int n = gen_moves(p, mv, legal(p));
    order_moves(p, mv, n, ttbest);
    bool maxn = p == decl;
    int best = maxn ? -1000 : 1000, bestc = -1;
    int a = alpha, b = beta;
    bool qsym = !(g_opts & OPT_NO_QSYM) && !is_null && obj == OBJ_POINTS && n > 1;
    uint32_t qdone = 0, qlive = hand[0] | hand[1] | hand[2] | tblmask;
    int8_t qlb[32], qub[32];
    for (int i = 0; i < n; i++) {
      int c = mv[i].card;
      int v;
      if (qsym) {
        // quasi-symmetry: a rank-neighbour d of c in the same hand (nothing live between)
        // gives the same trick outcomes; values differ by at most |pts(c) - pts(d)|.
        // The same holds for any d of c's run: live cards of the suit adjacent in rank, all
        // in p's hand.
        uint32_t run = qsym_run(p, c, qlive);
        bool skip = false;
        for (uint32_t m = run & qdone; m; m &= m - 1) {
          int d = lsb(m);
          int diff = std::abs(cval[c] - cval[d]);
          if (maxn) {
            int u = qub[d] + diff;
            if (u <= a) { skip = true; v = u; }
          } else {
            int l = qlb[d] - diff;
            if (l >= b) { skip = true; v = l; }
          }
          if (skip) break;
        }
        if (skip) {
          if (maxn ? v > best : v < best) best = v;  // bound only; never cuts (outside window)
          continue;
        }
      }
      int lo_w, hi_w;
      if (i == 0 || b - a <= 1 || (g_opts & OPT_NO_PVS)) {
        v = play(c, lo_w = a, hi_w = b);
      } else if (maxn) {  // PVS: prove the move is no better first, re-search if it is
        v = play(c, lo_w = a, hi_w = a + 1);
        if (v > a && v < b) v = play(c, lo_w = v - 1, hi_w = b);  // v is a lower bound
      } else {
        v = play(c, lo_w = b - 1, hi_w = b);
        if (v < b && v > a) v = play(c, lo_w = a, hi_w = v + 1);  // v is an upper bound
      }
      if (qsym) {
        qdone |= 1u << c;
        qlb[c] = (int8_t)(v > lo_w ? v : 0);
        qub[c] = (int8_t)(v < hi_w ? v : 127);
      }
      if (maxn) {
        if (v > best) {
          best = v;
          bestc = c;
          if (v > a) a = v;
        }
      } else {
        if (v < best) {
          best = v;
          bestc = c;
          if (v < b) b = v;
        }
      }
      if (a >= b) {
        if (i > 0) hist[ntbl][side(p)][c] += popc(hand[p]) * popc(hand[p]);
        break;
      }
    }
    if (use_tt) {
      int lb = 0, ub = 127;
      if (best <= alpha)
        ub = best;
      else if (best >= beta)
        lb = best;
      else
        lb = ub = best;
      int rank = bestc < 0 ? -1 : popc((hand[0] | hand[1] | hand[2] | (1u << bestc)) & ((1u << bestc) - 1));
      store(key, lb, ub, rank, popc(hand[p]));
    }
    return best;
  }

  // ---------------------------------------------------------------- drivers
  int upper_now() const {  // max value from the current node
    if (is_null) return 1;
    if (obj == OBJ_TRICKS) return popc(hand[0] | hand[1] | hand[2]) / 3 + (ntbl ? 1 : 0);
    int s = rem;
    for (int i = 0; i < ntbl; i++) s += cval[tbl[i]];
    return s;
  }

  // MTD(f): null-window tests until the fail-soft bounds meet.
  template <class F>
  int mtdf(F&& f, int guess, int hi) {
    int lo = 0;
    int g = std::max(0, std::min(guess, hi));
    while (lo < hi) {
      int beta = g == lo ? g + 1 : g;
      g = f(beta - 1, beta);
      if (g < beta)
        hi = g;
      else
        lo = g;
    }
    return lo;
  }

  int solve_exact() {
    if (is_null) return ab(0, 1);
    int hi = upper_now();
    if (!(g_opts & OPT_MTDF)) return ab(-1, hi + 1);  // PVS, full window
    return mtdf([&](int a, int b) { return ab(a, b); }, hi * 3 / 5, hi);
  }
  bool solve_thr(int t) {  // future value >= t
    if (t <= 0) return true;
    if (t > upper_now()) return false;
    return ab(t - 1, t) >= t;
  }
  // Per legal card (ordered idx) values; out[c] = -1 for illegal. With thr != NO_THR
  // (future-relative), out[c] is 1/0 for "value after c >= thr" (one null-window test each).
  static constexpr int NO_THR = -100000;
  void move_values(int* out, int guess, int thr = NO_THR) {
    for (int i = 0; i < 32; i++) out[i] = -1;
    if (thr != NO_THR && !is_null) guess = thr;
    int p = to_act;
    uint32_t L = legal(p);
    Move mv[12];
    int n = gen_moves(p, mv, L);
    order_moves(p, mv, n, -1);
    int hi = upper_now();
    RootEntry* re = nullptr;
    if (thr != NO_THR && !is_null && !(g_opts & OPT_NO_RCACHE)) {
      re = &rcache[root_key & (rcache.size() - 1)];
      if (re->key != root_key) {
        re->key = root_key;
        memset(re->lb, 0, sizeof re->lb);
        memset(re->ub, 127, sizeof re->ub);
      }
    }
    for (int i = 0; i < n; i++) {
      int c = mv[i].card;
      int v;
      if (is_null)
        v = play(c, 0, 1);
      else if (re) {  // null-window test unless earlier bounds decide it
        if (!(g_opts & OPT_NO_QSYM)) {  // bounds from a rank-neighbour (see ab)
          uint32_t run = qsym_run(p, c, hand[0] | hand[1] | hand[2] | tblmask);
          for (uint32_t m = run; m; m &= m - 1) {
            int d = lsb(m);
            int diff = std::abs(cval[c] - cval[d]);
            re->lb[c] = (int8_t)std::max<int>(re->lb[c], re->lb[d] - diff);
            re->ub[c] = (int8_t)std::min<int>(re->ub[c], re->ub[d] + diff);
          }
        }
        if (thr <= 0 || re->lb[c] >= thr)
          v = 1;
        else if (thr > hi || re->ub[c] < thr)
          v = 0;
        else {
          int lo = thr - 1, hi = thr;
          int r = play(c, lo, hi);
          if (r > lo) re->lb[c] = (int8_t)std::max<int>(re->lb[c], r);
          if (r < hi) re->ub[c] = (int8_t)std::min<int>(re->ub[c], r);
          v = r >= thr;
        }
      } else if (thr != NO_THR)  // one null-window test: 1 if value >= thr
        v = thr <= 0 ? 1 : thr > hi ? 0 : play(c, thr - 1, thr) >= thr;
      else if (g_opts & OPT_MTDF)
        v = mtdf([&](int a, int b) { return play(c, a, b); }, guess, hi);
      else
        v = play(c, -1, hi + 1);
      if (is_null && thr != NO_THR) v = v >= thr;
      out[c] = v;
      guess = v;
    }
    // expand merged cards: a card takes the value of the next lower live card of its run
    uint32_t live = hand[0] | hand[1] | hand[2] | tblmask;
    uint32_t m = L;
    while (m) {
      int c = lsb(m);
      m &= m - 1;
      if (out[c] >= 0) continue;
      uint32_t lower = live & gt->smask[gt->esuit[c]] & ((1u << c) - 1);
      out[c] = out[msb(lower)];
    }
  }
};

Solver& tls_solver() {
  thread_local Solver s;
  return s;
}

// One state, decoded from the batch arrays.
struct State {
  uint32_t hands[3];
  int trick[2], ntrick, leader, decl, game, base;
};

State read_state(int i, const uint32_t* hands, const int8_t* trick, const int8_t* leader,
                 const int8_t* decl, const int8_t* game, const int16_t* base) {
  State s;
  for (int p = 0; p < 3; p++) s.hands[p] = hands[3 * i + p];
  s.ntrick = 0;
  for (int k = 0; k < 2; k++)
    if (trick[2 * i + k] >= 0) s.trick[s.ntrick++] = trick[2 * i + k];
  s.leader = leader[i];
  s.decl = decl[i];
  s.game = game[i];
  s.base = base ? base[i] : 0;
  return s;
}

// mode 0 exact, 1 threshold, 2 move values, 3 move threshold tests
// (modes 2/3: out has 32 slots per state, -1 illegal)
uint64_t run_one(Solver& S, const State& st, int mode, int thr, int obj, int16_t* out) {
  S.setup(st.hands, st.trick, st.ntrick, st.leader, st.decl, st.game, obj);
  uint64_t n0 = S.nodes;
  int base = S.is_null ? 0 : st.base;
  if (mode == 0) {
    out[0] = (int16_t)(base + S.solve_exact());
  } else if (mode == 1) {
    out[0] = (int16_t)(S.is_null ? (S.solve_exact() >= thr) : S.solve_thr(thr - base));
  } else {
    int v[32];
    int guess = S.is_null ? 1 : S.upper_now() * 3 / 5;
    if (mode == 2)
      S.move_values(v, guess);
    else
      S.move_values(v, guess, S.is_null ? thr : thr - base);
    int add = mode == 2 ? base : 0;
    for (int o = 0; o < 32; o++) {
      int c = S.gt->to_orig[o];
      out[c] = (int16_t)(v[o] < 0 ? -1 : add + v[o]);
    }
  }
  return S.nodes - n0;
}

}  // namespace

extern "C" {

void skat_set_options(int flags) { g_opts = flags; }
// Smallest hand size (cards per player at a trick start) that uses the TT. Default 2.
void skat_set_tt_min_cards(int n) { tls_solver().tt_min_cards = n; }
// Resize (and clear) the calling thread's TT: 2^bits buckets of 64 bytes.
void skat_set_tt_bits(int bits) { tls_solver().resize_tt(bits); }
int skat_get_options() { return g_opts; }

// Clear the calling thread's transposition table.
void skat_reset() {
  Solver& S = tls_solver();
  memset(S.tt.data(), 0, S.tt.size() * sizeof(Bucket));
  S.gen = 1;
}

// Legal cards (original encoding) for the player to act.
uint32_t skat_legal(const uint32_t* hands, const int8_t* trick, int8_t leader, int8_t decl, int8_t game) {
  State st = read_state(0, hands, trick, &leader, &decl, &game, nullptr);
  Solver& S = tls_solver();
  S.setup(st.hands, st.trick, st.ntrick, st.leader, st.decl, st.game, OBJ_POINTS);
  uint32_t L = S.legal(S.to_act), out = 0;
  while (L) {
    int c = lsb(L);
    L &= L - 1;
    out |= 1u << S.gt->to_orig[c];
  }
  return out;
}

// Batch solve.
//   hands  [n*3]  uint32 card masks (original encoding)
//   trick  [n*2]  cards already in the current trick in play order, -1 padded
//   leader, decl, game [n]   (game 0..3 suit, 4 grand, 5 null)
//   base   [n]    declarer points already secured (tricks taken + skat); ignored for null
//   mode   0 exact value, 1 threshold test (value >= thr[i]), 2 per-card values,
//          3 per-card threshold tests (1/0 per legal card)
//   obj    0 card points, 1 tricks (suit/grand only)
//   out    [n] (mode 0/1) or [n*32] (mode 2/3, indexed by original card, -1 = illegal)
//   nthreads > 1 starts that many std::threads, each with its own 4 MB TT (allocated per
//   call); 1 runs on the calling thread and keeps its TT between calls.
//   nodes  [n] optional per-state node counts
// Returns total nodes searched.
uint64_t skat_solve_batch(int n, const uint32_t* hands, const int8_t* trick, const int8_t* leader,
                          const int8_t* decl, const int8_t* game, const int16_t* base, int mode,
                          const int16_t* thr, int obj, int16_t* out, uint64_t* nodes, int nthreads) {
  int stride = mode >= 2 ? 32 : 1;
  auto work = [&](int lo, int step, uint64_t* total) {
    Solver& S = tls_solver();
    uint64_t t = 0;
    for (int i = lo; i < n; i += step) {
      State st = read_state(i, hands, trick, leader, decl, game, base);
      uint64_t k = run_one(S, st, mode, thr ? thr[i] : 0, obj, out + (size_t)i * stride);
      if (nodes) nodes[i] = k;
      t += k;
    }
    *total = t;
  };
  if (nthreads <= 1 || n < 2) {
    uint64_t t;
    work(0, 1, &t);
    return t;
  }
  nthreads = std::min(nthreads, n);
  std::vector<uint64_t> tot(nthreads, 0);
  std::vector<std::thread> th;
  for (int k = 0; k < nthreads; k++) th.emplace_back(work, k, nthreads, &tot[k]);
  uint64_t t = 0;
  for (int k = 0; k < nthreads; k++) {
    th[k].join();
    t += tot[k];
  }
  return t;
}

}  // extern "C"
