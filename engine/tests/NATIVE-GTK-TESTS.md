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
