# Native Brave integration patches

Keep this series small: add an opt-in hook at a stable browser component,
leave GTK UI and policy in Gnosis, and preserve ordinary Brave behavior when
the switch is absent. Do not copy Views layouts into the GTK shell. The user
now prefers a version/hash-gated binary patch or native shim to a full Chromium
build; this source series remains a reference, not a prerequisite for that route.

`series.json` records the Brave/Chromium release pair, pristine source hashes,
and ordered patch hashes. The first patch changes one component file to honor
`--gnosis-native-ui` when deciding whether to show zoom feedback. Native zoom,
layout, events, and per-tab settings remain intact. The mode-transition path
also respects the visibility flag; suppressing extension zoom alone missed it.

This series is a source reference. The default flake now uses a private prebuilt
Brave overlay from `../native-ui.nix` instead: `binary-native-ui.json` pins the
exact Nix ELF SHA-256, original hook bytes and patched output SHA-256.
`patch-brave-binary.py` changes the first byte of `ZoomBubbleCoordinator::Show`
from `push rbp` to `ret`; all page zoom is performed upstream. Only the private
overlay suppresses zoom feedback (regardless of the source-patch switch).
System Brave and the Nix input remain unchanged. Resources are linked, and only
the 312 MiB executable plus two launcher scripts are copied. There is no
Chromium source compilation. Unsupported binaries fail before output creation.

CSS-styled scrollbars normally hard-code `CustomScrollbar::IsOverlayScrollbar`
to false in this Chromium release. The overlay patch changes only that class's
vtable slot's ELF RELATIVE relocation addend (destination `0x12c4dae8`, addend
at file offset `0x674950`) from the shared false-returning function to the
existing true-returning function. It never changes the shared function's code.
The constructor installs this vtable at `0xf5c0196`; the pinned source is
`third_party/blink/renderer/core/layout/custom_scrollbar.h` at 155.0.8059.40.
Gnosis enables `OverlayScrollbar:mode/overlay`; the previous custom-scrollbar
classification patch is retained for website-authored custom scrollbars.

`binary-native-scrollbars.json` applies **after** `binary-native-ui.json` and pins
the native renderer theme. It reuses Chromium's Fluent rounded painter and
compositor animations: 24px overlay hit area, 8px expanded thumb/track, 3px idle
thumb, 42px minimum thumb, and no arrows. The track is inset 8px across its axis
and 9px along it. Thumb colors are white at 20/40/60% alpha; the track is white
at 10%. Fade and thickness transitions use 200ms. Author-provided colors and
forced/high-contrast track rendering retain upstream behavior; opaque black or
white accessibility thumb palettes also retain their upstream colors.

The native `scrollbar-width: thin` proportion lookups point to a private `[1,1]`
table, so normal and thin controls have identical geometry. `none` remains
hidden. Only the identified scrollbar lookup sites change; shared float constants
and shared true/false implementations remain untouched.

LLVM constant-folds some virtual calls into metadata before each vtable. Updating
only the inset getter and nine-patch vtable slot does **not** update those callers.
The manifest therefore also changes Fluent's inset constant at `vptr-20` and
clears only bit 2 of its nine-patch flag at `vptr-17` (retaining the solid-color
thumb flag). Nine-patch track caching assumes nonempty arrow buttons; the native
arrowless theme instead paints a cached full-size rounded track. All addresses,
original bytes and both complete ELF hashes are checked before application.
`native-scrollbars-x86_64.S` is the reviewable source for the small helper block
placed in verified RX alignment padding; no sandbox or CFI checks are disabled.

There is no injected scrollbar stylesheet, mutation listener, pointer listener or
JavaScript animation loop. The remaining GTK selection/overlay colors and user
styles use temporary DOM/CSS inspector sessions: queued writes are serialized per
session and both agents are disabled after the batch. Inspector-owned styles
survive disabling and can be updated after reenabling, including under strict
CSP. Out-of-process frames follow the same lifecycle.

Touch history swipes use GTK page-under-page previews following WebKitGTK's
`ViewGestureControllerGtk` interaction model. Native viewport captures are
immutable GPU textures (long edge capped at1600px), held in a32MiB cache per
browser profile, plus the active transition. Captures happen at navigation
boundaries, not on frames or scrolling; same-document replacement does not
continuously capture. Closing a tab and disabling gestures release its images.
Private snapshots stay in their own browser profile's memory and never go to
disk. The old Chromium overscroll arrow is disabled in the embedded browser.
Source reference: https://github.com/WebKit/WebKit/blob/main/Source/WebKit/UIProcess/gtk/ViewGestureControllerGtk.cpp

Smooth GTK scroll events retain their pixel units and finger source, including
axis-stop events that let Chromium generate a native fling. The pinned Chromium
155 Ozone backend otherwise scales raw Wayland deltas by120/10. Its upstream
`WaylandUnscaledTouchpadScrolling:scroll_scaling_factor/1` feature keeps those
already-scaled GTK deltas at1:1; discrete wheel/value120 behavior stays upstream.

HTML dropdown defaults use `native-integration/select.css` through Chromium's
USER-origin stylesheet API, once per committed HTTP/HTTPS/file frame. Author
styles override these defaults, including named CSS layers, and strict CSP
does not block the browser-owned stylesheet. There are no page scripts or DOM
observers. Page Styling disables these defaults on the next browser start.
The compositor also configures and unconstrains real xdg popups, so a website
that opts back into ordinary native select rendering still has a working menu.

Settings → Debugging → Native Scrollbar Theme selects the original native-UI
renderer on the next start when disabled. Its native GTK prompt hooks remain
active. The shim accepts only the two exact manifest hashes. The former runtime
scrollbar implementation is preserved on
`archive/runtime-scrollbars-before-native-2026-10-27`.

Verify the renderer with `engine/tools/check-native-scrollbars.py --browser
/path/to/bin/brave` (ImageMagick on PATH), then run the real uBlock check. The
scrollbar check asserts the actual painted 8px root/normal/thin tracks, real thumb
dragging on both axes, authored colors, visible forced-color thumbs, and persistent
strict-CSP colors with the DOM/CSS agents disabled. Captures remain under
`/tmp/gnosis-native-scrollbars-check` for visual inspection.

Native infobar banners are suppressed through the exact
`InfoBarContainerView::CalculatePreferredSize` implementation at `0x04993bb0`.
It returns an empty size, so the upstream layout regards the container as empty
and reserves no webpage space. This covers all infobar messages, including flag
warnings, while leaving their underlying flags, delegates and native ownership
intact. The old per-flag warning-table workaround is removed. The ELF input,
original sizing-method bytes and patched output are pinned by SHA-256. A native
GTK viewport comparison verifies 712/712 pixels of webpage height, versus
656/712 with the previous banner. Sandbox settings are unchanged.

The native BrowserHandler::DownloadWillBegin body hook (`0x98d134a`, 20-byte
relocatable prologue) excludes only DownloadSource::EXTENSION_INSTALLER (5) from
DevTools download events and progress observers. The installer's own DownloadItem
observers still perform verification/unpacking. Previously GTK moved the CRX
on completion before SandboxedUnpacker could copy it, causing
FAILED_TO_COPY_EXTENSION_FILE_TO_TEMP_DIRECTORY. A real Google Translate install
now reports enabled through Web Store status and Remove from Brave; ordinary TXT
and manual CRX downloads still emit events and preserve their bytes. This uses
DownloadItem's native source getter, never file-extension or URL heuristics.

Check the pinned release without fetching a full browser checkout:

```sh
nix run .#check-brave-patches
```

Or check an initialized Brave checkout after Brave applies its own patches:

```sh
python3 engine/tools/check-brave-patches.py /path/to/brave-browser/src
```

Probe a release upgrade before changing the pin:

```sh
nix run .#check-brave-patches -- --fetch --brave-version 1.100.31 --upgrade \
  --report /tmp/gnosis-brave-upgrade.json \
  --candidate-pin /tmp/gnosis-brave-series-next.json
```

The checker resolves Chromium from that Brave release's own `package.json`.
It fetches only the touched sources and refuses to simulate a release where
Brave adds an override to those files: use an initialized checkout for that
case. Neither fetch mode nor local mode changes the supplied checkout or pin.
Candidate mode permits source/version drift, but still enforces patch hashes,
exact hunk context (zero fuzz), forward application, and byte-identical reversal.
Line offsets are allowed so unrelated upstream insertions do not need a rebase.
Failures identify the changed patch, source, release pair, or rejected hunk.

After source checks, review the proposed pin and build the patched Brave release.
Apply from its Chromium `src` root, after Brave initialization:

```sh
patch --batch --fuzz=0 --forward -p1 < /path/to/gnosis/engine/brave/patches/0001-native-zoom-feedback.patch
```

Required runtime checks before accepting an upgrade: GTK zoom buttons,
Ctrl+plus/minus/reset, Ctrl+wheel, normal/private profiles, reload/navigation,
actual page reflow, and absence of the native popup. Also launch Brave without
the Gnosis switch to verify its normal zoom feedback remains available.
Source checks never establish compilation or runtime compatibility.

The Nix package accepts a `brave` package argument for the production build.
For a local patched executable, the launcher also honors explicit overrides:

```sh
GNOSIS_BRAVE_EXECUTABLE=/path/to/patched/brave \
GNOSIS_BRAVE_VERSION=1.97.56 nix run .
```

This does not build or patch Brave for you. The executable must already contain
the intended native UI changes and have its runtime dependencies available.

`nix flake check` includes the pinned source check and detects when nixpkgs'
Brave version drifts from this source pin. The binary overlay independently
checks its version and exact ELF identity; changing source pins alone does not
authorize applying old binary offsets to a new executable.


## Native popup source map

Inspected against Chromium **155.0.8059.40** and Brave **1.97.56**. Paths below
are relative to Chromium `src`, unless prefixed with `brave/`. These are native
C++ Views widgets, so a content script or webpage CSS cannot restyle them.
The zoom entry has a verified binary-overlay patch. JavaScript dialogs now
also have a verified native manager hook and GTK implementation. Full-shell
checks verified alert/confirm/prompt decisions; beforeunload coverage is pending. Other entries remain source findings only.

| Surface | Native entry point | Replacement boundary |
| --- | --- | --- |
| Zoom feedback | `chrome/browser/ui/views/location_bar/zoom_bubble_coordinator.cc`: `ZoomBubbleCoordinator::Show` | Suppress the visual bubble while leaving real `ZoomController` zoom/events intact. This is the smallest initial binary-patch candidate. |
| Site permissions | `chrome/browser/ui/views/permissions/permission_prompt_factory.cc`: `CreatePermissionPrompt`, then `CreateNormalPrompt` / `CreatePwaPrompt` / `CreateQuietPrompt` | Replace the prompt factory with a GTK bridge that retains the request delegate and implements prompt lifecycle; returning null alone is not a replacement. |
| Permission bubble rendering | `chrome/browser/ui/views/permissions/permission_prompt_bubble.cc`: `PermissionPromptBubble::ShowBubble`; `permission_prompt_bubble_base_view.cc`: `CreateWidget` / `ShowWidget` | Own the visual presentation separately from permission decisions. Chip, quiet and embedded prompt variants must also be covered. |
| Extension install / extra-permission confirmation | `chrome/browser/ui/views/extensions/extension_install_dialog_view.cc`: `ShowExtensionInstallDialogImpl`, supplied by `ExtensionInstallPrompt::GetDefaultShowDialogCallback` | Route prompt data and the owned `DoneCallback` to GTK; preserve acceptance, cancellation, permission withholding and parent destruction. `ExtensionInstallPrompt::ShowConfirmation` prepares the data upstream. |
| Extension action popup, e.g. uBlock | `chrome/browser/ui/views/extensions/extension_popup.cc`: `ExtensionPopup::ShowPopup`, then `ShowBubble` | Keep the real `ExtensionViewHost` / extension WebContents, render that content in Gnosis's GTK popup, replace only the native Views wrapper. Do not suppress the extension page itself. |
| Extension list menu | `chrome/browser/ui/views/extensions/extensions_menu_view.cc`: `ExtensionsMenuView::ShowBubble`; newer access-control branch: `extensions_menu_coordinator.cc`: `ExtensionsMenuCoordinator::Show` | Keep Gnosis's GTK extension menu; avoid opening the native Views menu. There are two feature-dependent entry paths. |
| USB / HID / serial and other device chooser callers | `chrome/browser/ui/views/permissions/chooser_bubble_ui.cc`: `ShowDeviceChooserDialog` | Bridge the `permissions::ChooserController` choice list, updates and accept/cancel callbacks. This is separate from the ordinary permission factory and is not an exhaustive map of every device-specific chooser. |
| Passkeys / security keys | `chrome/browser/ui/views/webauthn/authenticator_request_dialog_view.cc`: `AuthenticatorRequestDialogView::Show` / `ReplaceCurrentSheetWith` | The controller at `chrome/browser/webauthn/authenticator_request_dialog_controller.cc` drives a multi-step model. GTK must handle model transitions and responses, not just hide the dialog. |
| JavaScript alert / confirm / prompt | `chrome/browser/ui/views/javascript_tab_modal_dialog_view_views.cc`: `JavaScriptTabModalDialogViewViews` | Replace the native view and preserve success/cancel/text and forced-close callbacks. Separate app-modal paths may also need coverage. |
| Open external application | `chrome/browser/ui/views/external_protocol_dialog.cc`: `ExternalProtocolHandler::RunExternalProtocolDialog` | Native libadwaita confirmation, routed by exact WebContents/CDP target; retains Chromium's launch callback and origin/protocol-specific grants. |

Brave-specific source wrapping verified at the pinned tag:

- `brave/chromium_src/chrome/browser/ui/views/permissions/permission_prompt_factory.cc`
  renames/includes Chromium's factory as `CreatePermissionPrompt_ChromiumImpl`;
  its public `CreatePermissionPrompt` handles wallet permissions and otherwise
  delegates to Chromium. This is the first permission factory to intercept.
- `brave/chromium_src/chrome/browser/ui/views/permissions/permission_prompt_bubble_base_view.cc`
  customizes the permission widget/buttons, so Chromium's styling alone is not
  the complete implementation shipped by Brave.
- Brave also wraps `extensions_menu_view.cc` and
  `extensions_menu_main_page_view.cc` in the corresponding `chromium_src` path.
  Its `chooser_bubble_ui.cc` wrapper includes Chromium's implementation.
- Direct override/patch files for `extension_popup.cc` and
  `extension_install_dialog_view.cc` were absent at the checked conventional
  paths. This is not proof that no indirect Brave modification exists.

Permission decisions are owned by
`components/permissions/permission_request_manager.cc`:
`RecreateView` invokes the factory; `Accept`, `Deny` and `Dismiss` complete
requests. A native bridge must retain origin/tab/private-context association,
permission options, navigation cancellation and delegate lifetime. A blanket
patch to `views::Widget::Show` or `BubbleDialogDelegate::CreateBubble` would
also suppress unrelated UI and leave some requests without a response.

Pinned sources can be opened at:

- Chromium: `https://github.com/chromium/chromium/blob/155.0.8059.40/<path>`
- Brave: `https://github.com/brave/brave-core/blob/v1.97.56/<path-without-brave-prefix>`

Investigation copies and a SHA-256 inventory are in
`/tmp/gnosis-popup-source-map/` (temporary; upstream links above are durable).
The identified zoom `Show` binary entry is: VA `0x0bab39d0`, file offset `0x0bab29d0` in the pinned Nix x86-64
ELF. A diagnostic-only write-and-return stub was reached twice by actual graphical
zoom requests; the final one-byte return patch preserves real 125% DPR, zoom
events, reset and sibling-tab isolation. Both probes used isolated profiles and
kept the Chromium sandbox enabled. The shipped manifest requires the complete
binary digest and original 64-byte prefix. No permission or extension binary
offsets have been established.


The second binary hook returns null from
`BrowserWebContentsDelegate::GetJavaScriptDialogManager` (VA `0x0b540040`, file
`0x0b53f040`). Chromium retains its real pending dialog callback in PageHandler,
but no longer constructs the native tab/app-modal dialog. Page session events
and `Page.handleJavaScriptDialog` carry the decision to/from GTK. Graphical
protocol checks verify `hasBrowserHandler=false`, alert continuation, confirm
cancellation and Unicode prompt input. The GTK adapter handles tab visibility,
requesting frame origin, prompts and pending-dialog disposal. This hook affects
only the private overlay and needs Gnosis's attached Page session; do not use
that overlay as a standalone browser without a dialog client. Extension native
popups and permission decisions are not patched by this change.

Full-shell GTK verification passed using accessibility actions scoped to Gnosis:
alert OK continues JavaScript, confirm Cancel returns false, prompt OK returns
Unicode input, and prompt Cancel returns null. Native manager absence was also
verified independently via hasBrowserHandler=false. Navigation beforeunload
verification and close-tab beforeunload are still pending.

### Extension install confirmation adapter

`native-install-ui.json` pins the already-patched executable hash and native
instruction spans used by `native-install-ui.c`. The private runtime launcher
preloads the small adapter only for the actual Brave ELF when the GTK shell
supplies inherited SOCK_SEQPACKET fd 5. Renderers receive no decision channel.
The constructor checks the full ELF hash and original instructions before
patching its own process; unsupported binaries retain native presentation.

The hook replaces the final Widget::Show call after Brave constructs its real
ExtensionInstallDialogView and InstallPromptData. It exports that model's title,
localized buttons and actual permission warnings. GTK returns an explicit
accept/cancel response to the existing native callbacks on the browser UI thread.
A destructor hook retires externally closed requests. Disconnects cancel pending
requests; neither the helper extension nor a website binding can approve them.

Only basic install, re-enable and permission confirmations without a withholding
checkbox or enterprise justification are adapted. Other variants keep native UI.
GTK ownership is limited to the unique mapped tab in the active window of the
same Brave process; this is not yet an exact native requesting-tab identifier.
Tab hide/close/navigation cancels its pending GTK confirmation. Both GTK and the
native adapter enforce a 500 ms acceptance delay, with Cancel as the default.

On upgrade, re-identify the Show tail, destructor trampoline, native model field
and libc++ layouts, localized getters, widget Close and decision callbacks.
Re-run cancel and accept against the real Web Store beginInstall API in a
throwaway profile before updating this pin. These probes must never call
completeInstall merely to validate the confirmation presentation.

### Shared device chooser and file-access confirmation

The shared device adapter retains DeviceChooserContentView and its native
ChooserController. Only the tracked chooser's Widget::Show is suppressed.
Native view notifications forward option/adapter changes to GTK and increment
an identity revision, so an old selection cannot grant access after hotplug.
Responses validate the revision, sorted device indices, adapter availability
and the native single/multiple-selection policy before invoking Select.
Cancellation and native destruction retire the same request. No device is
opened by this bridge.

GTK list callbacks are connected to the dialog's lifetime and explicitly
removed before list replacement or dialog closure. Full-shell USB selection
and Connect were exercised with GTK criticals fatal; the native request resolved
successfully and the GTK process stayed alive. USB/HID/Serial cancellation also
passed. HID/Serial acceptance and Bluetooth adapter/rescan UI are unverified.

File-system access uses another native DialogModel. Its scoped creation hook
tracks the model's actual native host, keeps native ownership and exports the
localized title, buttons and directory/origin paragraph. GTK decisions call the
model's original accept/cancel actions, then close its native widget. This is
separate from the XDG FileChooser selecting the path. This packaged bridge still
needs an actual directory-selection acceptance/cancellation check.

### Wayland screen/window capture

For requests with a delegated screen/window source list, the adapter keeps the
native DesktopMediaPickerDialogView alive and selects the corresponding
portal-backed category. Standalone widgets suppress Widget::Show. Embedded
web-modal requests use a separate CreateMediaPickerDialogWidget hook: it creates
the original owned native widget without registering/displaying a second modal
through WebContentsModalDialogManager. This separate path was necessary after
user testing showed Chromium's picker underneath the portal despite the
standalone Show hook. The user confirmed the packaged web-modal fix works, and
runtime logs show portal-only widget creation followed by explicit-selection
completion. Native construction retains
all original source lists, filters, capture options and ownership. Requests that
prefer browser-tab capture retain their existing picker pending a GTK tab chooser.

The public GIO SelectSources transport preserves the session, options, callback
and cancellation. For one unambiguous active picker whose native lists allow both
screen and window sources, it requests portal types=3 so the system selector can
offer both. Restricted/window-only lists are never expanded; concurrent pickers
retain their native source masks. Counts shared with the portal worker use GLib
atomics.

An explicit delegated-source selection and completed native enumeration are both
required before the original AcceptSpecificSource callback is called. Native
source-added and portal-selection events can arrive in either order. Portal
cancellation invokes the native reject path. Entry hooks cover both direct and
virtual observer calls: LTO devirtualizes these callbacks in this exact binary.
The user confirmed that window selection completes through the combined portal.
Browser-tab capture UI, concurrent capture and all capture-policy variants still
need coverage. Selecting a portal window through the screen-backed list may retain
Chromium's monitor classification in getSettings().displaySurface; the actual
captured pixels come from the portal-selected window.

### Full-page translation

The native shim now replaces only `ChromeTranslateClient::ShowTranslateUI`
(`0xba5f110`). The private inherited socket resolves the requested CDP target
through `DevToolsAgentHost::GetForId` (`0x4bceca0`), its WebContents and the
ChromeTranslateClient user-data key (`0x2db2e6c`). Exact vtables and driver /
WebContents ownership are checked before using the per-tab manager. Requests do
not accept renderer-provided addresses. A native page sequence rejects stale
start/revert requests; GTK also retires controls on navigation and tab close.

`TranslateManager::TranslatePage` (`0xecc31b0`) retains Brave's real translation
script/service and native LanguageState; `RevertTranslation` (`0xecc4660`)
restores the original DOM through the upstream driver. The shim reports native
steps/errors without retaining raw native objects across requests. Every called
entry and the bubble hook has an exact byte span in `native-install-ui.json`,
in addition to the whole-binary hash. No Chromium compilation is needed.

Pinned reference source: Chromium 155.0.8059.40,
`components/translate/core/browser/translate_manager.cc`,
`components/translate/core/browser/language_state.h`, and
`chrome/browser/translate/chrome_translate_client.cc`. Native Russian-to-English,
revert, invalid-language and stale-sequence probes pass; the packaged GTK submenu
also translates and restores actual page content without a dialog. Language
selection currently exposes the common supported languages in the GTK list;
translation itself remains upstream, including native unsupported-language errors.


### External application confirmation

`native-external-ui.inc` recognizes only the pinned ExternalProtocolDialog
vtable at ShowWebModalDialogViews. It creates the same owned native widget and
parent observer without showing a second Views modal, then presents the native
localized title/body/buttons and optional checkbox in the owning GTK tab. The
existing inherited SOCK_SEQPACKET channel returns Cancel/Open/remember to the
original DialogDelegate callback. Chromium retains handler lookup, initiating
origin/document, external-launch policy and remembered origin/protocol grants.
The checkbox starts unchecked and is omitted when native policy omits it.
Navigation, hidden tabs, parent destruction and channel loss cancel the request;
unknown or hidden targets never borrow another tab. The existing 500 ms input
guard remains, and Cancel is the default/close response. `external-apps` in
Debugging restores the upstream prompt for diagnosis. Every new native entry
is checked against the exact supported ELF digest and instruction prefix.

Reference: Chromium 155.0.8059.40
[external_protocol_dialog.cc](https://github.com/chromium/chromium/blob/155.0.8059.40/chrome/browser/ui/views/external_protocol_dialog.cc).
The adapter is original GPL code; the upstream BSD implementation remains in
the browser and is called in place.
