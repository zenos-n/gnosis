/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "edge-swipe.h"
#include <assert.h>

int main(void) {
  GnosisEdgeSwipe s;
  gnosis_edge_swipe_begin(&s, 150, 200, 1000, true);
  assert(s.eligible);
  assert(!gnosis_edge_swipe_move(&s, 160, 200));
  assert(gnosis_edge_swipe_move(&s, 180, 200));
  assert(!gnosis_edge_swipe_move(&s, 200, 200)); /* claim only once */
  assert(gnosis_edge_swipe_finish(&s, 310, 210) == -1);
  assert(!gnosis_edge_swipe_finish(&s, 170, 200)); /* short/returned swipe */
  assert(!gnosis_edge_swipe_finish(&s, 350, 400)); /* diagonal */
  gnosis_edge_swipe_begin(&s, 850, 200, 1000, true);
  assert(gnosis_edge_swipe_move(&s, 820, 200));
  assert(gnosis_edge_swipe_finish(&s, 690, 200) == 1);
  s.eligible = false; /* a second contact or disabled setting cancels navigation */
  assert(!gnosis_edge_swipe_finish(&s, 600, 200));
  for (int x = 151; x < 850; x++) {
    gnosis_edge_swipe_begin(&s, x, 200, 1000, true);
    assert(!gnosis_edge_swipe_move(&s, x + 200, 200));
    assert(!gnosis_edge_swipe_finish(&s, x + 300, 200));
  }
  gnosis_edge_swipe_begin(&s, 50, 200, 1000, true);
  assert(!gnosis_edge_swipe_move(&s, 55, 230)); /* vertical scroll wins */
  assert(!gnosis_edge_swipe_move(&s, 350, 230));
  assert(!gnosis_edge_swipe_finish(&s, 350, 230));
  gnosis_edge_swipe_begin(&s, 50, 200, 1000, true);
  assert(!gnosis_edge_swipe_move(&s, 30, 200)); /* outward motion wins */
  assert(!gnosis_edge_swipe_move(&s, 350, 200));
  gnosis_edge_swipe_begin(&s, 50, 200, 1000, false);
  assert(!gnosis_edge_swipe_move(&s, 350, 200));
  gnosis_edge_swipe_begin(&s, 0, 200, 0, true);
  assert(!s.eligible);
  gnosis_edge_swipe_begin(&s, 5, 200, 300, true);
  assert(gnosis_edge_swipe_move(&s, 30, 200));
  assert(!gnosis_edge_swipe_finish(&s, 65, 200));
  assert(gnosis_edge_swipe_finish(&s, 70, 200) == -1);
  return 0;
}
