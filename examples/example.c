/* Solve one Skat deal with the C API.  make build/example && ./build/example */
#include <stdio.h>
#include <string.h>

#include "skatdds.h"

/* "CJ SA H10 ..." -> card mask (suits D H S C = 0..3, ranks 7 8 9 10 J Q K A = 0..7) */
static uint32_t parse(const char* s) {
  static const char* suits = "DHSC";
  static const char* ranks = "789TJQKA";
  uint32_t m = 0;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    int suit = (int)(strchr(suits, s[0]) - suits);
    char r = s[1] == '1' ? 'T' : s[1];
    int rank = (int)(strchr(ranks, r) - ranks);
    m |= 1u << (suit * 8 + rank);
    s += s[1] == '1' ? 3 : 2;
  }
  return m;
}

int main(void) {
  /* Diamonds game, middlehand (player 1) declares, skat D9 H10 is the declarer's (10 points),
   * forehand (player 0) leads the first trick. */
  uint32_t hands[3] = {parse("D10 H7 HQ HA S9 S10 SK C8 CQ CA"),
                       parse("D7 D8 DJ DQ DK DA H8 HJ HK CJ"),
                       parse("H9 S7 S8 SJ SQ SA C7 C9 C10 CK")};
  int8_t trick[2] = {-1, -1}, leader = 0, decl = 1, game = 0;
  int16_t base = 10, out[32], thr = 61;
  uint64_t nodes;

  skat_solve_batch(1, hands, trick, &leader, &decl, &game, &base, SKAT_EXACT, NULL,
                   SKAT_OBJ_POINTS, out, &nodes, 1);
  printf("declarer gets %d card points with optimal play (%llu nodes)\n", out[0],
         (unsigned long long)nodes);

  skat_solve_batch(1, hands, trick, &leader, &decl, &game, &base, SKAT_THRESHOLD, &thr,
                   SKAT_OBJ_POINTS, out, &nodes, 1);
  printf("declarer can force >= 61: %s\n", out[0] ? "yes" : "no");

  skat_solve_batch(1, hands, trick, &leader, &decl, &game, &base, SKAT_MOVE_VALUES, NULL,
                   SKAT_OBJ_POINTS, out, NULL, 1);
  printf("forehand's leads -> declarer points:");
  for (int c = 0; c < 32; c++)
    if (out[c] >= 0) printf(" %c%s=%d", "DHSC"[c / 8], (const char*[]){"7", "8", "9", "10", "J", "Q", "K", "A"}[c % 8], out[c]);
  printf("\n");
  return 0;
}
