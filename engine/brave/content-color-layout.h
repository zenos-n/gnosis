/* SPDX-License-Identifier: GPL-3.0-or-later
 * Brave1.97.56/Chromium155 x86-64 layout, checked by the native shim's binary pin.
 * gfx::DisplayColorSpacesRef: 4-byte refcount, six 68-byte ColorSpaces, then
 * formats/display primaries/luminance. Enum values follow ui/gfx/color_space.h.
 */
#pragma once
#include <stdint.h>
#include <string.h>
static inline void gnosis_content_color_spaces(void *reference) {
  unsigned char *spaces = (unsigned char *)reference + 4;
  for (unsigned i = 0; i < 4; ++i) {
    memset(spaces + i * 68, 0, 68);
    spaces[i * 68] = i < 2 ? 1 : 10; /* BT709/sRGB, DisplayP3-D65 */
    spaces[i * 68 + 1] = 14; /* SRGB transfer */
    spaces[i * 68 + 2] = 1;  /* RGB matrix */
    spaces[i * 68 + 3] = 2;  /* FULL range */
  }
}
