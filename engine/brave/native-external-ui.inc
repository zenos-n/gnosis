/* SPDX-License-Identifier: GPL-3.0-or-later
 * Chromium 155 ExternalProtocolDialog keeps the real handler, initiating
 * document, policy checks and origin/protocol grant. Only presentation changes.
 */
static void *(*original_external_show)(void *, void *);
static NativeVoid original_external_destroy;

static void destroy_external(void *view) {
  Request *r = remove_request(0, view);
  if (r) { notify_closed(r->id); g_free(r); }
  original_external_destroy(view);
}

static void decide_external(Request *r, gboolean accept, const char *choice) {
  /* The hidden checkbox is read by Chromium's original OnDialogAccepted.
   * Never create a remembered grant when the native policy omitted it. */
  if (r->checkbox) *((unsigned char *)r->checkbox + 0x628) =
    accept && choice && !strcmp(choice, "remember");
  native_call(accept ? UI_DIALOG_ACCEPT_RVA : UI_DIALOG_CANCEL_RVA, r->view);
  g_free(r);
}

static void *show_external(void *view, void *contents) {
  if (!gnosis_adapter_enabled("external-apps") || channel < 0 ||
      pointer_at(view, 0) != (void *)(image + UI_EXTERNAL_VTABLE_RVA))
    return original_external_show(view, contents);

  void *box = pointer_at(view, 0x6e0);
  g_autofree char *title = native_label(UI_EXTERNAL_TITLE_RVA, view);
  g_autofree char *accept = copy_u16((unsigned char *)view + 0x1f8);
  g_autofree char *cancel = copy_u16((unsigned char *)view + 0x210);
  g_autofree char *body = box ? copy_u16((unsigned char *)box + 0x348) : NULL;
  g_autofree char *checkbox = NULL;
  void *check = NULL;
  if (box && ((bool (*)(void *))(image + UI_EXTERNAL_CHECKBOX_VISIBLE_RVA))(box)) {
    check = pointer_at(box, 0x390);
    typedef struct { const gunichar2 *data; size_t length; } TextView;
    TextView text = ((TextView (*)(void *))(image + UI_EXTERNAL_CHECKBOX_TEXT_RVA))(check);
    if (text.length <= 8192 && (text.data || !text.length))
      checkbox = g_utf16_to_utf8(text.data, text.length, NULL, NULL, NULL);
    if (!checkbox) return original_external_show(view, contents);
  }
  if (!title || !*title || !accept || !*accept || !cancel || !*cancel || !body)
    return original_external_show(view, contents);

  /* Same owned widget/parent observer, without showing or registering a second
   * Chromium modal. The GTK tab cancels on navigation, hiding and destruction. */
  void *top = ((void *(*)(void *))(image + UI_EXTERNAL_EMBEDDER_RVA))(contents);
  top = ((void *(*)(void *))(image + UI_EXTERNAL_TOPLEVEL_RVA))(top);
  void *widget = ((void *(*)(void *, void *))(image + UI_EXTERNAL_CREATE_RVA))(view, top);
  Request *r = g_new0(Request, 1);
  r->id = ++sequence; r->created = g_get_monotonic_time();
  r->external = TRUE; r->view = view; r->widget = widget; r->checkbox = check;
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "external-app");
  json_object_set_int_member(message, "id", r->id);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "heading", "");
  JsonArray *notes = json_array_new(); json_array_add_string_element(notes, body);
  json_object_set_array_member(message, "permissions", notes);
  const char *target = authenticator_owners ? g_hash_table_lookup(authenticator_owners, contents) : NULL;
  json_object_set_string_member(message, "target", target ? target : "");
  if (checkbox) {
    json_object_set_string_member(message, "checkbox", checkbox);
    json_object_set_boolean_member(message, "checked", FALSE);
  }
  r->next = requests; requests = r;
  /* Do not destroy synchronously here: the constructor still installs its
   * PictureInPictureWatcher after this function returns. HUP cancels later. */
  if (!send_object(message)) shutdown(channel, SHUT_RDWR);
  return widget;
}

static gboolean native_external_install(void) {
#define EXTERNAL_MATCH(name) matches(UI_EXTERNAL_##name##_RVA, UI_EXTERNAL_##name##_BYTES)
  if (!EXTERNAL_MATCH(SHOW) || !EXTERNAL_MATCH(CREATE) || !EXTERNAL_MATCH(DESTROY) ||
      !EXTERNAL_MATCH(TITLE) || !EXTERNAL_MATCH(EMBEDDER) || !EXTERNAL_MATCH(TOPLEVEL) ||
      !EXTERNAL_MATCH(CHECKBOX_VISIBLE) || !EXTERNAL_MATCH(CHECKBOX_TEXT)) return FALSE;
#undef EXTERNAL_MATCH
  unsigned char *code = mmap(NULL, 128, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) return FALSE;
  memcpy(code, (void *)(image + UI_EXTERNAL_SHOW_RVA), 16);
  absolute_jump(code + 16, (void *)(image + UI_EXTERNAL_SHOW_RVA + 16));
  memcpy(code + 64, (void *)(image + UI_EXTERNAL_DESTROY_RVA), 10);
  memcpy(code + 74, "\x48\xb8", 2);
  uintptr_t table = image + UI_EXTERNAL_VTABLE_RVA;
  memcpy(code + 76, &table, 8);
  absolute_jump(code + 84, (void *)(image + UI_EXTERNAL_DESTROY_RVA + 17));
  if (mprotect(code, 128, PROT_READ | PROT_EXEC)) return FALSE;
  original_external_show = (void *(*)(void *, void *))code;
  original_external_destroy = (NativeVoid)(code + 64);
  unsigned char jump[14];
  absolute_jump(jump, show_external);
  if (!replace_code(UI_EXTERNAL_SHOW_RVA, jump, sizeof jump)) _exit(126);
  absolute_jump(jump, destroy_external);
  if (!replace_code(UI_EXTERNAL_DESTROY_RVA, jump, sizeof jump)) _exit(126);
  return TRUE;
}
