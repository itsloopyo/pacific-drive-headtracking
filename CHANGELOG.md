# Changelog

## [Unreleased]

### Added
- A setting set to `default` in `CameraUnlock.ini` takes its value from `Defaults.ini`, which every head tracking mod that keeps its settings in `CameraUnlock.ini` reads. Head tracking mods that keep their settings in another file do not read it, and neither do earlier versions of this mod. Writing a value in place of `default` changes that setting for this game only. When the mod saves a setting that a hotkey changed in game, it writes the new value in place of `default`, so that setting no longer follows `Defaults.ini` in this game until you set it to `default` again.
- `Defaults.ini` is `%AppData%\CameraUnlock\Defaults.ini` on Windows; `$XDG_CONFIG_HOME/CameraUnlock/Defaults.ini` on Linux, or `~/.config/CameraUnlock/Defaults.ini` where `XDG_CONFIG_HOME` is not set, under Wine and Proton too; and `~/Library/Application Support/CameraUnlock/Defaults.ini` on macOS. The mod's log, where it writes one, names the file it read.
- When the mod starts and finds no `Defaults.ini`, it creates one holding the built-in values, unless Windows runs the game as a packaged app. The mod never changes `Defaults.ini` after that.
- Added support for the Xbox Game Pass / Microsoft Store copy of the game. That
  build is a different executable in a different folder
  (`PenDriverPro\Binaries\WinGDK\PenDriverPro-WinGDK-Shipping.exe`) with its own
  addresses, so it gets its own build profile; the mod picks the right one by
  fingerprinting the executable it finds itself inside. Both copies can be
  installed at once and each gets its own deployment.

### Changed
- Settings move to `CameraUnlock.ini` next to the game executable (`PenDriverPro\Binaries\Win64\` on Steam, `PenDriverPro\Binaries\WinGDK\` on Xbox Game Pass). Earlier versions of the mod kept these settings in `PacificDriveHeadTracking.ini`, in the same folder. The first time this version starts and finds no `CameraUnlock.ini`, it reads your settings from `PacificDriveHeadTracking.ini` and writes them into `CameraUnlock.ini`. It never changes `PacificDriveHeadTracking.ini`, and does not read it again while `CameraUnlock.ini` exists.
- A setting that the defaults the README shows set to `default` is written as `default` when you never changed it from the default earlier versions used, because `PacificDriveHeadTracking.ini` does not hold it or holds that default. It then follows `Defaults.ini`, so it takes the value `Defaults.ini` gives it, or the built-in value where `Defaults.ini` gives none, which can differ from the default earlier versions used. A setting you changed is written with the value imported for it, or as `default` where that value equals its default at that start.
- `RotationEnabled` and `PositionEnabled` are one setting here, the tracking mode, so both are written as `default` or neither is.
- Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:
  - A sensitivity or axis inversion you changed from its default. Set these in your tracker instead.
  - A hotkey set to Ctrl, Shift or Alt on its own. That key goes down before the key of any chord made with it, so the hotkey is left unbound, and it keeps its Ctrl+Shift chord.
- An older version of the mod reads `PacificDriveHeadTracking.ini` and never reads `CameraUnlock.ini`, so a setting you change after updating is not in `PacificDriveHeadTracking.ini`.
- Deleting only `CameraUnlock.ini` makes the next start read `PacificDriveHeadTracking.ini` again. To go back to the defaults, replace everything in `CameraUnlock.ini` with the defaults the README shows. Every setting they set to `default` then follows `Defaults.ini`.
- Hotkeys are written as key names, and each hotkey lists every key that triggers it, the Ctrl+Shift chord included: `ToggleKey=End, Ctrl+Shift+Y`. The `[Controls]` virtual-key codes and the chords the mod always bound beside them become `ToggleKey`, `CycleTrackingModeKey` and `YawModeKey`.
- The tracking mode and the yaw mode are saved to `CameraUnlock.ini` the moment you change them in game, so the game starts in them next time. Toggling tracking with `End` still lasts for the session only.
- The installer, the launcher and the Nexus ZIP no longer ship a config file. Uninstalling keeps `CameraUnlock.ini` and `PacificDriveHeadTracking.ini`.
- The installer now places the mod next to whichever game executable it
  detected, rather than always at the Steam build's path. A Game Pass install
  previously received the files in a folder the game does not read, which looked
  like a successful install and did nothing.

### Removed
- The sensitivity and axis inversion settings. Set these in your tracker app instead.
- With these settings at their shipped defaults the camera moves as it did before.

## [0.0.0] - 2026-08-26

### Added
- Added decoupled 6DOF head tracking. The head moves the view in yaw, pitch, roll
  and positional lean on X, Y and Z, while the mouse or controller still drives,
  steers and aims.
- Added aim decoupling. The head pose is written only into the renderer's view
  transform, so the view point the game uses for interaction traces, AI vision
  and physics is never modified. What you can reach, shoot at or interact with is
  exactly what you could without the mod.
- Added crosshair compensation. Since aim is decoupled, the crosshair is drawn
  where the game's own aim direction lands on screen, and returns to centre when
  tracking is off.
- Added HUD compensation so interaction prompts and highlights stay on the object
  they belong to.
- Added a field of view setting, `[Camera] FovOffset`, in degrees added to the
  game's own FOV. Pacific Drive has no FOV setting of its own. It is an offset
  rather than a fixed number because the game already varies its FOV, wider in the
  car than on foot and wider again with speed, and pinning one value would flatten
  all of that. It applies to the rendered projection only, so interaction traces
  and AI are unchanged, and the crosshair, highlights and objective markers are
  reprojected through the new FOV. It works whether or not a tracker is connected.
- Added a log line reporting the field of view actually being rendered, measured
  from the projection matrix rather than read out of an engine field:
  `fov game=75.0 rendered=90.0 vertical=58.7 (offset +15.0)`. That is where you
  read off the number to set `FovOffset` to.
- Added gameplay-only tracking. The main menu, loading, the pause screen and
  cutscenes are left alone, and tracking eases in and out over about a tenth of a
  second instead of snapping. There is nothing to configure: the mod reads the
  game's own state.
- Added OpenTrack UDP input on port 4242, so any tracker speaking that protocol
  works, whether webcam, phone app or dedicated hardware.
- Added port retry twice a second for as long as the game runs, so launching with
  another head tracking game already holding port 4242 recovers without a
  restart. Every heartbeat reports whether the port is bound, still being waited
  on, or bound and silent.
- Added two smoothing settings, `[Tracking] LocalSmoothing` (default 0.0) and
  `RemoteSmoothing` (default 0.15), chosen per connection from the tracker's
  source address. A tracker on this machine gets no added latency by default; a
  phone or headset over the network gets enough smoothing to hide link jitter.
  Both cover rotation and position.
- Added hotkeys on the nav cluster with Ctrl+Shift chord alternatives: `End` or
  `Ctrl+Shift+Y` toggles tracking, `Page Up` or `Ctrl+Shift+G` toggles positional
  tracking.
- Added patch resilience. The UE4 reflection the mod needs is discovered at
  runtime and self-validates, and the offsets pinned per build live in an
  append-only registry keyed on the executable's fingerprint. On an unrecognised
  build the mod stays dormant and says so rather than hooking a binary it does not
  understand.
- Added `HeadTracking.log` next to the game exe. It is truncated on every launch,
  so it only ever covers the session you just played, and one previous generation
  is kept as `HeadTracking.prev.log` so a crash does not destroy the log of the
  session that crashed. Everything in it is either a one-off or written only when
  the state it reports changes, apart from a heartbeat every 30 seconds for the
  first few minutes and every five minutes after that.
- Added range checking on every value in `PacificDriveHeadTracking.ini`. A value
  that will not parse, or one outside its documented range, is corrected to the
  default or to the nearest limit and the correction is written to
  `HeadTracking.log` naming the key. Nothing downstream re-checks these numbers,
  so a mistyped one used to reach the renderer intact: `nan` or a number too
  large to represent produced a view transform full of NaN every frame with
  nothing in the log, and a `Port` line the parser could not read became port 0,
  which binds a random port and silently guarantees that no tracker packet ever
  arrives.
