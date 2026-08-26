# Changelog

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
