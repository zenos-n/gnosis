# Gnosis

Gnosis uses GNOME Web's GTK/libadwaita shell with an embedded
Brave backend. Tabs, bookmarks, address entry and application controls are native
GTK. The supplied `gnosis.svg` is the application artwork.
Source and issues: [doromiert/gnosis](https://github.com/doromiert/gnosis).

Run `nix run .` or `nix run .#gnosis`. The executable and internal namespaces
use Gnosis. The application and schema ID is `com.neg-zero.Gnosis`; D-Bus
interfaces use `com.neg_zero.Gnosis` because interface names cannot contain hyphens.

The first normal launch exposes the existing profile under `gnosis`, moves cache
and configuration to `gnosis` directories, and copies settings to
`/com/neg_zero/gnosis/`, preserving any settings already stored there. Existing
profile storage stays at its original canonical path behind the new directory
redirect, preserving keyless extension IDs and their settings. Fresh installations
store their profiles directly in `gnosis`. Bundled extension public keys remain
unchanged, preserving KeePassXC pairing and extension storage. Migration refuses
to run while the previous browser is open or overwrite conflicting directories.

Enable **Vim Mode** in **Preferences → General → Browsing** for keyboard navigation.
It is off by default and takes effect immediately in open tabs.

- `h/j/k/l` select nearby page controls; counts such as `3j` move several controls.
  `d/u` scroll half a page, `gg`/`G` jump to the top/bottom of the current
  scroll area, and Tab cycles controls.
- `f` shows control hints; type a hint to select it. Enter activates selected
  buttons and links. `F` selects a link to open in a new tab when you press Enter.
- `/` opens native find; Enter confirms the match and returns to page navigation.
  The actual matched text stays selected, and Enter activates its enclosing
  button or link. `n`/`N` select the next/previous match.
- `H`/`L` go back/forward; `J`/`K` cycle tabs; `o` focuses the address bar;
  `t` opens a tab, `r` reloads and `x` closes through the normal close safeguards.
- Enter on a selected input enters **field normal mode**. `i/a/I/A` enter
  insert mode; Escape returns to field normal mode, and another Escape returns
  to page navigation. `h/j/k/l`, `w/b/e`, `0/$`, `x`, `dd/dw`, `u` and Ctrl+R
  provide field motions, deletion and undo/redo. `yy`/`p` yank and put within the field. `gi` enters the first field.

Press `:` in page or field normal mode for the native command bar. Commands
include `:open URL`, `:tabopen URL`, `:tabnext`, `:tabprev`, `:back`, `:forward`,
`:reload` / `:reload!`, `:write`, `:quit`, `:qall`, `:undo`, `:redo`,
`:set novim` and `:help`. Save and close use the existing native dialogs and
unsaved-change safeguards. Field text stays inside the renderer.

F3 and Shift+F3 use the native GTK find bar, including when Vim mode is off.

## AI browsing (MCP)

Open the **hamburger menu → AI Browsing** to control access for a connected MCP client:

- **Off** disables control and cancels pending AI operations. This is the default
  on every launch.
- **Shared Tabs** lets the AI work in your normal browsing windows. A separate
  native cursor labelled **AI** shows where it points and clicks, independently
  of your mouse pointer.
- **Separate Window** gives the AI its own native window and tabs in Gnosis's
  connected browser process. It shares your normal website profile, cookies and
  extensions. Its tabs stay in that window when you switch modes.

Private tabs are excluded. Switching modes revokes the old controller's pending
work; closing the AI window switches control off. One MCP client can control a
session at a time. Disconnecting that client releases its session.

Start Gnosis with `nix run .`, then add the stdio server to your MCP client. If
Gnosis is installed, its executable is `gnosis-mcp`. From this checkout:

```sh
nix run .#mcp
```

For clients using `mcpServers` configuration, for example:

```json
{
  "mcpServers": {
    "gnosis": {
      "command": "nix",
      "args": ["run", "/absolute/path/to/gnosis#mcp"]
    }
  }
}
```

Choose a mode in the browser to enable it. The AI cannot change that choice
through the browser tools. The server provides tab listing/open/select/close,
navigation/history, page text and viewport PNGs, element references, real mouse
input, Unicode text insertion, scrolling and keys. Element references cover the
top document and open shadow roots; use screenshot coordinates for frames,
closed shadow roots and canvas content. Coordinates are viewport CSS pixels.
Page text is website content, not trusted instructions.

Use `list_windows` to see each available window's title, focus and tabs, then
`activate_window` with its `window_id` to choose where subsequent calls operate.
Calls without `tab_id` stay in that window even when another window gets focus.
`open_tab` accepts `window_id`, and `list_tabs` can filter by it. Explicit tab IDs
still work across available windows and select their containing window. If a
selected window closes, choose another; calls do not silently move elsewhere.
Window selection resets when the client disconnects or the browsing mode changes.

The transport connects to Gnosis through the user's session D-Bus. It does not
open an HTTP or public debugging listener. In-flight calls have a 25-second
deadline, and normal tab-close/unsaved-change prompts still apply.
On Linux it discovers the current user's desktop bus even when an MCP host omits
the desktop environment variables; no per-client D-Bus address is needed.
An explicit `DBUS_SESSION_BUS_ADDRESS` or `XDG_RUNTIME_DIR` remains authoritative.

Protocol checks can run without a browser:

```sh
python3 engine/tests/mcp_protocol_test.py /path/to/gnosis-mcp
```

The native GTK/Brave integration check uses a disposable profile and local
fixtures. Compile `engine/tests/mcp-gtk-probe.c` as a preload library following
[the native probe instructions](engine/tests/NATIVE-GTK-TESTS.md), then run:

```sh
python3 engine/tools/check-mcp-ui.py --desktop --session-bus \
  --browser /path/to/gnosis --mcp /path/to/gnosis-mcp \
  --probe /path/to/mcp-gtk-probe.so
```

With `--session-bus`, the check targets only its disposable browser's unique
D-Bus connection, identified by PID. It does not control a running human profile.

The engine migration is **in progress**. Brave provides the active browser
backend, extensions and native engine services. WebKitGTK/JavaScriptCore and the
CEF compatibility layer remain linked for services still being migrated. See
[the current migration notes](BRAVE-MIGRATION.md) and
[the live checklist](GNOSIS-TODO.md) for verified coverage and remaining gaps.

The following sections retain notes from the earlier CEF implementation.

JavaScript evaluation runs in Chromium's main context or a native named isolated
world, using unique context identities across navigation. Isolated worlds share
the page DOM without sharing globals or gaining cross-origin access. Cancellation,
tab closure, and moving a view between windows resolve pending evaluations.
Chromium document/context destruction cancels them without awaiting a late
DevTools reply, and context removal checks the unique identity before numeric IDs.
Hiding a tab preserves its scripts. Declarative user scripts and private-world
messages still need porting. Internal top-level pages use native CEF messages
with JSON payloads: Reload, certificate-action callbacks, and web-app controls no
longer use WebKit/JSC message objects. The adapter rejects websites, subframes,
forged alternate-page URLs, and unregistered handlers, and keeps deliveries local
to each regular/private view. Certificate-error handling itself still needs its
native service migration. Returned values are native JSON descriptions from
Chromium, preserving nested values, Unicode, null/undefined, BigInt, and non-finite
numbers without constructing a JavaScriptCore context. Existing consumers use
JSON-GLib directly; remaining JavaScriptCore use belongs to legacy services and
the private page-message bridge.

Main-document navigation initializes native resources separately from overall
loading activity. Fragment changes, same-document Back, and subframe loads keep
the current resource, response metadata, and pending isolated-world scripts.
Same-document history refreshes without emitting false document-load events;
cached Back navigations retain the original response metadata.

Chromium load failures now reach Epiphany's actual error-page template, whose
Reload button navigates to the original destination. Internal SVG and CSS replies
infer their resource MIME types, so Chromium renders their icons and styles;
the templates use the standard system font family. Installed web-app icons load
through per-view native resource URLs. Only bounded decoded image data is served;
malformed or missing files use GNOME's fallback, and icon URLs expire on reload.

Permission requests now use native CEF objects and Epiphany's actual dialogs,
including microphone/camera requests and general permission prompts. The requesting
origin is kept separate from the top-level page. Navigation, subframe removal, and tab closure
withdraw pending requests; Ask Later dismisses them without saving a choice.
Notification, location, microphone, and camera choices now use Chromium's content
settings. Existing policy-file choices are imported once before navigation;
GNOME's website controls update the same store as JavaScript permission queries,
and revocation survives restart. Clipboard choices apply to each request. Private
choices remain in their own memory-only context and ignore the regular policy
file. Media checks use Chromium's diagnostic fake devices, not a claim of
physical webcam coverage. The old WebKit query hook is removed. Location/Bluetooth
services, native WebAuthn UI, and screen-capture source selection still need work.
Password, advertisement, and autoplay policies retain their existing backend;
the legacy page-script permission export still needs migration.

Clipboard completions return to the requesting V8 world, including concurrent
isolated worlds and replacement documents after reload. Text reads first pass
Chromium's original native permission and frame-policy checks, then read GTK's
actual clipboard; this fixes empty reads from the offscreen Wayland backend.
Native focus checks reject background operations. Main/private-world custom
Copy handlers retain their formats, and Cut deletes the selection once.
Rich `navigator.clipboard.read()` also reads GTK formats after Chromium grants
the request. It supports text, HTML/SVG, encoded PNG and native GTK textures,
and Figma's binary format. PNG bytes retain their encoding and metadata; decoding
stays in the renderer. Chromium's safe HTML API sanitizes markup in an inert
document; inline formatting survives, and explicit unsanitized HTML remains
available. Selecting formats avoids reading unwanted data, and a clipboard
change during a pending read cancels it. Generic web-custom-format interoperability
and Chromium's full HTML style-inlining behavior still need work.

Reader Mode extracts the current Chromium document before navigating, then
serves the prepared HTML through Epiphany's existing internal scheme. Live DOM
edits survive extraction. Explicit reader URLs use a separate native source view
in the same regular/private session; normal quit also works during a pending
source request. The actual Find entry receives selected page text from Chromium.

Session placeholders and alternate error pages retain their intended address;
their internal data URLs are excluded from the address bar and saved session.
Selecting a restored placeholder or reloading an alternate page loads its real URL.

Full uBlock Origin MV2 runs in Chromium's real extension runtime. Alloy tab APIs
have a native adapter; this is not uBlock Lite or a replacement URL blocker.
The compatibility check covers network filtering, cosmetic filtering in the main
page and an iframe, and a scriptlet. It is not a claim of complete extension API
coverage. Ordinary browsing keeps Chromium's sandbox enabled.
The unused WebKit JSON-filter manager and its startup downloads have been removed;
filtering is provided by the actual uBlock extension.

## Run

```sh
nix run .
```

The Nix package pins the CEF SDK and official full uBlock Origin. Gnosis uses
its own application ID and profile, separate from Epiphany and Select.

## Develop and check

```sh
nix develop
meson setup build-cef -Ddeveloper_mode=true -Dman-pages=disabled \
  -Dcef_root="$CEF_ROOT" -Dublock_dir="$UBLOCK_DIR"
meson compile -C build-cef -j$(nproc)
meson test -C build-cef --print-errorlogs 'Engine favicon cache' 'Engine Chromium origins'

cmake -S engine -B engine/build -DCEF_ROOT="$CEF_ROOT" -DUBLOCK_DIR="$UBLOCK_DIR"
cmake --build engine/build -j$(nproc)
python3 engine/tools/check-native.py engine/build/engine-view-test
python3 engine/tools/check-native.py engine/build/script-test
python3 engine/tools/check-native.py --desktop engine/build/capabilities-test
python3 engine/tools/check-native.py --desktop engine/build/capabilities-test --gpu-only
python3 engine/tools/check-extensions.py engine/build/extension-probe
python3 engine/tools/check-native.py --portals engine/build/portal-test --files-only
python3 engine/tools/check-native.py "$(command -v python3)" engine/tools/check-cache-ui.py \
  build-cef/src/gnosis --schemas build-cef/data
python3 engine/tools/check-native.py "$(command -v python3)" engine/tools/check-cookies-ui.py \
  build-cef/src/gnosis --schemas build-cef/data
```

The native checks use a local HTTP fixture, Xvfb, and disposable profiles.
The desktop capability check waits for its test window to be active before
exercising focus-protected clipboard operations. The GTK Find/Reader workflow
can be checked in regular and private mode with:

```sh
python3 engine/tools/check-native.py --timeout 90 "$(command -v python3)" \
  engine/tools/check-script-ui.py build-cef/src/gnosis --schemas build-cef/data
python3 engine/tools/check-native.py --timeout 90 "$(command -v python3)" \
  engine/tools/check-script-ui.py build-cef/src/gnosis --schemas build-cef/data --incognito
```
 The
isolated CEF diagnostic processes disable their sandbox; the application does
not. The inherited upstream test suite still needs its engine-dependent fixtures
ported to CEF; a successful compile does not mean that whole suite passes.
The portal check saves through Chromium's real writable file handle and verifies
the exact binary bytes and Unicode filename on disk. It also exercises uploads,
cancellation, hiding the page, and closing the window. Folder uploads remain excluded because of a known Chromium dialog
crash in the current offscreen embedding.

The original project's documentation is preserved in
[README.epiphany.md](README.epiphany.md). Epiphany's original license and
copyright notices remain in place. The package includes CEF's license and
Chromium third-party credits.
