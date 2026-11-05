/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <math.h>

typedef struct {
  double x, y, width;
  int direction; /* -1 back (left edge), +1 forward (right edge) */
  bool eligible, owned;
} GnosisEdgeSwipe;

static inline void gnosis_edge_swipe_begin(GnosisEdgeSwipe *s, double x, double y,
                                         double width, bool enabled) {
  *s = (GnosisEdgeSwipe){.x = x, .y = y, .width = width};
  if (!enabled || !isfinite(width) || width <= 0 || x < 0 || x > width) return;
  s->direction = x <= width * .15 ? -1 : x >= width * .85 ? 1 : 0;
  s->eligible = s->direction != 0;
}

/* Returns true only when horizontal intent first takes the sequence. */
static inline bool gnosis_edge_swipe_move(GnosisEdgeSwipe *s, double x, double y) {
  if (!s->eligible || s->owned) return false;
  double inward = (s->x - x) * s->direction, dy = fabs(y - s->y);
  if (inward <= -16 || (dy >= 16 && dy > fabs(x - s->x))) s->eligible = false;
  else if (inward >= 16 && inward > dy * 1.5) return s->owned = true;
  return false;
}

static inline int gnosis_edge_swipe_finish(const GnosisEdgeSwipe *s, double x, double y) {
  double inward = (s->x - x) * s->direction;
  double distance = fmax(64, fmin(160, s->width * .15));
  return s->eligible && s->owned && inward >= distance && inward > fabs(y - s->y) * 1.5
    ? s->direction : 0;
}
