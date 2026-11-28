# Native shared-session integration

Run in the Gnosis development environment with Python 3 and aiohttp:

```sh
python3 engine/tests/shared-session-native-test.py \
  --browser /path/to/packaged/gnosis/bin/gnosis \
  --build /path/to/matching/meson-build \
  --server /path/to/gnosis-server/server.py \
  --rrweb /path/to/rrweb.umd.min.cjs \
  --xvfb /path/to/Xvfb
```

The compilation database supplies the native headers and compiler flags. The
preload includes the production Session implementation with renamed entry points;
the packaged GTK shell, Brave backend, UI, encryption and transport are real.
This avoids a production testing API. Use a matching source/package revision.

The driver owns two browser process groups, a private Xvfb display, separate
session buses, temporary HOME/XDG/profile directories, a local relay and a local
HTTP origin. Chromium's sandbox stays enabled. It sends no physical desktop input
and does not connect to a human browser, profile or remote service.

Checks cover:

- Host approval before content; real rrweb DOM replay and live mutations, with
  mirrored scripts remaining inert.
- An authenticated SVG loaded once by the host. The guest receives cached bytes;
  its independently seeded credential never reaches the origin. An uncached URL
  fails without an origin request.
- Host logical viewport and guest letterboxing; local guest zoom preserves host
  width, height and scroll position.
- View-only input rejection, granted trusted host clicks, and control revocation.
- Native resize prompt, unchanged dimensions before approval/after refusal, and
  applied approved dimensions.
- Closing and reopening the session dialog around participant/status updates.
- Actual Stop Viewing/Stop Sharing banner buttons, reconnecting a viewer, and
  termination of remote viewing.
- GTK frame-clock after-paint feedback for start, join, resize and stop below
  400 ms; a 20 ms main-loop heartbeat stays below 400 ms while work is pending.

The result marker is `SHARED_NATIVE_ALL_PASS`. Logs and the symbol-bearing probe
are retained under `--log-dir` (default `/tmp/gnosis-shared-native-results`).
The default run uses the relay path. Repeat with `--p2p` to require a real direct
data-channel connection as well; external STUN is disabled for that local test.
`shared-transport-test.py` separately exercises transport pressure and recovery.
