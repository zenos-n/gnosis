# Content color and document lifetime

The exact-binary bridge targets Brave 1.97.56 / Chromium 155.0.8059.40,
GTK 4.22.4, wlroots 0.19.3 and Mutter 50.4. Review the pinned binary signatures
and toolkit patches when upgrading.

Display HDR capability is not evidence that a document contains HDR. Chromium's
SDR sRGB and Display-P3 output slots retain their own color states; its HDR slots
retain PQ output and the display's negotiated luminance metadata. No page DOM
heuristic decides whether a page is HDR. An HDR document is currently one mixed
HDR surface, including its SDR parts; separate video-only HDR planes are not
implemented.

Rounded window clipping can prevent a page's edge strips from being offloaded
to native Wayland subsurfaces. Those strips are rendered by GTK instead. The
outer GTK surface follows the visible textures: HDR content enables its preferred
HDR state; ordinary SDR content resets it to sRGB. Keeping the outer surface
permanently SDR clips/tone maps just those strips, producing a darker bottom
band. Monitor capability alone must never enable the outer HDR path.

Installed web-app launchers use the stable installed Gnosis launcher found in
PATH, falling back to the build executable only when none is installed. Profile
migration 42 refreshes existing owned launchers; declarative launchers are left
to their configuration owner. Existing running processes retain their old code
until closed. Do not diagnose a web app's colors without checking its executable.

A closed Gnosis tab always destroys its Chromium target. Reusing a hidden page
and navigating it to about:blank is insufficient: navigation can be blocked by
beforeunload, and the document can keep playing audio. Chromium's keepalive switch
keeps the managed engine alive without retaining any document. The private
launcher execs the browser ELF directly; its shim requests SIGTERM if the owning
GTK process dies. Normal shutdown still requests Browser.close. Neither changes
native beforeunload preflight or its Cancel/Leave decisions.

Regression coverage:

- `native-hdr-gtk-test.py`: pure SDR on an HDR-capable display, Display-P3 SDR,
  HDR images/video, root HDR mode, fractional clipping, repeated fullscreen and
  real presentation cadence. The fixture simulates display metadata on an
  isolated Mutter display; it is not a physical monitor calibration.
- `native-tab-lifetime-gtk-test.py`: actual GTK tab close destroys the target;
  zero remaining document targets; fresh tab opens through the managed engine.
- `native-close-test.py`: real beforeunload handlers, Cancel/Leave, canceled
  asynchronous replies and repeated checks.
- `native-address-direction-gtk-test.py`: URL editing resets GTK's persistent
  Change Direction action to LTR. Multilingual searches can still use GTK's
  direction action during editing.

All fixtures use private HOME/XDG directories. Do not stop a human browser or
change physical display settings to run them.
