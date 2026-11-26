/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "content-color-layout.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  unsigned char before[500], after[500];
  for(unsigned i=0;i<sizeof before;i++)before[i]=(unsigned char)(i*7+11);
  memcpy(after,before,sizeof after);gnosis_content_color_spaces(after);
  assert(!memcmp(before,after,4));
  for(unsigned i=0;i<4;i++) {
    const unsigned char *c=after+4+i*68;
    assert(c[0]==(i<2?1:10)&&c[1]==14&&c[2]==1&&c[3]==2);
    for(unsigned j=4;j<68;j++)assert(c[j]==0);
  }
  /* HDR slots, formats, physical display primaries and white/headroom survive. */
  assert(!memcmp(before+276,after+276,sizeof before-276));
  puts("CONTENT_COLOR_PASS SDR slots corrected; HDR and metadata unchanged");
}
