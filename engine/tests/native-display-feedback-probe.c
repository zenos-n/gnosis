/* SPDX-License-Identifier: GPL-3.0-or-later */
#define WLR_USE_UNSTABLE
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <math.h>
#include <gdk/gdk.h>
#include <dlfcn.h>
#include <unistd.h>
extern int gnosis_hdr_fixture_mode;
int gnosis_test_variable_feedback, gnosis_test_unknown_feedback, gnosis_test_outer_feedback;

gboolean gnosis_gdk_frame_timings_get_presentation(GdkFrameTimings *timings, guint32 *refresh, guint32 *flags) {
  gboolean (*real)(GdkFrameTimings *, guint32 *, guint32 *) = dlsym(RTLD_NEXT, "gnosis_gdk_frame_timings_get_presentation");
  gboolean valid = real(timings, refresh, flags);
  if (valid) gnosis_test_outer_feedback++;
  if (g_getenv("GNOSIS_HDR_REAL_FEEDBACK")) return valid;
  if (gnosis_hdr_fixture_mode == 1) {
    *refresh = 12345678;
    *flags = 1 | 8 | 16; /* Ensure nested rendering strips direct-scanout claims. */
    return TRUE;
  }
  if (gnosis_hdr_fixture_mode == 0) {
    *refresh = *flags = 0; /* Unknown/variable timing must not become fixed 60Hz. */
    return TRUE;
  }
  return valid;
}
void wlr_presentation_feedback_send_presented(struct wlr_presentation_feedback *feedback, const struct wlr_presentation_event *event) {
  if (gnosis_hdr_fixture_mode == 1 && event->refresh == 12345678 && event->flags == (1 | 16)) gnosis_test_variable_feedback++;
  if (gnosis_hdr_fixture_mode == 0 && event->refresh == 0 && event->flags == 0) gnosis_test_unknown_feedback++;
  void (*real)(struct wlr_presentation_feedback *, const struct wlr_presentation_event *) = dlsym(RTLD_NEXT, "wlr_presentation_feedback_send_presented");
  real(feedback, event);
}

int gnosis_test_target_metadata;
void wlr_color_manager_v1_set_surface_preferred_image_description(struct wlr_color_manager_v1 *manager, struct wlr_surface *surface, const struct wlr_image_description_v1_data *data) {
  if (gnosis_hdr_fixture_mode == 1 && data->has_mastering_display_primaries &&
      fabsf(data->mastering_display_primaries.green.y - .69f) < .00001f &&
      fabsf(data->mastering_display_primaries.blue.y - .06f) < .00001f &&
      fabsf(data->mastering_display_primaries.white.y - .329f) < .00001f &&
      data->max_cll == 1000 && data->max_fall == 400 && data->has_luminances &&
      data->luminances.reference == 203) gnosis_test_target_metadata++;
  void (*real)(struct wlr_color_manager_v1 *,struct wlr_surface *, const struct wlr_image_description_v1_data *)=dlsym(RTLD_NEXT,"wlr_color_manager_v1_set_surface_preferred_image_description");
  real(manager,surface,data);
}
