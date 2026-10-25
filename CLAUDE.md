# Handover: Eclipse for NVIDIA Shield TV (Android port)

Read this first. It is the knowledge from the long session that built the Android port.
The user speaks **German** (Europe/Vienna), GitHub user **Zerbith**. Answer in German, short and
practical. Windows PC with **CMD (not PowerShell)** and `adb`. Tell the user plainly what is untested.

## Goal
Upstream https://github.com/PreceptorOfMagic/Eclipse (GPL-3.0, C/CMake/SDL2/LVGL, Moonlight descendant)
ships no working Android co-op. This repo (`Zerbith/test-eclipse-shield`, **public since 2026-10-24** (was
private; made public so Actions minutes are free), branch `main`, push directly) is an Android port for an **NVIDIA Shield TV** (arm64, Android 11, 4K TV, wired Ethernet).
Main goal: **co-op stream** (two hosts, one TV) with the **hardware decoder** at 4K, both orientations
(horizontal = top/bottom, vertical = left/right). Software decoding was rejected by the user.

Hosts used for testing: `192.168.0.200` = AMD GPU (Umbra, "DESKTOP-5I48VME"), `192.168.0.122` = NVIDIA GPU
("DESKTOP-VTLDNFB").

Upstream also published its own Android APK (`eclipse-android.apk` in release 1.3.6, package
`io.github.preceptorofmagic.eclipse`). It was tested on this setup and its co-op FAILS (second PC gets the
GPU HEVC encoder request, no video, "Network error"). Our port is a different app: package `org.eclipse.stream`.

## How we work (loop)
1. I (Claude) change code, commit, push to `main`.
2. GitHub Actions builds (`.github/workflows/build-android.yml`). **Only this workflow is enabled**; the other
   nine were disabled on purpose to save Actions minutes (re-enable in GitHub > Actions if ever needed).
3. Every push to `main` and every manual run on `main` (`gh workflow run build-android.yml --repo
   Zerbith/test-eclipse-shield --ref main`) publishes the APK as the rolling release **`android-latest`** (asset
   `eclipse-android-<run number>.apk`; versionCode = run number). The artifact `eclipse-android-arm64` also exists.
4. The user updates on the Shield with the **in-app updater** (Settings > About > "Check for update...")
   or `adb install -r <apk>`. Same signing key => in-place update, pairing/settings are kept.
5. For bugs the user sends a log: `adb logcat -c`, reproduce, `adb logcat -d -s Eclipse EclipseCoop DEBUG AndroidRuntime > log.txt`.
6. Watch CI: `gh run list/view --repo Zerbith/test-eclipse-shield`. Annotations (not full logs) are readable:
   `gh api repos/Zerbith/test-eclipse-shield/check-runs/<job id>/annotations`. Full job logs return 403.
   Builds are free since the repo is public (standard runners). If "The job was not started because recent
   account payments have failed..." shows up again, it is the user's GitHub billing/limit, not a code problem.
   The user does not want to pay. Do not make the repo private again without asking (minutes would count again).

### Local setup (user's Windows PC, set up 2026-10-24)
- Clone: `D:\eclipse Repo`. Git: `C:\Program Files\Git\cmd\git.exe`, GitHub CLI: `C:\Program Files\GitHub CLI\gh.exe`
  (both via winget; may not be on PATH in already-open shells, so call them by full path).
- `gh` is logged in as `Zerbith`. Repo-local git identity: `Claude <noreply@anthropic.com>` (same as older commits).
- Interactive prompts (sign-in) do not work in Claude's background shell; run them in the user's Terminal panel.

The sandbox cannot build Android (SDK/NDK hosts blocked, no KVM), so CI is the only compiler. Nothing here can
be run against the real hosts/Shield, so everything user-visible is "untested" until the user reports.
`gh` GraphQL is blocked; use `gh api` (REST).

## Signing / updater
- One fixed key for every build: `android/app/eclipse-port.keystore` (test key, passwords in
  `android/app/build.gradle`; now publicly visible, the user accepted that). Do NOT change it, or users must
  uninstall (and lose pairing).
- `UpdateActivity.java` reads release `android-latest` via the GitHub API with a **read-only fine-grained token**
  (Contents: read, this repo only) that the user typed once (`adb shell input text <token>` works, the token
  field is focused when empty). Download follows the redirect WITHOUT the token. Install via PackageInstaller;
  the write stream must be closed before `commit()`.

## Architecture (co-op on Android)
- Original Eclipse co-op: two Moonlight sessions in two processes; the secondary forwards reassembled HEVC
  access units over a unix socket (`coop_ipc`) to the primary which splices both panes into ONE bitstream for
  ONE decoder; mixed AMD+NVIDIA pairs request Umbra's "GPU HEVC encoder". On the Shield the spliced composite
  showed garbage although the decoder reported no errors.
- **Our design: dual decoders, no splicing.** Primary AU -> decoder/Surface 0 through the normal video path;
  secondary AUs -> a second SS4S player/MediaCodec/Surface 1 fed by `src/app/stream/video/coop_dual_android.c`
  (waits for IDR, tracks frame numbers, requests IDR on gaps, 5 s stats with tag `CoopDual`).
  `EclipseActivity` places two SurfaceViews (`setCoopLayout`: 1 = top/bottom, 2 = left/right, 0 = single).
  Disable with `adb shell setprop debug.eclipse.coop_dual 0` (old composite path, kept for comparison only).
- **GPU HEVC encoder is forced OFF on Android dual mode** (`coop_session.c`, `coop_session_set`): each host
  streams its native HEVC. Asking for the GPU encoder made the second host send no video.
- The secondary runs in `CoopSecondaryService` (`android:process=":coop"`), which does NOT inherit the
  primary's environment. `android_coop.c` hands over `COOP_GPU_HEVC`, `COOP_STREAM_MODE`, `COOP_HDR_ALLOWED`,
  `COOP_ORIENTATION_SEL`, `COOP_PIN_*`, `COOP_LOCAL_AUDIO`, `ECLIPSE_HOST_MONITOR_OFF` explicitly.
- "Start with monitors off" is sent per host (`umbraMonitorOff=1`); works for both hosts since the env hand-over.
  Whether the virtual display becomes the main display is host-side (Umbra), not verifiable here.
- Logging: stdout/stderr go to logcat tag `Eclipse`; `diag_logs` redirection and the UI-stack dump are skipped
  on Android.

## Features added in this port
- **Controller virtual mouse per pad**: each controller's mouse goes to the host that controller is assigned to
  (`session_virt_mouse.c`, new coop messages `MOUSE_MOVE`/`MOUSE_SCROLL_HR`). Toggle LB + right-stick click
  (needs Settings > Input > Virtual mouse ON) or the radial menu.
- **Radial menu**: hold **Start 3 s** (`RADIAL_HOLD_MS` in `session_gamepad.c`); left stick picks Mouse (up) /
  Keyboard (right) / End stream (down, quits on both PCs) / Steam Big Picture open-close (left); release
  Start confirms, stick centred = cancel (the old "Cancel" entry was replaced on 2026-10-24).
- **Big Picture toggle** (`stream/input/host_actions.c`): Win+R, `steam://open|close/bigpicture` as UTF-8 text,
  Enter. Goes to the PC(s) the controller is assigned to; the peer gets coop message `HOST_ACTION` (14).
  Open/closed is only what we last sent, per host. UNTESTED until the user reports.
  Module `src/app/ui/streaming/radial_menu.c`. Short Start taps still reach the host.
- **Android input filter** (`input_gamepad.c`): devices named remote/virtual/search or with < 4 axes are not
  treated as controllers (Shield remote, "Virtual Search" cluttered the co-op assignment).
- **Solar-eclipse banner/icon**: `android/app/src/main/res/drawable/banner.png` (640x360, drawn by script).
  Launchers cache banners; a Shield reboot refreshes it.
- In-app updater (above).

## State at handover (what the user last confirmed)
- Dual-decoder co-op works with both orders (AMD first / NVIDIA first), horizontal and vertical, "pictures
  fine". Controller mouse, radial menu, updater, icon, controller filter all confirmed working.
- Commit `238b7cf` "Updater: clear focus highlight for the buttons and token field" is built and published
  (build 56, `eclipse-android-56.apk`, 2026-10-24); not yet confirmed on the Shield.
- CI warns that `actions/setup-java@v4` and the Node 20 actions are deprecated; still works, upgrade some time.
- First real co-op game test (2026-10-25, Borderlands 2): (1) short picture freezes every few seconds on the
  PRIMARY (NVIDIA) pane; (2) secondary (AMD) audio crackles; (3) dialogue heard twice, out of sync (two game
  instances; settings: Co-op > Audio mode / "Play shared sounds once" / Advanced "Second PC's sound buffer");
  (4) only 59 FPS selectable (SDL truncated the Shield's 59.94 Hz; fixed in basic.pane.c). For (1) the
  MediaCodec output thread now renders only the newest picture of a burst (`skippedStale` in the
  `EclipseMediaCodec` stats[N] log line every 300 frames, next to `droppedBusy`). Cause of (1) NOT confirmed;
  needs a log with tag `EclipseMediaCodec`.
- Build 58 fixed the freezes (user confirmed). Audio, from a captured log 2026-10-25: the `:coop` process gets
  Android's 40 ms background timer slack, so `SDL_Delay(1)` in the secondary's stop-and-wait sender slept ~41 ms
  -> only ~24k of 48k frames/s reached the mixer (crackle, `rejected_window`, FIFO full = 1-3 s delay).
  Sender now waits on poll()/cond + PR_SET_TIMERSLACK. Also every stream end crashed in SDL 2.30's AAudio driver
  (SDL_PumpEvents -> AudioStreamTrack::getTimestamp); app.c now asks SDL for OpenSL ES first. Both UNTESTED.
- Capturing logs myself: `D:\adb\adb.exe -s 192.168.0.100:5555` (Shield over network). The Shield's logcat
  buffer was 256 KB (too small, the user's log came out empty); set to 16 MB with `logcat -b all -G 16M`
  (may reset on reboot). Look for `[COOP-AUD.secondary]` / `[COOP-AUD.pull]` lines every 5 s.
- Not yet reported/verified: 60 FPS smoothness at 4K, audio quality, seam visibility, "monitors off" on the
  AMD host after the env fix, End-stream semantics ("only that PC" was asked once; currently quits both).

## Ideas not done
- Per-PC choice "Steam Big Picture or Desktop" at co-op start. The co-op host app is the fixed Umbra app
  "Co-op Pane"; what it shows is host-side. Would need a host parameter or another host app; unverified.
- HDR10/AV1, rumble, gyro, German translation, "End stream for only this controller's PC".
- Cleaning an old remote entry from the saved known-controllers list in co-op settings.

## Gotchas
- `session_t` is opaque in `src/app/ui/**`; use helpers in `stream/session.h` (`session_vmouse_active/flip`).
- Groovy: `versionCode (x).toInteger()` parses as a method call; use `versionCode = ...`.
- Editing a file in the coop env: check that `setenv` uses the overridden `g_session`, not the caller's copy.
- The repo is public: never put the user's GitHub token or any other secret into code, commits or the APK.
- Do not re-enable software decoding or the old composite path as a "fix"; both were rejected.
