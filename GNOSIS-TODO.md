# Gnosis progress

Updated: 2026-10-26. The active migration embeds Brave in Epiphany's GTK shell;
current implementation details and remaining gaps are in `BRAVE-MIGRATION.md`.
The CEF entries below preserve earlier work and do not establish Brave coverage.
Checked items describe verified behavior,
not complete coverage of the surrounding feature. Development checkpoints live
on `main`; nothing here implies a published release.

Status: `[ ]` planned, `[o]` in progress, `[x]` verified complete.

- [x] Read the GTK extension inventory and toggles from real Brave management,
  including live native install/remove events and actual Web Store installs.
  Exclude the packaged integration worker and stop seeding fake/stale uBlock
  rows from CEF's bundled catalog. Full native uBlock regression passed.
- [x] Load validated unpacked installations into Brave immediately; reject
  regular-profile extension management from ephemeral sessions.
- [x] Replace extension uninstall confirmation with the native-model GTK bridge;
  native and actual GTK Cancel/Remove/report-checkbox checks pass.
- [x] Add the GTK zoom OSD: current percentage, three-second timeout refreshed
  by further zoom changes, native GTK overlay instead of the Chromium bubble.
- [x] Verify reader-mode transport and automatic translation-offer suppression.
- [x] Add Translate Page as a GTK site-menu submenu using native full-page
  translation, language selection, progress/errors and native Show Original.
  Actual GTK submenu translate/revert/navigation and native stale-page checks pass.
- [o] Investigate the 2026-10-25 15:05:12 GPU-process SIGTRAP freeze. Fix the
  separately confirmed LD_PRELOAD leak into external tools without claiming
  that this alone fixes the GPU crash.

Custom tab experiment is archived on `archive/select-custom-tabs`. The active
shell uses the original AdwTabBar and AdwTabOverview again. Restored-title and
favicon fixes, unrelated settings work and active-tab callback cleanup remain.

Current settings repair (2026-10-25):
- [x] Wire ad-blocking to full uBlock Origin, disable Brave's independent ad /
  cosmetic filtering, and keep unpacked extensions enabled through self-reload.
  Actual GTK GSettings off/on and real network/cosmetic/scriptlet checks passed.
- [x] Honor custom/blank homepages for new tabs; preserve private-mode defaults.
- [x] Give Ctrl+Tab an opaque native surface and contrasting selection/text.
- [x] Expose real Chrome DevTools in a transient GTK window, with F12 / Ctrl+Shift+I
  and a hamburger-menu entry. Keep inspector windows out of the shell tab list.
- [x] Move cookies/cache/site-data management to Brave. Cookie path/domain deletion,
  native Website Data UI, storage clearing and private/normal isolation passed.
- [x] Apply custom fonts, reader style, custom CSS/JS and default zoom to Brave.
  Actual GTK preference enable/disable, reader-style and default-zoom checks pass.
- [x] Add the online extension-store submenu for Chrome Web Store and Microsoft
  Edge Add-ons. Actual GTK activation opens a new store tab; the Chrome catalog
  may redirect to Google consent. Edge installation itself is not verified.
- [x] Keep the native webdriver getter false in ordinary browsing, while retaining
  explicit automation mode, the private pipe and Chromium sandbox. The user
  confirmed Google sign-in proceeds. Suppress native infobar presentation
  at the container layout boundary. CAPTCHA frequency has not been attributed or independently fixed.

Current focus: reliable restored-tab allocation, then native GTK context menus,
permission prompts and the remaining browser dialogs. WebKit and CEF removal
continues as their services move to Brave. Prepared patches are source artifacts,
not fixes present in the running browser.

Build preference: use all available CPU cores (currently 24 logical CPUs),
using `nix run` for build-and-launch and native builds with `-j$(nproc)`.
Keep Chromium memory core dumps disabled.

## Current work

- [ ] Fix multiple keyboard layout support. User report (2026-11-01): Gnosis
  appears to recognize only one configured layout. Verify layout detection and
  live switching for typing and shortcuts in the GTK shell and embedded Brave.
  Cause and reproduction remain unverified.
- [x] USB, HID and Serial device choosers use GTK with native device models,
  revision checks and native grant/cancel callbacks. Fixed the GTK list callback
  lifetime crash; full GTK USB selection passed and the user confirmed all three
  work. Bluetooth adapter/rescan UI still needs separate coverage.
- [o] Replace the post-directory-selection file-access confirmation separately
  from the portal chooser. Native DialogModel bridge is packaged; verify the
  directory/path text and accept/cancel results in the running app.
- [x] Route ordinary Wayland screen/window sharing directly through XDG
  ScreenCast without Chromium's extra picker. The portal offers both screens
  and windows when both are allowed by the native source lists. User confirmed
  window selection completes. A separate web-modal creation hook prevents the
  Chromium picker appearing underneath; user confirmed the packaged fix works.
  Runtime logs verify portal-only creation and explicit-selection completion.
  Preserve native source models and callbacks;
  wait for explicit portal selection plus enumeration before accepting.
- [ ] Port browser-tab-only/prefer-browser capture selection to GTK. It currently
  retains the native picker; the portal screen/window bridge does not cover it.

- [o] Fix right-click Copy: retain the action callback across popover dismissal
  and publish text/link/image addresses during the real GTK activation, before
  destroying the popup's Wayland surface/input serial. Native edit chords still
  wait for the keyboard grab to release. Packaged user check pending.
- [x] Rebrand native desktop notifications as Gnosis using the freedesktop
  Notify transport, retaining native IDs/actions and website content. Actual
  delivery and Gnosis app name/desktop-entry are verified; the packaged icon
  points to Gnosis's SVG. User confirmed delivery before this branding change.
- [o] Enable real file/directory pickers: pinned Brave disables
  FileSystemAccessAPI by default, so the methods were absent. Enable that base
  feature; a windowed native probe then invoked XDG FileChooser.OpenFile.
  Packaged file/directory result and cancellation checks remain pending.
- [o] Enable BraveWebBluetoothAPI and Linux's WebBluetooth Blink runtime gate.
  Native requestDevice is now exposed, alongside the real file/directory APIs.
  Verify chooser behavior/hardware; bridge its UI alongside USB/Serial/HID.
- [ ] Diagnose protocol-handler registration on permission.site. An absent
  visible dialog does not establish that registration failed; inspect native
  pending-handler approval and connect that approval to GTK.

- [o] Handle Widevine-requested relaunches inside the GTK shell: intercept the
  native relaunch callback, restart only Brave with fresh private pipes, and
  reconnect the existing GTK tabs. Isolated native callback and full two-tab
  GTK reconnection passed; module-download/private-context coverage pending.
- [o] Replace the native site-permission bubble with GTK using its actual title,
  allow/allow-once/block/dismiss callbacks. permission.site native Block/Allow,
  notification delivery, navigation retirement and stale-response checks pass.
  Full GTK permission.site Notifications button became success/granted in a
  disposable profile; user also confirmed the prompt works. USB/Serial/HID and
  screen-sharing chooser dialogs remain separate and still use Chromium UI.
- [ ] Replace Brave's translation bubble, including the leak into reader mode,
  with GTK controls connected to its real translation backend.
- [o] Route screen/window sharing through the XDG ScreenCast portal and remove
  Chromium's initial/final source picker. Real portal CreateSession, SelectSources
  and Start are verified; this does not prove the Chromium picker is removed.

- [o] Match webpage text selection to the live libadwaita accent and replace
  Chromium scrollbar appearance with libadwaita styling/behavior.
  Browser-owned per-document styles now follow GTK accent/contrast changes,
  including strict-CSP pages. The pinned CustomScrollbar overlay classification
  and native OverlayScrollbar mode remove the root/nested layout gutter while
  preserving wheel and thumb dragging. Track, track-piece and corner backgrounds
  are transparent, including on doromiert.neg-zero.com. Native integration checks
  pass; packaged visual acceptance and out-of-process frame coverage remain.
  Follow-up state adapter runs in an isolated world without native bindings or
  scroll interception: 200ms hover/leave/press interpolation and a hover-only
  10% track (25% in high contrast). Root/nested independent state, reduced motion
  and native dragging pass. Blink's incompatible overlay fade timer is disabled;
  idle indicator auto-hide and out-of-process frames remain follow-ups.
  User testing exposed a missing viewport style-source update: Blink prefers
  body when it has custom scrollbar rules, while the adapter updated only html.
  The adapter now mirrors viewport state to both sources, and initializes once
  per isolated context. Actual GTK-rendered captures show the expanded thumb
  and hover track on Chrome Web Store; computed CSS alone was insufficient.
  A native scrollbar can emit pointerout(null) without leaving its viewport hit
  area. Keep hover when its coordinates remain inside that area rather than
  collapsing beneath the pointer. Boundary-event, native drag, nested scroller
  and reduced-motion checks pass; a GTK seat probe holds an 8px painted thumb
  and visible track for 20 seconds, returning to 3px on actual leave.
  Follow-up appearance preserves libadwaita's actual alpha instead of blending
  it onto a solid GTK view background: white 10% track and 20/40/60% normal,
  hovered and pressed thumb in dark mode. The earlier solid gray palette was
  an incorrect interpretation of reference pixels composited on Nautilus's
  background. Track geometry now shares the thumb's animation progress, growing
  from 3px to 8px instead of fading in at full width. 9px end margins, the outline,
  and the user's white collapsed indicator/7px inset are retained. White alpha,
  intermediate track growth, native drag, independent axes, nested scrollers,
  boundary events and reduced motion pass. User visual recheck remains pending.
- [x] Keep Web Store installer downloads out of GTK's ordinary-download manager.
  Filter the native DownloadSource::EXTENSION_INSTALLER before its DevTools event
  and observer registration; GTK must not move a CRX before native verification.
  Real Google Translate installation reports enabled/Remove from Brave in an
  isolated profile. Ordinary TXT and manually downloaded CRX transfers still
  emit events and preserve exact bytes. Packaged user recheck remains.
- [x] Suppress native infobar banners at the shared preferred-size boundary;
  no per-flag workaround, underlying flag change or empty content gutter.
  Actual GTK/webpage viewport comparison passes.
- [x] Bridge desktop/GTK file drops into native webpages and webpage links out
  to GTK destinations. Real file contents, trusted DOM events, link payloads,
  Escape cancellation and input recovery pass. Outer Wayland/Nautilus human
  testing remains pending.
  Source-derived image/text/mixed/link previews pass real desktop drag checks;
  image and text cards remain compact squares with rounded corners.
- [x] Add Copy Image with original RGBA pixels and transparency on the clipboard.
  Cross-origin images, data/blob URLs and same-process subframes pass. The
  preexisting out-of-process iframe context-menu gap remains follow-up work.
- [x] Rebrand the product and executable as Gnosis using the supplied SVG;
  retain stored profile/schema/application IDs and old command/package aliases.
  About and AppStream credit the Negative Zero team.
- [x] Remove the translation controls' forced width from the page menu;
  native sizing, unclipped submenu contents and back navigation pass.

- [x] Add a maintainable native Brave patch series and upgrade gate: one stable
  component file, opt-in switch, pinned release/source/patch hashes, zero-fuzz
  application and exact reversal. Source checks pass for Brave 1.97.56 /
  Chromium 155.0.8059.40 and candidate Brave 1.100.31 / Chromium 156.0.8078.48.
  Run `nix run .#check-brave-patches`; compilation/runtime are separate gates.
- [x] Suppress Brave's native zoom bubble using a private, prebuilt binary
  overlay. Exact executable SHA-256, original bytes and patched digest are gated;
  one byte makes ZoomBubbleCoordinator::Show return before widget creation.
  A diagnostic graphical probe confirmed the hook is reached. Final patched
  graphical checks preserve real 125% zoom/DPR, reset, zoom events and sibling
  isolation. Default `nix run` selects this overlay; user confirmed the native
  popup is gone.
  Site permission popup replacements are separate future work.
- [x] Route JavaScript alerts, confirmations and text prompts through GTK.
  A second exact-hash binary hook returns no native dialog manager while CDP
  retains real decisions. Isolated graphical checks verify the native manager
  is absent. Full-shell accessibility actions verified alert continuation,
  confirm rejection, Unicode prompt input and prompt cancellation against
  actual webpage JavaScript results.
- [x] Native beforeunload preflight now protects tab/window close. Real handlers
  and Keep Open/Leave decisions run without destroying earlier approved tabs.
  Actual GTK last-tab, two-tab window and two-window application-wide Cancel/Leave tests pass. Missing
  native replies offer explicit Keep Open/Close Anyway, verified by fault injection.
  Existing form-warning Cancel/Discard and nine edited-form cases still pass.
  Navigation/interaction invalidate prior approval; broader live-editor coverage
  and interactions inside cross-process frames remain follow-up validation.
- [o] Replace Brave's extension install confirmation with GTK while preserving
  its actual permission model and install/cancel callbacks. A small exact-binary
  adapter exports the native title and warnings over an inherited private socket;
  the GTK dialog defaults to Cancel and delays acceptance. Both original callback
  results pass isolated Web Store beginInstall probes (no completeInstall or
  extension installation). User confirmed the GTK dialog works after routing
  was limited to an actually mapped tab. Enterprise justification and permission
  withholding checkbox variants still use native UI; broader coverage is pending.
- [o] Port the extension menu and popup host to Brave's actual extension backend.
  Current GTK menu management still queries CEF, and Brave startup does not load
  the installed Gnosis catalog. Preserve real ExtensionViewHost popup semantics,
  activeTab grants and full uBlock Origin; do not substitute an ordinary tab.
  Isolated Brave successfully loads full uBlock 1.75.0 and reports its native
  inventory. Extensions.triggerAction requires a `tab` target, not a `page`
  target; targeting an actual tab inside the current --app window crashes the
  isolated Brave process. Resolve native app-window popup hosting before wiring
  the GTK action row. Main Gnosis was not involved in that crash.
- [x] Port find-in-page to Brave's actual FindTabHelper, with native counts,
  ordinals, wrapping, case/Unicode matching and same/cross-process frame search.
  Native and real GTK toolbar tests pass; unsupported whole-word matching is
  hidden. Replaced, stopped, navigated and closed-target requests are retired.
- [o] Connect GTK printing to Brave's native PDF renderer. The GTK printer setup
  now drives paper size, orientation and margins; tab hide/close cancels the job.
  Windowed Brave renders valid portrait/landscape A4 PDFs and applies print CSS
  (screen-only content excluded). User confirmed Ctrl+P opens the packaged GTK
  print dialog. Saving through that dialog and physical spooling remain pending.

- [ ] Match sliders to libadwaita styling and behavior instead of Chromium's
  defaults, including webpage range inputs. Cover track/thumb appearance,
  hover/focus/pressed/disabled states, dragging, keyboard steps and RTL/orientation
  while preserving web form values and accessibility semantics.

- [o] Fix restored Brave tabs retaining their startup viewport: configure on map
  and preserve positive dimensions while hidden. User confirmed the original
  attempt fixed sizing, but reported freezes beginning with that change.
  Frame caching, screenshot serialization/resize holds and forced maximization
  have been reverted to the exact preceding build. Retest this smaller fix.
  User subsequently reported substantially improved stability; longer-running
  resize/session validation remains open.
- [o] Replace Brave right-click menus with native GTK context menus: page,
  link, image-address, selection and editing actions implemented. User confirmed
  menus open. Firefox-style flat navigation icon row and overflow placement
  added; remaining media/download/source/inspector actions are not ported.
- [o] Stabilize resize/menu/shutdown: remove compositor listeners before destroying
  their wlroots owners. Exact teardown assertion identified in real crash logs;
  patched isolated processes exit without that assertion. Resize reliability
  still needs verification in the full shell.
- [o] Fix selection after native context menus: release held Wayland buttons
  before a GTK popup grabs input, ignore duplicate releases, and clean up on
  gesture cancellation/tab hiding. Content now claims its drag sequence.
  Wider equal-width flat navigation buttons are included. Runtime confirmation
  of selection and frequent tab-load failures remains pending.
- [o] Diagnose tab-load crashes with a preserved main Brave log, separate launch
  helper logs, actual subprocess exit reporting and renderer crash events.
  Guard the remaining CEF-only extension-load callback against a missing CEF
  browser when the view belongs to Brave.
  User clarified these are frozen views, not disappearing tabs or Aw, Snap pages.
  Queue restored-tab launchers until the initial browser handshake completes.
- [ ] Connect permission requests and browser dialogs to the GTK shell.

## Earlier CEF work

- [x] Serve the About page's GNOME icon through native resources instead of a
  blocked local-file URL. Developer/Nix builds and actual packaged rendering pass.
  Latest package: `/nix/store/payp5a0dn8q96jfqcg3bnjlll2z8xyg6-gnosis-0.1.0`.
  Evidence: `build-cef/verification/native-about-resources/`.

- [x] Integrate cold-start extension popup readiness. Persistent MV2 backgrounds
  now reach native document/runtime readiness before opening their action popup;
  lazy backgrounds and MV3 workers keep their Chromium messaging behavior.
  Actual two-window uBlock popup shows the correct owner site, options stay in
  the owning window, strict native/private/close checks, full uBlock and MV2/MV3
  gates pass. Packaged two-window popup/settings and normal quit also pass.
  Package: `/nix/store/5gkwifnsgfzvh9hxw7ipf5bd1drsvf51-gnosis-0.1.0`. Evidence:
  `build-cef/verification/extension-popup-readiness/`.
- [o] Remove foreign WebKit hit-test commands and empty context_event storage.
  Native CEF menu actions remain authoritative; integrated build passes.
  Actual link/image/edit menu checks remain pending.
- [ ] Port missing native context-menu actions: Set as Wallpaper, email sharing,
  bookmark clicked link, and apply enterprise lockdown to actual CEF actions.
  Removed legacy action bindings never controlled the native CEF menu.

- [x] Remove twelve never-emitted WebKit compatibility signals and dormant
  consumers, and reconnect OpenSearch discovery to native load completion.
  Native/Nix builds pass; libraries no longer import legacy policy, script-dialog
  or notification functions/types. Packaged OpenSearch menu shows only the valid
  discovery entry and clears it after navigation. About reports Gnosis and real
  CEF/Chromium versions. Rapid navigation/close OpenSearch cases remain a separate
  follow-up; their replies already check native document identity and cancellation.

- [o] Integrate native-only extension manager/model and stop compiling the twelve
  retired WebKit/JSC API providers. Source cleanup and UI callback migration are
  landed. Integrated build, full-uBlock network/cosmetic/scriptlet/frame, MV2
  lifetime/host isolation and MV3 runtime/blocking/host-isolation checks pass.
  Nix build and packaged native menu/popup/settings pass; remaining detail-toggle
  and install-state workflows remain pending.
- [o] Polish native extension menu sizing, rows/settings spacing, focus, loading
  states and accessibility. Packaged menu, real uBlock popup and owning-window
  settings pass. Pending/settings-only/multiple-row cases remain separate checks.
- [o] Fix private/default window relaunches to use the current executable with
  explicit arguments instead of a missing PATH lookup. Source landed; verify
  absolute-path launches and private-window behavior. Developer and packaged
  GUI pass actual Ctrl+Shift+N with gnosis absent from PATH, distinct private
  process/window and clean quit. Private-link/argument-boundary cases remain pending.
- [o] Implement native Chrome Web Store installation. The actual Add button crash
  is reproduced in Chromium's install-dialog creation stack. A temporary guard
  now shows an accurate native unavailable notice; actual Add, Close and normal
  quit pass. This does not install extensions; complete native install-dialog
  integration remains unimplemented. Evidence:
  `build-cef/verification/webstore-crash/guard-validated/`.

- [o] Disconnect retired WebKit extension-provider startup: remove its custom
  resource handler, WebKit match-pattern scheme registration, and legacy download
  event subscriptions. Chromium owns the native extension resources and APIs.

- [x] Remove the legacy XPI/manifest installer UI, extraction backend and silent
  local-XPI installation route. Native unpacked installation remains the supported
  path into the CEF store; legacy files follow ordinary browser file navigation.
  Native/Nix builds and full uBlock pass. Actual packaged GTK installations of
  both MV2 and MV3 run real content scripts after restart; both launches quit
  normally. Old provider API source cleanup remains separate. Evidence:
  `build-cef/verification/native-only-installer/`.

- [x] Stop importing the default profile’s legacy WebKit extension directory
  into native CEF extension catalogs. Use the native owned store/inventory for
  the running profile; retain existing files on disk untouched. Native/Nix builds
  and full uBlock pass. Actual packaged preferences show native uBlock and exclude
  a separate default-profile legacy sentinel, whose manifest remains untouched.
  Normal quit passes. Old provider API source cleanup remains separate.
  Evidence: `build-cef/verification/native-catalog-scope/`.

- [x] Remove the dormant WebKit page-context-menu pipeline and allocated empty
  WebKitEditorState. Actual GTK menus already use Chromium’s native menu model
  and edit-state flags; keep that path authoritative. Native/Nix builds, actual
  packaged selected-text Copy menu rendering, delayed restore and normal quit
  pass. Editable/private/frame coverage remains tracked below.
  Evidence: `build-cef/verification/native-menu-cleanup/`.

- [x] Remove the shell’s unused WebKitWebContext, obsolete renderer password
  broadcast and WebKit-only automation callbacks. Keep native CEF session
  isolation and profile-directory creation; native WebDriver remains unported.
  Native/Nix builds and actual packaged delayed session restore and normal quit
  pass. Evidence: `build-cef/verification/drop-shell-webkit-context/`.

- [x] Stop building/installing the unused WebKit renderer plugin. Remove its
  shell initialization data and the unused JSC permission-export bindings; drop
  the WebKit web-process-extension build dependency from autofill storage.
  Native and Nix builds, full uBlock, MV2 lifecycle and MV3 checks pass.
  The clean Nix output contains no WebKit renderer plugin. WebKitGTK/JSC
  dependencies still remain for other unported services. Evidence in
  `build-cef/verification/drop-webkit-renderer/`.

- [o] Deliver native CEF subresource response completions to the shell and move
  Firefox Sync challenge retry off the inert WebKit resource-start callback.
  Old-navigation generations are checked before UI delivery, including redirects.
  Native iframe metadata and existing response/history/close checks pass, as do
  full-uBlock network/cosmetic/scriptlet/frame checks and the Nix build. Actual
  Firefox Accounts challenge remains pending. Real held/delayed responses now
  verify main/subframe navigation, redirect chains, hide/show, close and stale
  reply rejection. Evidence: integrated-native-cleanup/gnosis-integrated-response-check.log.
  Evidence: `build-cef/verification/native-resource-response/`.

- [x] Remove WebKit request allocations from delayed session restore and the URI
  loading wrapper. Real GTK restore verifies foreground loading, background-tab
  delay, loading on activation, and normal quit in development and the Nix package.
  Evidence: `build-cef/verification/native-delayed-uri/`.

- [o] Restore last-upload-folder memory in the native CEF file picker, scoped to
  the owning browsing session so private folders do not reach regular settings.
  Removed the inert WebKit file-chooser class hook and implementation. Native
  and prior Nix builds plus seven real file-portal operations pass. Current native
  runtime verifies nine remembered-folder stages across two processes: regular
  persistence, private reuse/isolation/reset, explicit path precedence and cancel
  preservation. The old save regression runner lost Unicode during xdotool typing;
  its independent portal response identified that harness issue. Corrected driver
  now passes all seven file-portal operations, including UTF-8 save and exact bytes.
  Final package verification remains pending. Evidence in
  `build-cef/verification/native-upload-folder/`.

- [o] Replace WebKit client-certificate authentication with CEF candidate selection
  and a native GNOME chooser. Integrated into the main view/window and both build
  definitions; removed the old WebKit handler and its unused GCK build dependency.
  Native Gnosis build
  and the shared HTTP-authentication lifecycle regression pass. Real client-TLS
  successful client-certificate handshake remains pending. Actual packaged GTK
  empty-candidate chooser and Close pass against a local TLS server; the server
  confirms no client certificate was sent, and the browser exits normally.
  Certificate import and token PIN handling remain separate work. Nix build passes; evidence in
  `build-cef/verification/client-certificate/status.json`.

- [x] Replace the inert WebKit HTTP password-authentication path with actual CEF
  challenges and GNOME username/password dialogs. Six real native gates verify
  Basic retry, Digest, proxy 407, independent regular/private credential caches,
  once-only answers, navigation/hide/unroot/close and pending engine shutdown.
  Remove the obsolete WebKit HTTP credential/default-dialog pipeline. Actual
  regular/private GTK dialogs pass real typing, wrong-password retry, accepted
  Basic/Digest exchanges, cancellation and normal quit in both development and
  the Nix package with development paths removed. Protected URLs are opened
  directly; URL-entry completion is now fixed below. The final Nix package also
  passes the actual proxy-407 dialog, accepted credentials and normal quit after
  reopening a profile configured through native SetPreference/GetPreference.
  The dialog shows the actual proxy recipient. Persistent password saving and
  client-certificate integration remain separate work.
  Package: `/nix/store/3d7zjy8wcwyynqa0dmgkg6vnz0g39cd1-gnosis-0.1.0`.
  Logs: `/tmp/gnosis-native-auth-complete.log`, `/tmp/gnosis-http-auth-ui-direct.log`,
  `/tmp/gnosis-http-auth-final-packaged-ui.log`. Final MV2 lifecycle, MV3 host-isolation
  and full-uBlock network/cosmetic/scriptlet/frame checks all pass. Persistent
  evidence and the scoped acceptance record are in
  `build-cef/verification/http-auth/status.json`.
- [x] Fix URL-entry completion retaining a previous suffix during real GTK typing.
  Exact entry text and committed-page URL checks reproduce it independently of
  authentication. Native backtraces identify GTK X11 SelectionClear handling;
  synchronous ownership plus XRes proves GTK already owns PRIMARY when its stale
  clear event collapses the completion selection. Normal idle scheduling passed
  with traces but failed again without them, so that change alone is not a fix.
  Gnosis's private GTK build now checks actual current selection ownership
  before accepting queued SelectionClear/XFixes ownership loss. Native tests
  reproduce the failure with upstream GTK, preserve local ownership after reclaim,
  and still accept genuine foreign takeover. CEF and the system GTK are unchanged.
  Decode presentation markup before inline completion, preserving raw query `&`;
  retain the original completion scheduling priority. Quiet packaged GUI checks
  pass exact entry text and committed URLs at 1/5 ms typing, history/query length
  changes, and paused inline completion. The actual process maps exactly one GTK,
  the patched build, with development library paths removed. Rich clipboard,
  ownership, permissions, focus, Cut/Paste and reload regressions also pass.
  Package: `/nix/store/j5gxqp19rapx3b46kl30agi5wg5j580g-gnosis-0.1.0`.
  Evidence: `build-cef/verification/location-entry/`.
- [x] Fix About/app metadata: Gnosis 0.1.0, actual CEF/Chromium diagnostics,
  upstream credits preserved.
- [x] Rebuild the Nix package and inspect the corrected About dialog; verified
  Gnosis branding and retained GNOME icon with development library paths unset.
- [x] Replace the null main-resource response stub with actual Chromium metadata;
  verify redirects, error responses, Unicode filenames, subframes, history
  restoration, and resource lifetime. Remove the old WebKit response-policy code.
- [x] Rebuild and check the Nix package with the response migration included:
  version output, About dialog, concurrent downloads, and normal shutdown pass.
- [x] Replace WebKitSettings/WebKitFeature with native Chromium preferences:
  fonts, languages, HTTP/JavaScript identity, spellcheck/caret preferences,
  storage policy, and restart feature switches. Verify live changes in regular
  and private tabs and background downloads; rebuild and check the Nix app.
- [x] Fix preferences dialog ownership so Reset All and normal shutdown do not
  leave a dangling dialog pointer. Eight affected checks, real full uBlock,
  MV2/MV3 fixtures, and desktop clipboard/WebGPU pass after the migration.
- [x] Cancel file choosers when their page is hidden, breaking the parent-window
  lifetime cycle on close. Real GTK portal checks verify save-path selection,
  single/multiple uploads, native cancellation, hiding the page, and closing the
  window; D-Bus confirms six requests and both pending-request Close calls.
- [x] Rebuild the Nix package with the chooser lifetime fix; verify the real
  packaged browser reads a portal-granted file with sandboxed renderers, closes
  a pending portal on quit, and still passes the preferences UI check.
- [x] Enforce each session's storage policy on CEF's intercepted cookie path.
  Verify HTTP and JavaScript cookie blocking, rejected Set-Cookie responses,
  regular/private downloads, restored cookie access, persistent cookies, and
  explicit private-session overrides after live preference changes.
- [x] Rebuild and verify the package after the intercepted-cookie fix: actual
  sandboxed portal uploads, pending-chooser shutdown, preferences Reset All,
  concurrent downloads, full uBlock, MV2/MV3, and desktop clipboard/WebGPU pass.
- [ ] Finish migrating site-data management to Chromium's actual stores; keep
  enumeration and clearing accurate for persistent and private contexts.
- [x] Route HTTP cache clearing from the existing Website Data dialog to CEF.
  Verify actual eviction, cancellation, regular/private isolation, preservation
  of cookies, checkbox persistence, filtering, and normal/incognito dialog use.
  Wait for operations before showing success; keep hidden selections out of
  clearing. Nine native checks pass. Packaged normal/incognito clearing passes.
  One earlier normal-mode quit timed out; subsequent packaged and development
  runs quit normally. Preserve screenshots/window details on future failures.
- [x] Move Website Data's cookie list and deletion to the native CEF session.
  Verify per-domain clearing and all-cookie clearing, HttpOnly and CHIPS cookies,
  cancellation, invalid domains, other-domain preservation, and private-session
  isolation. Actual regular/incognito dialogs preserve localStorage and quit
  normally. Background legacy queries no longer reset native row selections;
  asynchronous fetch callbacks hold weak references to the dialog.
  Keep the GTK filter predicate free of row-expansion side effects and make
  category visibility independent of child insertion during asynchronous refresh.
  Rebuilt the Nix app; regular/incognito packaged workflows and normal quit pass
  without development library paths. Nine native checks and real full uBlock
  pass. GUI checks explicitly focus the search entry before typing or editing.
- [ ] Use a service-level site-data API for private contexts. A Chromium
  Settings helper successfully enumerated real regular-profile storage, but
  the pinned SDK crashes in its sign-in localization for off-record profiles.
  Keep that probe out of the app and do not fall back to the regular profile.
- [x] Verify Chromium quota enumeration without the Settings helper: real
  IndexedDB buckets appear in regular/private contexts with their own storage
  keys and usage; private enumeration excludes regular-profile buckets. This
  remains an isolated diagnostic probe, not a production site-data adapter.
- [x] Evaluate native scripts in actual Chromium named isolated worlds. Verify
  shared DOM, isolated globals, cross-origin boundaries, private/tab isolation,
  source URL escaping, cancellation from another thread, real navigation,
  hiding, moving, and normal window closure. Pin evaluations to unique contexts
  and actual document epochs, so starting a download does not cancel them.
  Ten native checks pass, including downloads with and without an evaluation
  callback; reachable pending-promise fixtures also pass three repeated runs.
  Real uBlock and MV2/MV3 permission/isolation fixtures pass.
- [x] Stop exposing internal alternate-page data URLs in the address bar,
  history, resource address, and saved session. Retain opaque page origins;
  load actual destinations when restoring/selecting placeholders or reloading.
  Native regression and repeated real-browser restart/deferred-tab workflows
  pass, including the Nix package without development library paths.
- [ ] Resolve intermittent packaged shutdown assertion (`!ephy_shell`): one
  restored-session quit failed; subsequent repeated restarts, preferences, and
  cookie clearing quit normally. Reference tracing found obsolete WebKit filter
  downloads holding the app. Remove that pipeline and recheck the package;
  do not claim the assertion's root cause is proven from successful repeats.
- [x] Remove the unused WebKit JSON-filter manager, startup downloads, and
  filter-initialization navigation queue. Full uBlock still passes its real
  filtering checks. The rebuilt package passes repeated restore/deferred-tab
  workflows and normal quit, including a fixture that detects any legacy filter
  download. This removes a WebKit service; other WebKit dependencies remain.
  Actual packaged preferences and normal/private cookie clearing also pass.
  Cookie UI automation retries the clear action during the documented disabled
  period while CEF flushes the store; screenshots captured this timing failure.
- [x] Replace WebKit permission-request objects with actual CEF prompt/media
  callbacks and reuse Epiphany's dialogs. Verify allow/deny, Ask Later, canonical
  requesting/top-level origins, opaque-origin denial, private scope, single-use
  decisions, navigation, iframe removal/navigation, tab closure, and late replies.
  Real Chromium media tests use diagnostic fake devices; physical webcam/mic
  behavior is not claimed. Eleven native checks pass. The actual app's dialogs
  allow/deny/dismiss correctly; clipboard denial leaves camera/microphone records
  unchanged, and private requests leave the regular permission file unchanged.
  Remove the unused WebKit permission-state query signal and handler as well.
  The Nix package passes actual permission dialogs, repeated session restoration,
  preferences, and normal quit without development library paths. Full uBlock,
  MV2/MV3, desktop clipboard/WebGPU, and six portal requests pass. The extension
  probe now waits for all result markers instead of closing before the delayed
  blocking check; the three-launch permission workflow has an explicit deadline.
- [ ] Investigate intermittent CEF integration timeouts. A parallel suite timed out
  in script/permission lifetimes and downloads; isolated checks and the same full
  suite run sequentially pass all eleven checks. Keep the native browser tests
  sequential for normal verification and preserve operation-specific diagnostics.
  A later serialized script check also timed out during a concurrent Nix build;
  three immediate isolated repeats passed. Script waits now identify the caller
  and pending expression instead of only reporting a generic assertion.
  A second suite during compilation also timed out in permission and session
  checks. After compilation stopped, all twelve checks and the full runtime/UI
  gates passed. New navigation regression coverage below fixes a separate
  lifecycle error; do not claim it proves the under-load timeout's root cause.
  The rich-clipboard regression suite also hit clipboard startup, session and
  download timeouts after compilation had finished (10/13 passed). Sanitizer
  documents are now created only on the first rich markup read, rather than
  every renderer context. The subsequent thirteen-check suite, actual Wayland,
  AMD WebGPU, full uBlock/MV2/MV3 and file portals pass; this does not establish
  the intermittent startup timeout's full cause.
  The underlying timing issue is not proven fixed. Builds still use all cores.
  The final uBlock gate initially failed dashboard initialization or actual
  blocking after filters reported ready. Source review and runtime traces found
  a fixture race: fresh uBlock reloads itself, disconnecting messages during the
  fixed wait; disconnected messages resolve undefined. The probe now waits for
  a real getLists response (after native network observers start), reads back the
  exact enabled filters, and avoids reloading an initialized dashboard. All four
  genuine uBlock checks and a fresh-profile repeat pass. The rebuilt Nix app
  passes regular/private icons, real clipboard permission dialogs and Find/Reader
  workflows without development library paths. The separate CEF startup timing
  issue above is still open; this fixes the observed uBlock fixture races.

## Verified foundation

- [x] Use Epiphany's actual GTK/libadwaita shell with CEF rendering pages.
- [x] Package CEF libraries so the built Nix app starts without development paths.
- [x] Keep ordinary browsing sandboxed; verify a packaged renderer has
  `Seccomp=2` and `NoNewPrivs=1`.
- [x] Verify native copy, web clipboard writes, and GTK paste into Chromium inputs.
- [x] Verify WebGPU on the desktop AMD GPU with an actual compute operation.
- [x] Verify hardware WebGPU separately on the actual Wayland backend. The
  `--gpu-only` capability test computes [2,4,6,8] on the actual AMD RDNA 2 adapter
  and cleanly shuts down Chromium. The Vulkan presentation warning does not
  prevent this compute operation; clipboard focus checks remain unchanged.
- [x] Run full uBlock Origin MV2: network filtering, cosmetic filtering,
  scriptlets, and iframe filtering.
- [x] Pass MV2/MV3 extension fixtures, including host permission checks.
- [x] Verify native MV3 USER-origin CSS overrides inline author `!important` and
  native removal restores the author style. Real MV3 runtime, blocking and host
  isolation gates pass. Log: `build-cef/verification/rich-clipboard/png-final/mv3-user-origin.log`.
- [x] Use native CEF request contexts for cookies and isolated private sessions;
  verify cookie persistence and private-session cleanup.
- [x] Route downloads through CEF into Epiphany's existing download UI;
  verify redirects, concurrent filenames, cancellation, private cookies,
  tab closure, and normal application shutdown.
- [x] Verify file and multiple-file selection through real GTK portals into
  actual Chromium file inputs.
- [x] Verify a UTF-8 save-dialog path, user cancellation, and pending file-chooser
  cancellation when hiding a page or closing its window through real portals.
- [x] Prevent large Chromium memory core dumps while preserving process
  identification needed by desktop portals.

## Finish the engine swap

- [ ] Exclude Cast, Wear, Vision, Wallet, and Play Billing integrations from
  Gnosis and its custom CEF build, as requested. Distinguish actual compiled
  components from runtime feature switches; the current SDK is prebuilt.
  Disable Chromium's actual MediaRouter feature while the source-build
  exclusions are prepared; this does not remove it from the prebuilt library.
- [ ] Fix folder uploads: Chromium currently crashes in its upload confirmation
  dialog with the GTK/offscreen embedding. Avoid repeating the known crash
  without a relevant fix.
- [ ] Fix File System Access directory selection separately from folder uploads.
  A real `showDirectoryPicker()` gesture reaches `Browser::OnFileDialog` as
  `FILE_DIALOG_OPEN` (0), confirmed under GDB; the resulting GTK chooser lists
  files. CEF's `SelectFileToFileChooserParams` omits the non-upload folder modes.
  Correct this in CEF rather than guessing from dialog titles or replacing
  the web API with a JavaScript mock. The source patch in
  `engine/patches/cef-directory-picker.patch` dry-runs against pinned commit
  `c1a097e`; it is not applied to the prebuilt SDK, so the runtime bug remains.
- [x] Verify actual saved-document contents through Chromium's writable file
  handles and the GTK SaveFile portal. A real user gesture creates and closes
  a writable handle; Python independently verifies all nine binary bytes and the
  Unicode filename. All six supported chooser cases and their D-Bus requests
  pass, including upload, cancellation, hiding and closing the pending window.
  Log: `build-cef/verification/rich-clipboard/png-final/portal-save-bytes.log`.
- [x] Verify real GTK printing portals with the existing CEF SDK: save a PDF
  to a Unicode filename and independently extract the actual page text; cancel,
  hide the tab and close its window while setup is pending. Cancel portal work
  when the page unmaps. D-Bus verifies four PreparePrint requests, one Print and
  two Close calls, with no surviving dialog. Log:
  `/tmp/gnosis-print-portals-fixed.log`.
- [x] Fix the application-side CEF child entry boundary so Chromium's fork-time
  stack-canary reset does not unwind older protected embedder frames. Keep
  stack protection in ordinary application code and leave the SDK and sandbox
  unchanged. The real print compositor now produces the PDF and exits cleanly.
- [ ] Finish portal coverage for OpenURI and screen sharing/PipeWire. The GTK
  print portal's Escape path returns a generic failure rather than dismissal;
  explicit Cancel passes. Folder uploads remain tracked separately above.
- [x] Make the migrated browser preferences control Chromium's actual settings
  and behavior; replace the feature panel's WebKit catalog with Chromium switches.
- [ ] Make clearing site data and cache operate on Chromium's stores, including
  private contexts and asynchronous error reporting. The pinned SDK needs a native
  storage-enumeration adapter: quota buckets omit local-storage-only sites and
  cannot identify every category/partition by themselves. Chromium Settings can
  enumerate regular data but crashes in isolated private profiles because its
  signin client is absent. Quota Internals works with a hidden Chrome-runtime
  view in both contexts; a windowless Alloy probe returns ERR_ABORTED. Do not
  infer the site list from history, inspect live database files, or mark private
  clearing complete using the regular context.
- [ ] Replace remaining WebKit storage/ITP services; ensure privacy reporting
  describes protection actually provided by this browser.
- [ ] Finish native permissions, authentication, TLS errors/exceptions, and
  navigation/dialog lifecycle behavior.
  User desktop checks: clipboard read/write works; `showDirectoryPicker()` opens
  a file chooser, WebAuthn still shows Chromium UI, and Bluetooth/location requests
  appear to do nothing. Trace these individually; camera remains unverified
  because the user has no webcam.
- [x] Connect CEF certificate failures to Epiphany's existing TLS error page and
  scoped exception flow. Native checks now verify the real issuer chain, status
  mapping, host/certificate/session isolation, private subresources and downloads,
  navigation cleanup, and a single completion for replacement error documents.
  Actual regular/private GUI checks pass Enter activation, exact acceptance,
  replacement and other-host rejection, reload, and clean quit. Resource MIME
  inference now uses Chromium's built-in types; actual regular/private captures
  show the correct error icon, and SVG/CSS checks pass with no GLib MIME database.
  Cached Back navigation and explicit about:blank keep their normal load events;
  replacement error documents emit exactly one committed/finished pair. Strict
  certificate enforcement remains tracked separately below.
- [x] Fix Enter and Space activation in actual Chromium web controls. Forward
  literal Space through the native CHAR event with its real keycode/modifiers,
  while retaining the IM path for composed text. Real GTK/X11 checks verify
  button activation on key release, checkboxes, one literal/Shift Space insertion,
  native Ctrl/Alt/Meta behavior, non-BMP Unicode composition and composition
  completed with Space, plus Enter activation. The application build passes.
  The fixture uses GTK's simple IM; external asynchronous IBus/Fcitx behavior
  and Wayland-specific keyboard coverage remain unverified. Log:
  `/tmp/gnosis-keyboard-runtime-4.log`. The independent browser-info startup
  timeout still appears and is not fixed by this input change.
- [ ] Preserve Chromium's strict certificate enforcement (including HSTS) in the
  native exception UI. Pinned CEF `c1a097e` receives `strict_enforcement` in
  `certificate_query::AllowCertificateError` but neither enforces it nor exposes
  it to `OnCertificateError`; the SDK needs to retain that native policy before
  the TLS migration can be considered complete. The SDK source patch now uses
  Chromium's profile/origin policy and SSL helper, clamps native continuation,
  and exposes immutable request metadata. Exact-pin and generated binding checks
  pass; SDK compilation, runtime strict-policy checks and UI consumption remain.
- [x] Synchronize notifications, location, microphone, and camera choices with
  Chromium content settings. Import existing policy-file choices once before
  navigation; persist a migration marker alongside native content settings.
  Native checks cover JavaScript query results, allow/deny, repeated requests,
  revocation, malformed origins, and separate private contexts. The actual GTK
  app imports a notification denial before its first query, changes Allow/Deny/Ask
  through GNOME's website controls, preserves those choices across restart, and
  does not resurrect revoked legacy records. Private choices disappear on close.
  Clipboard denial prompts again on the next request and does not alter media
  settings. The Nix package passes the same permission workflow, repeated session
  restoration, preferences, and normal quit without development library paths.
  All eleven native checks, actual uBlock/MV2/MV3, desktop clipboard/WebGPU,
  and the six supported file-portal requests pass.
  Existing password/ad/autoplay policies keep their original backend.
- [ ] Finish remaining permission-service integration and persistence rules,
  including storage-access choices and the legacy page-script permission export.
- [x] Return native JSON results from Chromium evaluation instead of creating a
  JavaScriptCore context per result. Port all evaluation callers, including Find,
  reader metadata, OpenSearch arrays, web-app metadata, and legacy extension
  replies. Native checks verify nested data, Unicode, distinct null/undefined,
  BigInt/non-finite numbers, script/serialization errors, isolated worlds, and
  cancellation/navigation/closure. All eleven native regressions pass.
- [x] Fix Reader Mode's pending-navigation deadlock: extract the current DOM
  before navigating, consume prepared HTML once for its matching reader URI,
  and register scheme requests before a synchronous completion can free them.
  Real GTK checks verify Find selection through native clipboard, live DOM edits
  in Reader content, direct reader URLs, normal/private cookie contexts, and quit
  during a stalled source request. Cold reader URLs release their hidden source
  view after parsing. Remove the reader source view's unused WebKit context.
  The Reader keyboard shortcut and menu both pass. Actual uBlock/MV2/MV3 and
  desktop copy/write/paste plus AMD WebGPU compute pass after the result migration.
  The desktop probe explicitly presents its window and waits for active focus
  before exercising the unchanged clipboard security gate. All six supported
  file-portal requests pass with the ported result consumers. The final Nix
  package passes normal/private Find/Reader workflows, pending-reader quit,
  repeated session restoration, preferences, and permission persistence without
  development library paths.
- [x] Replace internal top-level page messaging with actual CEF renderer hooks
  and process messages carrying JSON, without a WebKit JavaScript API shim.
  Port Reload, certificate-action callbacks, and web-app controls; remove JSC
  use from EphyWebView. Restrict delivery to registered handlers on this view's
  own internal documents; reject websites, subframes, forged alternate-page
  markers, oversized messages, and wrong worlds. Native checks cover parser-time
  delivery, JSON/Unicode, live registration changes, history, private contexts,
  normal close, and a callback closing its own window. Chromium network errors now reach the actual Epiphany error
  template. Its real Reload action recovers a failed HTTP request; the actual
  web-app Delete control opens GNOME's confirmation, and cancellation preserves
  the disposable app fixture. Certificate-error service handling remains below.
- [x] Infer internal resource MIME types when a scheme callback leaves them
  unspecified. Real Chromium checks render an alternate-page SVG and apply an
  internal CSS stylesheet; the application error icon and page styles now render.
  Replace the WebKit-only system-font name in imported templates with system-ui.
- [x] Cancel outstanding native evaluations directly from Chromium document and
  context destruction, independently of a late DevTools method reply. Match a
  context's unique identity before removing a reused numeric ID. Retain script
  promises in hidden tabs and during downloads; navigation and closure checks
  exercise the affected lifetime rules. Keep the intermittent timeout above
  open rather than attributing it to this lifetime gap without a specific trace.
- [x] Port local web-app icon loading from blocked file URLs to per-view native
  resource URLs. Actual regular/private application pages render exact fixture
  pixels; malformed, oversized, missing and nonregular icons use GNOME's fallback.
  Unknown/path/expired/cross-view icon requests fail and disposable apps survive.
  Both workflows close normally. The empty-list illustration branch remains
  source-reviewed only. Logs: `build-cef/verification/rich-clipboard/png-final/`.
- [x] Separate main-document starts from the aggregate loading indicator.
  Use actual main-navigation callbacks, including cached-history commit fallback;
  fragment changes and subframe loads preserve the main resource, native
  response, pending isolated-world promises, and document-event counts.
  Initialize the root frame from Chromium's real frame tree; refresh same-
  document history outside DevTools event dispatch to avoid reentrancy.
  Actual mouse-gesture fragment navigation and native Back pass, as do cached
  response restoration and the real Reload/Delete UI. Keep Chrome's normal
  suppression of non-user-activated script history entries intact.
  The final Nix package passes all twelve native checks, real full uBlock,
  MV2/MV3, desktop copy/write/paste and AMD WebGPU compute, and six actual file
  portal requests. Packaged regular/private Reload and web-app cancellation,
  Find/Reader (including stalled-source quit), repeated deferred-tab restoration,
  preferences Reset All, and the eight-launch permission workflow pass with
  development library paths unset. No compilation ran alongside this final suite;
  under-load stability remains unverified after the lifecycle changes.
- [ ] Port declarative user scripts and private-world page messages plus their
  remaining JavaScriptCore values. Preserve top/all-frame selection, timing,
  globals, CSP behavior, URL scopes, and isolated worlds. A native runtime probe
  showed root-only DevTools scripts miss cross-process frames and delayed main-
  world JavaScript eval fails strict CSP; do not use those shortcuts as the port.
  Exact Chromium 150 source research rules out native chrome.userScripts as a
  complete substitute: per-extension consent, restricted internal/data schemes,
  a 100-world limit and shared incognito world configuration leave manager gaps.
  Direct native Eval survives strict CSP and reaches actual cross-process frames.
  The reusable document-start regression caught a stock CEF timing gap: renderer
  OnLoadStart can see the committed URL while JavaScript document.URL still points
  to an old about:blank document. Blink updates that cached document accessor only
  afterward. Keep the assertion; use Chromium's actual script-start hook before
  activating scoped scripts. Context creation alone also misses reused contexts.
  The CMake user-content-start diagnostic retains that document.URL assertion;
  it stays outside the default suite until the native SDK hook replaces this path.
  Named worlds and live per-browser bootstrap updates still need native SDK
  support; the initial immutable CEF extra_info cannot update future renderers.
  The native script-start/end SDK source patch is prepared in engine/patches, with
  forward/reverse and generated ABI checks; its SDK build/runtime checks remain.
  Native extension USER-origin injection/removal now passes its real runtime
  check; per-manager stylesheets, internal schemes and early timing still need
  their complete native integration.
- [x] Bind clipboard replies to the actual requesting V8 context, including
  isolated worlds and replacement render views after reload. Verify concurrent
  overlapping request IDs, Unicode, rejected formats, custom Copy handlers in
  main/private worlds, Cut deleting once, and native background-focus denial.
- [x] Fix empty Wayland clipboard text reads: retain Blink's original native
  readText permission/policy checks, then read the actual GTK clipboard. Capture
  the original Promise.then before website scripts so they cannot invoke the
  native completion without permission. Real X11 and desktop Wayland checks
  verify exact main/private-world text, GTK-owned and empty clipboards, per-read
  permission prompts, denial without disclosure, and fresh bindings after reload.
  AMD WebGPU still computes [2,4,6,8]. All thirteen native checks, real full
  uBlock/MV2/MV3 and six file portal requests pass. The rebuilt Nix app passes
  the actual GTK Allow/Deny dialogs (exact text and a fresh prompt on each read)
  and regular/private Find/Reader checks with development library paths unset.
  Verification logs and the package path are saved under
  `build-cef/verification/clipboard/`; the rest of the CEF port remains unfinished.
- [x] Port the supported rich navigator.clipboard.read() formats to GTK,
  preserving Chromium permissions, HTML sanitization and binary image data.
  Native X11 and actual Wayland checks verify text, exact encoded PNG metadata
  and pixels, native GTK textures, safe HTML/SVG, Figma binary data, selective
  formats, explicit unsanitized HTML, iframe policy, and cancellation when the
  clipboard owner changes. Embedded image/CSS resources are not fetched.
  PNG bytes pass unchanged to the renderer for decoding; GTK encodes only local
  textures when no PNG bytes are offered. Invalid image data is rejected by the
  renderer decoder. Twelve other native checks, AMD WebGPU, MV2/MV3, exact portal
  saves and full uBlock in two fresh profiles pass. The rebuilt package passes
  regular/private icons, clipboard Allow/Deny and Find/Reader workflows with
  development library paths unset. Logs:
  `build-cef/verification/rich-clipboard/png-final/`. Package:
  `/nix/store/iwsw7dwhs0h4cczvq0fdplsz4qcrhddk-gnosis-0.1.0`.
  The initial broad run hit clipboard startup/reload and uBlock initialization
  failures; the targeted native/desktop runs and corrected uBlock probe pass.
  Keep the independent CEF startup issue above, generic web-custom-format
  interoperability and HTML style-inlining differences open before calling
  clipboard coverage complete.
- [ ] Implement Chromium's web-custom-format clipboard map for interoperability
  beyond Figma's legacy raw MIME. The pinned Chromium 150 backend publishes JSON
  under `application/web;type="custom/formatmap"`, mapping web MIME names to
  indexed `application/web;type="custom/format0"` payloads. Preserve the `web `
  namespace separately from native clipboard formats, bounded reads/writes,
  format filtering, and exact binary bytes in cross-application checks. Preserve
  the stricter native custom-format permission and final data-policy decision.
  Source review found the current permission-proof bridge discards native read
  results before rereading GTK, and native write promises can resolve before the
  browser-side policy callback. Neither promise authorizes original bytes after
  native denial/redaction/replacement. Add a GTK provider beneath ClipboardHost
  (or an exact approved-data transaction) before expanding formats; preserve
  native owner/sequence and source/destination identity. Runtime policy tests
  remain required alongside the existing functional clipboard checks.
- [ ] Port password/autofill integration and content script management.
- [ ] Replace remaining WebKit request/resource model APIs and report subresource
  lifecycle events. Main-response metadata is native now; source reads still use
  Chromium's document source and need original response-body support.
- [o] Connect extension management, enable/disable, settings, and menu actions
  to the native CEF extension runtime while preserving the existing GNOME UI.
  Extension actions belong exclusively in the extensions menu. Do not add
  per-extension buttons/icons to the top bar or provide toolbar pinning; keep
  each extension's action popup and settings reachable from the menu.
  The main window now exposes the configured CEF startup extension catalog in
  the GNOME models and opens real chrome-extension popup/options pages. The
  actual GTK window check confirms the embedded popup, a real dashboard settings
  pane, opening settings in its owning window, and normal quit. uBlock uses its
  supported portrait layout inside the constrained menu surface. Native catalog
  enable/disable is connected to a private management-only CEF helper and the
  existing GNOME list/detail switches. Native inventory is authoritative; pending
  controls are disabled, failures reconcile actual state, and native-disabled
  extensions retain their loader paths for restart. Install/delete remains
  separate work. Menu actions reconcile live state without toolbar embedding.
  The strict owning-site/cold-start native gate now passes after persistent MV2
  background readiness, alongside real two-window popup/settings and private/close
  rejection. Final packaged acceptance remains pending; see extension-popup-readiness.
  The real MV2/MV3 runtime and full uBlock checks pass; manager wiring is separate.
  Current page-level CEF DevTools sessions cannot call browser-only extension CRUD.
  Prefer request-context-scoped native SDK operations over a remote debugging port
  or hidden Chrome UI. A real management-only MV3 helper now verifies full MV2
  uBlock enumeration, live disable/re-enable, unchanged identity, persisted filters,
  ordinary HTTP API isolation and missing-ID rejection. Separate real HTTP tabs
  plus server logs prove blocking stops and resumes. Production manager wiring
  is now implemented. Actual GNOME list-disable/detail-re-enable stops and restarts
  uBlock's real native background page, removes/restores the menu action, and
  synchronizes both switches; normal quit passes. The first cold inventory query
  exceeded 10 seconds, so startup now has a 30-second bound and opening the manager
  retries unavailable state. The packaged app now passes actual list-disable,
  detail-re-enable, synchronized switches and normal quit with development
  library overrides removed.
  Evidence: `build-cef/verification/native-extension-controls/`.
  Native unpacked installation now stages an owned copy through the preferences
  folder chooser and loads it after restart (see the installation item below).
  Native uninstall remains outstanding. The public management-helper
  uninstall experiment was withdrawn: Chromium mandates its own confirmation
  when one extension removes another. Native Remove stays unavailable. The
  inherited GNOME Remove row callback signature/lifetime bug found during this
  work is fixed; it does not implement native uninstall.
  Stock-SDK lifetime fixes address native tab-bridge requests stranded by a
  destination tab closing, failed DevTools dispatch, or a detached target session.
  Preserve unrelated surviving-tab work and propagate actual errors to extension
  promises. Real MV2 background regression coverage verifies destination closure,
  surviving-tab work, single completion, and explicit transport errors; MV3 and
  full uBlock also pass. Recovery after losing the anchor/source session remains
  a separate gap.
- [x] Verify that genuinely native-disabled owned extensions can remain supplied
  at restart without activating first. Two clean browser launches used the same
  profile and exact loader paths. The first native inventory on restart reported
  full uBlock and a background sentinel disabled before any mutation; independent
  HTTP observations showed filtering off and no sentinel startup beacon until
  explicit native re-enable. Re-enable restored the sentinel and persisted uBlock
  filtering. This proves unchanged native identities, not legacy marker-only
  entries or migration across changed path-derived IDs. Production management
  wiring remains open. Evidence:
  `build-cef/verification/native-extensions/disabled-restart/REPORT.md`.
- [x] Verify native full-uBlock request blocking independently of popup counters.
  A disposable profile runs 64 real GET/HEAD fetches with 50 workers across two
  local origins. Server logs confirm all 32 allowed requests arrive and none of
  the 32 blocked/redirected requests arrive. Hard blocks reject; empty redirects
  fulfill locally. Log: `/tmp/gnosis-adblock-network-probes.log`.
- [x] Diagnose the public adblock test's 2% result with unchanged uBlock defaults,
  original fetch outcomes and native network events. Both the initial run and
  actual Re-test reproduce 2/132. Of 128 HEAD fetches in each run, 88 fulfill from
  local uBlock data replacements, 39 fulfill otherwise and one rejects. The site
  counts all fulfilled fetches as unblocked, including the local replacements;
  the score understates filtering. This is not proof of blocking every host.
  Evidence: `/tmp/gnosis-extension-check-0rv1nhj4/public-adblock-results.json`
  and `public-adblock-network.jsonl` in the same directory.
- [o] Recover native extension pages invalidated during initial extension reload,
  and verify the real uBlock settings pane and owning-tab popup in the main UI.
  One bounded reload on the exact native invalid-context error restores native
  getURL/getManifest and the actual settings pane; ordinary pages are excluded.
  The remaining first-install race is distinct: uBlock caches native tabs.query
  before the late CDP bridge, misses existing HTTP tabs when rebuilding its tab
  contexts, and can display an extension address or an uninitialized popup after
  reload. The regular `extension-popup-test` still fails; keep that regression
  visible. Do not force-reload user pages or patch uBlock to hide it. Logs:
  `/tmp/gnosis-extension-popup-status.log` and main-window screenshots in
  `/tmp/gnosis-native-extensions-ui-1viaqsol/`.
  Nix build passes at
  `/nix/store/9kjlxnv5rp2w0vqadb56dlxqlyf58dkm-gnosis-0.1.0`, but the packaged
  main-window run with development library paths removed reproduces a blank
  popup on fresh startup. Packaged UI acceptance remains failing; evidence:
  `/tmp/gnosis-packaged-native-extensions-ui.log` and
  `/tmp/gnosis-native-extensions-ui-x4znh5yr/failure.png`.
  A separate `--owner-reload-diagnostic` control waits for real uBlock startup
  and reloads only disposable test pages: both popup hostnames then match their
  owners and window queries exclude popups. It is a causal control, not the
  normal acceptance gate or a production workaround. The separate native tab-ID
  failure found by that control is fixed below; the control now reaches real
  settings and completes normally. Evidence: `/tmp/gnosis-dashboard-native-id-fix.log`.
  Persistent evidence copies are in `build-cef/verification/native-extensions/`.
  Final engine-view, MV2 lifecycle, MV3 and full-uBlock regression checks pass.
  All six real file-portal operations also pass with the installed GTK schema
  supplied to the development test environment; the first broad run lacked that
  schema. Packaged popup acceptance and the regular cold-start gate remain
  failing independently of those checks.
  A stock-SDK V8 initializer experiment reached its bounded deadlines without
  execution snapshots, including a pure-V8 control. Registration success does
  not prove ordering or that stock hooks are impossible. The experimental code
  was removed; small evidence is in the `early-context/` subdirectory. Further
  CEF investigation remains deferred under the current constraint.
- [x] Preserve the native tab-ID reply across a dashboard's same-document hash
  change. Validate its real document/navigation/browser identity rather than exact
  URL text. The causal-control test now completes tabs.create with identical
  returned, cached and genuine chrome.tabs.getCurrent IDs, reaches real settings
  controls, and passes owning-window/private/closed-owner checks. The normal
  cold-start popup inventory race above remains open. Log:
  `/tmp/gnosis-dashboard-native-id-fix.log`.
- [ ] Connect webpage external links to the actual OpenURI portal and GNOME's
  existing consent prompt, with owning-window cancellation and private isolation.
  CEF's OnBeforeBrowse exposes the destination frame but omits the navigation
  initiator origin. Never use its top-level display context for persistent grants;
  retain exact-initiator completion as an SDK prerequisite. OnOpenURLFromTab can
  also substitute the main frame for an unresolved source, so it cannot reuse
  grants either. OnBeforePopup exposes the actual opener. Do not infer it from
  referrers.
  Native request handling and the application build pass. Real GTK/X11 input
  verifies exact URI/context metadata, no-gesture/iframe rejection, document and
  tab lifetimes, cancellation once, and independent private/normal requests.
  Launches now use libportal with the exact owning window and cancellation token;
  GNOME's existing dialog uses asynchronous cancellation. Private and unattributed
  requests neither read nor write persistent grants. The final native test passes
  after making OnOpenURLFromTab unattributed too:
  `/tmp/gnosis-external-uri-final-native.log`.
  Real GUI integration independently verifies seven exact handler arguments,
  owning-window OpenURI tokens, a successful response for each specific request,
  saved consent/reuse, private isolation and stale-navigation cancellation.
  The packaged suite remains incomplete. With the GTK URL fix, the fixture's
  actual pageURL is now checked exactly and scheduled stale-navigation cancellation
  passes. One run verified all seven launches, then failed in the X11 helper's
  library lookup; that helper lookup is corrected. The next run verified five
  launches and independent window focus, then normal quit timed out. No renderer
  timer-pause fix is justified by these observations. Pending-modal desktop-close
  acceptance is still unproven; native destruction checks pass. Latest package:
  `/nix/store/j5gxqp19rapx3b46kl30agi5wg5j580g-gnosis-0.1.0`.
  Log: `/tmp/gnosis-open-uri-patched-gtk.log`. Further broad reruns deferred per
  user request to prioritize feature implementation. Persistent evidence:
  `build-cef/verification/open-uri/`.
- [ ] Update the pinned CEF/Chromium SDK from Chromium 150 while preserving
  full MV2/uBlock support and rerunning compatibility checks.
- [x] Port overview pin/remove, live history/title/thumbnail updates and snapshot
  resources to CEF internal-page transport, preserving GNOME UI.
  Packaged real titles, live title updates, pin/remove/Undo, protected JPEG
  delivery and normal quit pass. Interrupted startup no longer freezes the next
  normal history visit. Saved snapshots now read the retained CEF frame directly,
  avoiding a stale GTK thumbnail node; the final package captures real heading /
  body text with correct colors and orientation. Cheap tab thumbnail caching stays
  unchanged. Package: `/nix/store/5r7v8mg0cmywkqkv6ccwz8m3ga29qmb9-gnosis-0.1.0`.
  Evidence: `build-cef/verification/native-overview/`.
- [ ] Verify direct saved-frame snapshot readback on accelerated Wayland; the
  packaged software-rendering path is verified, GPU readback is not yet exercised.
- [x] Install unpacked MV2/MV3 extensions through the native preferences folder
  chooser into the owned profile store, with honest restart-required state.
  Actual GTK installs show disabled staged rows, then both native content scripts
  execute after restarting the same isolated profile. Cancellation/rollback,
  source independence, symlink/traversal and duplicate-ID rejection pass; full
  uBlock still passes. The Nix package opens/cancels the chooser and quits normally.
  Evidence: `build-cef/verification/native-extension-install/`.
- [x] Preserve CEF native file-filter descriptions and expanded extensions in
  actual GTK file portals, including case-insensitive filename suffixes.
  Real native selection returns the uppercase fixture and exact bytes; seven
  FileChooser calls, actual D-Bus filters, save, cancellation and close pass.
  Directory mode loss remains a separate pinned-SDK gap.
  Evidence: `build-cef/verification/native-file-filters/`.
- [x] Remove inert WebKitWebContext storage/property from CEF views and callers;
  retain native session isolation. Native message/clipboard/uBlock checks and
  actual packaged browsing/overview/preferences pass. Shell password/automation
  context work remains separate.
- [ ] Remove WebKitGTK/JavaScriptCore dependencies entirely once their remaining
  callers are migrated.
- [x] Replace inert WebKitWindowProperties and popup creation with actual CEF
  window.open metadata, browser-ready lifecycle and content-bounds callbacks.
  Preserve Epiphany window/tab policy and normal/private opener isolation.
  Native regular/private opener, retained child DOM, resize, close and storage
  isolation pass. The packaged shell passes both window policies and normal quit.
  Actual page viewports stay420x300 and640x360 across responsive toolbar animation;
  sizing includes GTK CSS/chrome and stops enforcing after a bounded settle period.
  Absolute x/y placement remains compositor-controlled, as with the GTK shell.
  Package: `/nix/store/k9hpxf40gcl5vzcmsw0bhhbz17vl1nmx-gnosis-0.1.0`.
  Evidence: `build-cef/verification/native-popup/`.
- [x] Remove stale WebKit security-manager setup and the ignored WebKit context
  argument from native URI-handler registration. Native CEF scheme flags remain
  authoritative. Actual native internal schemes, resources/MIME, history restore,
  private/tab message isolation and close lifetime pass; the packaged shell quits
  normally. This does not remove the remaining WebKit contexts/services.
  Evidence: `build-cef/verification/native-scheme-registration/`.

- [ ] Verify actual native context-menu editing across normal/private pages,
  cross-origin frames, and readonly/disabled fields. Chromium already provides
  the live edit flags through its native menu model; the separate unpopulated
  WebKitEditorState and dormant WebKit menu path have been removed above.
- [ ] Run a final packaged browser smoke check covering the migrated services;
  port affected upstream tests rather than treating compilation as completion.

## Requested Gnosis features

- [ ] After feature work, profile Gnosis against Brave on the same machine.
  User Speedometer 3.1 baselines: Gnosis 25.7 ± 1.1, Firefox 18.5 ± 2.9,
  Brave 33.5 ± 2.2. Compare engine versions, release compiler/runtime settings,
  workload/background state and GTK/CEF scheduling/render overhead before
  assigning causes. User explicitly wants this after features.

- [o] Add native WebXR/VR support and verify it with the installed SteamVR/OpenXR
  runtime. Inspect stock CEF/platform support first; preserve the no-SDK-rebuild
  constraint until the exact engine requirements are established.

These are not implemented in the Epiphany-based fork yet. Shell work can start
before every service migration is finished, with the engine work still tracked
above.

- [ ] Add tab groups. The Select-style custom-widget experiment is preserved on
  `archive/select-custom-tabs` for possible future work.
- [o] Port Select Ctrl+Tab with a thumbnail grid showing only tab names as text.
  Developer and packaged GUI verify grid, strip/MRU order, reverse, Escape and
  Control-release commit. Remaining lifetime/pointer/private cases still pending.
- [o] Offer tab order versus most recently viewed order for Ctrl+Tab, including
  Home where applicable.
- [ ] Check tab insertion animation and overflow against vanilla libadwaita;
  keep single-tab visibility and group navigation consistent with the requested UI.
- [x] Package the supplied Gnosis SVG and matching symbolic artwork.
- [o] Add built-in KeePassXC integration for saving/filling passwords, TOTP and
  passkeys, using KeePassXC's supported browser integration and database authorization.
  Official 1.10.4.1 runs as an invisible internal backend with a stable identity.
  Native libadwaita controls live in the hamburger menu; no toolbar extension UI.
  Real native proxy/worker and packaged GTK panel/settings/navigation tests pass.
  User-gesture unlock requests remain in KeePassXC. Pairing uses Connect Database.
  Native-style closed-shadow overlays and full uBlock coexistence checks pass.
  The user confirmed passkeys work on GitHub; Discord's issue is unconfirmed.
  Complete TOTP/save/fill coverage still needs live connected-database checks.
  Native Redetect Login Fields and late-loaded content-script recovery pass
  packaged tests, including staged password forms. Disable Brave's competing
  password-saving and automatic-sign-in prefs without deleting saved vault data.
- [o] Replace Chromium WebAuthn presentation with native GTK model-driven sheets.
  Local real-request QR/cancel and ownership checks pass. Mechanism, hybrid/USB,
  progress and basic errors are covered. GTK account selection returns a real,
  cryptographically verified assertion; account/phone-QR cancellation and
  switching to native phone discovery pass. Dialog height follows each sheet,
  with actions outside the scroller. Native PIN/GPM fallbacks and
  conditional passkey suggestions remain follow-up work.
- [ ] Build a proper native Gnosis MCP server: control real tabs and groups,
  navigate and interact with pages, inspect the DOM, capture screenshots, and
  expose console/network tools through Gnosis's existing CEF sessions. Preserve
  profile and private-context boundaries.

## Completion rule

Keep this file updated at each meaningful milestone. Do not mark the engine
replacement complete while WebKit/JSC remain required, or call extension/portal
support complete based only on the passing subsets above.

## Next-session handoff — 2026-10-26

The user requested only low-hanging fixes tonight, then PC shutdown. Resume the
larger work in the next session; no background task/automation was created.

Completed tonight:
- Restore the existing GTK edited-form warning without the unavailable WebKit
  `Ephy.hasModifiedForms` helper. Query input/textarea defaults in the isolated
  world; keep the short single-search-input heuristic, include changed checkboxes,
  ignore readonly/disabled fields, and recognize reset/pristine forms.
- Guard asynchronous tab-close callbacks after window/page destruction, clear
  checking state on Cancel, and clear the previous-tab weak pointer correctly.
- Add Open Developer Tools to native right-click menus, using the actual existing
  GTK-hosted Brave inspector rather than claiming element selection support.

Validation:
- `nix run . -- --help` passes. Latest output:
  `/nix/store/zz9w3lkmnpjwx501n5m4zmfm4w1921ql-gnosis-0.1.0`.
- Real packaged GTK/Brave test passes nine form cases; actual context-menu action
  opens Developer Tools; Cancel retains the dirty last tab; Discard removes it.
- Full official uBlock native MV2/network/main-and-frame cosmetic/scriptlet and
  enable/disable regression passes. `git diff --check` passes.
- Sources, fixture, logs, old unverified native-find prototype and a complete
  HEAD-to-worktree binary patch are backed up under
  `.git/gnosis-handoff-2026-10-26/`. Existing unrelated edits are preserved.
  No commit/reset/stash was performed; main remains intentionally dirty.

Remaining, in priority order:
1. Native Ctrl+F is implemented and verified today; see the current milestone
   below. Keep the old prototype only as historical evidence.
2. Real beforeunload preflight and native GTK close recovery are implemented
   and verified today. Broaden live-editor and cross-process activity coverage.
3. GPU freeze/overview stalls and actual mixed-monitor frame pacing.
4. Remove WebKit/JSC/CEF dependencies and obsolete code. CEF stays linked for
   live container/JSON APIs, but Nix now references its immutable SDK instead of
   copying it per build. The old WebKit network session is created lazily only
   for remaining legacy consumers; regular/private behavior is verified.
5. Continue remaining GTK auth/conditional-passkey/tab-sharing UI, context-menu
   media/iframe coverage, bundled uBlock refresh and broader live validation.

Keep the normal compatibility profile/application ID and KeePassXC pairing.
Work solo. Do not touch `.opencode/` or the existing untracked extension metadata.
Use `nix run`, never a separate `nix build`. The custom tabs stay archived.

## Verified implementation milestone — 2026-10-26

Eight agents prepared independent parts before the quota reset was observed.
All agents were stopped; integration, fixes and final validation continued solo.

- Native Ctrl+F uses the pinned browser's real FindTabHelper. Native frame/query/
  target lifetime tests and actual GTK count/ordinal/options/reopen tests pass.
  F3/Shift+F3 and Ctrl+G stay in GTK instead of also reaching Brave's find UI.
  Physical XTest next/previous checks pass. Optional Vim navigation is exposed
  in General → Browsing and defaults off; native switch binding, scroll/counts,
  DOM-control selection, nested field normal/insert modes, native colon commands,
  Unicode-safe motions/deletion, field undo/redo, closed-shadow inputs and live
  enable/disable are covered by the keyboard regression. Vim search retains the
  actual renderer match and adopts its control, including OOPIFs; Enter confirms
  search and returns to the page. Exact gg/G bounds in documents and nested scroll
  areas pass. Selection uses one accent outline without the page instruction badge.
- Native close preflight runs Chromium's real handlers and preserves all tabs
  until the complete close transaction is approved. GTK last-tab, two-tab window
  and two-window application-wide Keep Open/Leave,
  explicit failure recovery and the previous edited-form/DevTools checks pass.
- Removed the default unlimited frame-rate flag and enabled Wayland's external
  frame source. Real steady-state checks report 59.79 fps at 60 Hz and 119.55 fps
  at 120 Hz, versus the earlier 1698.37 fps uncapped sample. Two actual shell
  windows sharing one browser process also sustain 59.64/119.76 fps on separate
  60/120 Hz monitors. The separate GPU-process SIGTRAP remains unconfirmed.
- Nix links the compatibility CEF runtime to the immutable SDK. Loader, file modes,
  sandbox helper, locales and ICU checks pass. Regular/private GTK probes verify
  the WebKit network session is lazy and retains ITP/credential policy. CEF,
  WebKit and JSC are still dependencies; removing them requires real API migration.
- Bundled full uBlock upgrades keep their stable profile path and identity,
  atomically replace files, retain storage, restore disabled state and support
  rollback/recovery. Downgrades and unsafe paths are refused. Filesystem tests,
  real same-path native reload/storage checks, full native filtering and real
  GTK GSettings off/on filtering checks pass. External uBlock remains authoritative.
- Actual context-menu media operations use the trusted captured element; stale
  documents/elements are rejected. Unit and real Brave audio tests pass. Native
  Inspect Element and downloads remain incomplete. Child and nested out-of-process
  frame menus now route to their actual renderer sessions. Real GTK image copying,
  audio actions, custom CSS, navigation/stale-action rejection and child trusted
  activity invalidating native close approval pass; top-level title/URI stay intact.
- Private Brave data is removed only after browser and extension filesystem
  writers exit. Symlinks are never followed and replaced roots are refused.
  The actual packaged incognito shutdown and ownership tests pass. Abrupt host
  crashes can still leave a temporary directory; no broad cleanup scan is used.
- Repeatable native and GTK probes are retained under engine/tests and
  engine/tools; see engine/tests/NATIVE-GTK-TESTS.md.

Missed compositor allocations are reconciled on the next active frame. A fault
injection that suppressed allocation callbacks fails the previous package and
passes the new package; ordinary resize/maximize/restore also pass. The user's
original half-width screenshot was not independently reproduced.

Still remaining: GTK PIN/GPM and conditional passkey presentation,
GTK browser-tab sharing, private
extensions/KeePassXC integration, full dependency removal, broader overview
validation, and the unconfirmed severe GPU stall. Existing native fallbacks and
private-context restrictions remain where these adapters are incomplete.
