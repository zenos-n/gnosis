# Packaged Brave and GTK regressions

The Python native probes use inherited pipes and local fixtures. Keep Chromium's
sandbox enabled. Pass the pinned patched Brave launcher from the Gnosis package:

```sh
python3 engine/tests/native-find-test.py --browser "$BRAVE"
python3 engine/tests/native-close-test.py --browser "$BRAVE"
python3 engine/tools/check-context-menu-media.py --browser "$BRAVE"
python3 engine/tools/check-full-ublock-native.py --browser "$BRAVE" --ublock "$UBLOCK"
```

`brave-private-profile-test` and `brave-bundled-extension-test` are Meson tests
when `unit_tests` is enabled. They exercise owned-directory cleanup without
following symlinks, stable extension identity, updates, rollback, recovery,
permissions, concurrent updates and downgrade refusal. They need no browser.

The `*-gtk-probe.cpp` sources preload the actual packaged Gnosis executable;
they operate its actual GTK widgets and native browser backend. Use a disposable
`--profile` and `GSETTINGS_BACKEND=memory`; never preload into a human profile.
They are deliberately separate from the old CEF view tests.

`native-touch-gtk-probe.cpp` needs a real GNOME/Mutter desktop. Compile it like
the other C++ GTK probes, then run `native-touch-gtk-test.py --gnosis "$GNOSIS"
--probe /tmp/gnosis-touch-probe.so`. The runner serves its local fixture, owns a
temporary profile and process group, and bounds cleanup. The probe temporarily
fullscreens only its own window and uses its own Mutter RemoteDesktop/ScreenCast
session for actual touchscreen events; DevTools only observes fixture state.
It uses DisplayPort by default (`--monitor DP`), or the explicitly selected
connector, e.g. `--monitor HDMI`, without changing display configuration. These
physical-device checks take focus; do not run them while the user is playing. It checks
trusted taps, six contacts, scrolling and post-release momentum, pinch in/out, the 15% edge zones,
back/forward, the live setting, vertical/multi-finger rejection, and unmap cleanup.
It stops its input session before quitting. `--preview` checks actual destination
pixels during back/forward drags, returning a cancelled swipe, and completed
navigation; captures are `/tmp/gnosis-swipe-*.png`. `--trackpad` uses real smooth
pointer-axis events and a finger-source stop to check pixel scaling and momentum.
A physical touchscreen/trackpad and touchpad pinch are not covered by these
virtual-device checks.

`keepassxc-pairing-gtk-probe.cpp` is driven by
`python3 engine/tools/check-keepassxc-pairing.py --browser "$GNOSIS" --probe "$PROBE"`.
Compile it with the same preload flags and `-Iengine/brave`. The driver owns its
profiles, display and fixtures. It blocks the real native host and uses public
fixture keys to check association transfer through the actual extension workers,
live re-pairing/removal, app storage isolation, excluded contexts and launcher
upgrades. It does not establish a live database password/TOTP round trip.

`native-select-gtk-probe.cpp` uses `native-select.html` and its child fixture.
It asserts that the native Wayland popup actually paints (the previous compositor
left it unconfigured), then checks trusted keyboard selection, themed dropdowns,
nested frames, Escape cancellation, listbox preservation, and regular/layered
author overrides under a nonce-only stylesheet CSP. It saves the native,
themed, and viewport-edge menus to `/tmp/gnosis-select-{native,themed,edge}.png`.

From the repository root, serve `engine/tests/fixtures` at 127.0.0.1:18473. The
find fixture includes a same-process frame and an independently hosted localhost
frame. Compile each probe inside the matching Nix development environment:

```sh
cc -std=c++20 -shared -fPIC -fplt -fno-rtti -fno-exceptions -O1 \
  -Iengine/brave engine/tests/native-find-gtk-probe.cpp \
  $(pkg-config --cflags gtk4 libadwaita-1 json-glib-1.0) \
  -ldl -Wl,-z,lazy -o /tmp/gnosis-find-gtk-probe.so
```

Do not link GTK into the preload library; the packaged process supplies it.
If the Nix compiler wrapper enables immediate binding, clear its inherited
`NIX_LDFLAGS` for this test-only compilation. Run with a real desktop display,
or an isolated Xvfb display with `GDK_BACKEND=x11 GSK_RENDERER=cairo`:

```sh
GSETTINGS_BACKEND=memory LD_PRELOAD=/tmp/gnosis-find-gtk-probe.so \
  dbus-run-session -- "$GNOSIS" --profile=/tmp/gnosis-find-test \
  http://127.0.0.1:18473/native-find.html
```

The find probe checks counts/ordinals, next/previous, case sensitivity, hidden
unsupported options, empty queries, reopening and Unicode. It saves only the
GTK toolbar to `/tmp/gnosis-find-toolbar.png`.

`web-theme-gtk-probe.cpp` uses the same compile flags and `web-theme.html`.
Run it with the fixture server above, `about:blank`, a disposable profile and
`GSETTINGS_BACKEND=memory`. It forces GTK dark/light/dark transitions inside the
test process and checks initial inline-script values, CSS media queries and
`matchMedia` change events in regular/private tabs and cross-process iframes.
A new regular tab must inherit light after switching. `GNOSIS_THEME_FIXTURE`
can override the default `http://127.0.0.1:18473/web-theme.html` URL.

`extension-popup-gtk-probe.cpp` opens the real Extensions menu and uBlock action
twice, checking its native browser session, owning hostname, background readiness,
visible sizing and Back/reopen behavior. Start the disposable browser on an HTTP
fixture at `127.0.0.1`. Optionally set `GNOSIS_POPUP_TEST_EXTENSION` to a disposable
copy of Consent-O-Matic's extension files (exclude `_metadata`) under the test
profile's `brave/Default/Extensions/<id>/<version>_0` to run the same open/reopen
checks and verify its owning site. No extension profile/storage files are needed.
Popup captures are saved as `/tmp/gnosis-extension-popup-{0,1,2,3}.png`.
`node engine/tests/popup-owner-test.js` checks native query forwarding, unchanged
filters/results/errors, callback/promise support and excluded/private contexts.

`native-cursor-gtk-probe.cpp` needs an HTTP fixture (any local document). It
checks text/link/autoscroll cursors and actual middle-click scrolling, then
opens real GTK tabs through middle-click, noopener and target-blank links.
Repeated links to the same URL must each open exactly one tab, while the source
page stays in place. Run it with both a regular disposable profile and
`--incognito-mode` to check routing within the private browser process.
On an isolated X11 display, set `GNOSIS_XDOTOOL` to exercise actual GTK input
instead of injecting mouse events into Brave. To check upgrades, first run the
probe against the previous package with `GNOSIS_SEED_PROFILE=1`, then run the
current package with the same disposable profile. Cached workers must acquire
the new navigation listener without registering it twice.

`keyboard-navigation-gtk-probe.cpp` uses actual XTest events through xdotool;
set `GNOSIS_XDOTOOL` to its executable and run on an isolated X11 display with
`keyboard-navigation.html` and `keyboard-child.html` served on localhost:18473.
It dismisses only its disposable window's startup dialog so it cannot intercept
the test's physical keys. It checks F3/Shift+F3 against native match ordinals and last-query retention,
enables Vim through the real Preferences switch, and checks spatial selection of
buttons/inputs, Enter entering field normal mode, i entering insert mode, the two
Escape steps, motions/deletion, trusted input events, Unicode graphemes, undo/redo,
and Ctrl+R/Ctrl+W remaining field operations. The native colon entry tests help,
unknown commands, undo/redo, tabopen and set novim. Open and closed shadow-DOM
inputs and forced cross-process frames are covered; no renderer field values
are sent through the native binding. Default page-world code cannot access the
adapter's globals. Only this test adds the site-isolation flag.


The close probe checks the real native beforeunload handler, last-tab Keep Open,
two-tab transactional cancellation, Leave on both tabs and absence of duplicate
form warnings. It waits for distinct dialogs so an exit animation cannot cause
an extra simulated click. The close-failure probe deliberately drops only
native close-result packets, testing the real timeout and explicit Keep Open /
Close Anyway recovery. It does not disable the sandbox or replace page handlers.

The quit probe repeats cancellation/approval across two real shell windows using
application-wide Quit. Its weak set tracks all answered dialogs independently,
including the case where two windows have prompts open simultaneously.

The frame-pacing probe runs on the real desktop with `GNOSIS_FRAME_MONITOR=60`
or `120`, using `/perf-native.html`. It waits fifteen seconds after loading to
exclude startup/fullscreen-placement transitions, then prints six seconds of
rAF timing, GTK ticks, compositor invalidations and buffer imports. It measures
the shipping command line without adding browser feature flags. To obtain the
explicit uncapped diagnostic baseline, use `GNOSIS_UNCAP_FRAMES=1` separately.
`mixed-frame-pacing-gtk-probe.cpp` opens two actual shell windows sharing the
browser process on separate 60/120 Hz monitors. It measures each window's rAF
and GTK ticks independently. This does not diagnose the separate severe GPU stall.

`native-resize-gtk-probe.cpp` verifies ordinary resize/maximize/restore and can
suppress backend allocation callbacks to test active-frame reconciliation.
`oopif-gtk-probe.cpp` uses forced site isolation only in the test. Parent, child
and nested child fixtures verify real GTK image copying, frame-local audio,
custom CSS, private-world isolation, stale navigation rejection and close
approval invalidation without replacing the parent title or URI.

Compile `webauthn-accounts-gtk-probe.cpp` with `c++`, rather than `cc`, because its
preload has global C++ objects. Run incognito to keep KeePassXC's content-script
interception out of this browser-owned WebAuthn test, using localhost and
`webauthn-accounts.html`. The public synthetic fixture keys are only for testing.
It uses a real virtual CTAP authenticator, checks both account rows and Cancel
fit, chooses Bob and verifies the real ECDSA assertion, UV and counter. It then
tests account cancellation and switching to the genuine native phone QR flow.
Automatic virtual USB touches are paused before switching devices, so they
cannot immediately answer the restarted request again. No pairing QR is logged.
This is a UI/request-lifetime regression: Chromium's virtual discovery factory
does not listen for physical phones. Do not use that QR for a phone round trip.
Manual phone checks must run without `WebAuthn.enable` or a virtual authenticator
and use an actual passkey registered for the test RP.

The private-profile probe adds `--incognito-mode`; it verifies that the owned
Brave profile disappears after the browser and background filesystem workers
exit. The legacy-session C probe also accepts incognito with
`GNOSIS_EXPECT_LEGACY_EPHEMERAL=1` and verifies lazy WebKit session creation.

The adblock-settings probe waits for the official extension's first-install
reload, configures only local fixture filters, and changes real GSettings.
It checks native enablement plus actual cosmetic/scriptlet filtering off/on.
A timeout or `_FAIL` output is a failed check. Isolated portal startup may take
25 seconds; allow at least two minutes for a whole GTK process.


`native-external-gtk-probe.cpp` and `native-external-gtk-test.py` exercise the
actual Chromium external-protocol dialog through the GTK shell on an isolated
Xvfb display and private session bus. Compile with the standard preload flags,
then run `native-external-gtk-test.py --gnosis "$GNOSIS" --probe "$PROBE"
--xvfb /path/to/Xvfb`. The runner registers a log-only custom URI handler inside
its temporary XDG directories. It checks Cancel, Open once, an initially unchecked
remember checkbox, per-origin grants, private-context isolation, exact URI delivery and navigation cleanup.
No desktop input, physical monitor or human protocol-handler registration is
used. This does not launch Prism or exercise a real OAuth account.


`web-app-links-gtk-probe.cpp` and `web-app-links-gtk-test.py` exercise native
link-association controls and the real default-browser/app activation path on
Xvfb with a private bus and disposable XDG directories. Compile with the usual
preload flags plus `-Iengine -Isrc/preferences -Ilib` and `gio-unix-2.0` cflags.
Run `web-app-links-gtk-test.py --gnosis "$GNOSIS" --probe "$PROBE" --xvfb "$XVFB"`.
Optionally pass `--legacy-gnosis /path/to/older/gnosis` to check incoming URLs
forwarded by an older installed desktop launcher to an updated running browser.
The fixture checks a closed app, an already-running app receiving a deep link,
ordinary launcher activation, exact encoded URLs, browser fallback, private
isolation, disabled associations and ambiguous claims. No human XDG defaults,
browser profile, physical display or desktop focus are changed.
