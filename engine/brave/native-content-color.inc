/* SPDX-License-Identifier: GPL-3.0-or-later
 * Keep output capability separate from content usage. See patches/README.md
 * for the exact Chromium ABI and asynchronous description commit requirement.
 */
#include "content-color-layout.h"
static void *(*original_content_color_spaces)(void *, uintptr_t, unsigned);
static void (*original_color_request)(void *, const void *, const void *);
static void (*original_color_ready)(void *, void *);
static _Thread_local unsigned color_request_depth;
static void *content_color_spaces(void *description, uintptr_t format, unsigned present) {
  void *reference = original_content_color_spaces(description, format, present);
  if (reference) gnosis_content_color_spaces(reference);
  return reference;
}
static void content_color_request(void *self, const void *color, const void *metadata) {
  ++color_request_depth;
  original_color_request(self, color, metadata);
  --color_request_depth;
}
static void content_color_ready(void *self, void *description) {
  gboolean valid = description != NULL;
  original_color_ready(self, description);
  /* Cached descriptions are set synchronously inside ApplyPendingState: that
   * caller already commits the buffer and color state together. A newly created
   * description arrives later and otherwise waits for another content frame. */
  if (valid && !color_request_depth) {
    void *surface = *(void **)((unsigned char *)self + 0x18);
    void *proxy = *(void **)((unsigned char *)surface + 0x348);
    unsigned version = ((unsigned (*)(void *))(image + UI_CONTENT_COLOR_PROXY_VERSION_RVA))(proxy);
    /* Only commit pending protocol state. WaylandSurface::Commit also runs
     * explicit buffer-release handling owned by the normal frame submission. */
    ((void *(*)(void *, unsigned, const void *, unsigned, unsigned, ...))
      (image + UI_CONTENT_COLOR_PROXY_MARSHAL_RVA))(proxy, 6, NULL, version, 0);
    ((void (*)(void *))(image + UI_CONTENT_COLOR_FLUSH_RVA))
      (*(void **)((unsigned char *)self + 0x20));
  }
}
static void *content_color_hook(uintptr_t offset, const char *prefix, size_t length, void *replacement) {
  if (!matches(offset, prefix)) return NULL;
  unsigned char *code = mmap(NULL, 64, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) return NULL;
  /* Pinned complete prologue instructions, no PC-relative operands. */
  memcpy(code, (void *)(image + offset), length);
  absolute_jump(code + length, (void *)(image + offset + length));
  if (mprotect(code, 64, PROT_READ | PROT_EXEC)) { munmap(code, 64); return NULL; }
  unsigned char jump[14]; absolute_jump(jump, replacement);
  if (!replace_code(offset, jump, sizeof jump)) { munmap(code, 64); return NULL; }
  return code;
}
static gboolean install_content_color(void) {
  if (!matches(UI_CONTENT_COLOR_PROXY_VERSION_RVA, UI_CONTENT_COLOR_PROXY_VERSION_BYTES) ||
      !matches(UI_CONTENT_COLOR_PROXY_MARSHAL_RVA, UI_CONTENT_COLOR_PROXY_MARSHAL_BYTES) ||
      !matches(UI_CONTENT_COLOR_FLUSH_RVA, UI_CONTENT_COLOR_FLUSH_BYTES)) return FALSE;
  original_content_color_spaces = content_color_hook(UI_CONTENT_COLOR_SPACES_RVA,
    UI_CONTENT_COLOR_SPACES_BYTES, 18, content_color_spaces);
  original_color_request = content_color_hook(UI_CONTENT_COLOR_REQUEST_RVA,
    UI_CONTENT_COLOR_REQUEST_BYTES, 17, content_color_request);
  original_color_ready = content_color_hook(UI_CONTENT_COLOR_READY_RVA,
    UI_CONTENT_COLOR_READY_BYTES, 16, content_color_ready);
  return original_content_color_spaces && original_color_request && original_color_ready;
}
