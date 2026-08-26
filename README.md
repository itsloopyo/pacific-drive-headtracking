# Pacific Drive Head Tracking

![Pacific Drive running with this mod](https://raw.githubusercontent.com/itsloopyo/pacific-drive-headtracking/main/assets/readme-clip.gif)

Move your head to look around while your mouse or controller keeps driving, aiming and interacting, using an ordinary webcam, phone or any OpenTrack compatible tracking source, no VR headset required.

## Features

- **Decoupled look and aim** - head tracking moves the camera; aim, interaction and driving stay on your mouse or controller
- **6DOF positional tracking** - lean and peek with head position, not just yaw, pitch and roll

## Requirements

- [Pacific Drive](https://store.steampowered.com/app/1458140/) on Steam.
- A head tracking source that speaks the OpenTrack UDP protocol: [OpenTrack](https://github.com/opentrack/opentrack) with a webcam, a VR headset, or a phone tracking app.
- Windows 10 or 11, 64-bit.

## Installation

1. Download `PacificDriveHeadTracking-v<version>-installer.zip` from the [Releases](../../releases) page.
2. Extract it anywhere.
3. Double-click `install.cmd`.
4. Configure OpenTrack (or your phone app) to output UDP to `127.0.0.1`, port `4242`.
5. Launch the game.

If the installer cannot find your copy of Pacific Drive, point it at the game
yourself, either with an environment variable:

```powershell
$env:PACIFIC_DRIVE_PATH = "D:\Games\Pacific Drive"
.\install.cmd
```

or by passing the path as an argument:

```powershell
.\install.cmd "D:\Games\Pacific Drive"
```

### Manual Installation

The installer places the Ultimate ASI Loader and the mod plugin next to
`PenDriverPro-Win64-Shipping.exe`. To do the same by hand:

1. Open `<Game>\PenDriverPro\Binaries\Win64\`, the folder holding
   `PenDriverPro-Win64-Shipping.exe`.
2. Copy `dinput8.dll` from the ZIP's `vendor\ultimate-asi-loader\` folder into
   that directory and rename it to `winmm.dll`. Skip this step if you already
   run an ASI loader for Pacific Drive.
3. Copy `PacificDriveHeadTracking.asi` and `PacificDriveHeadTracking.ini` into
   the same directory.

The Nexus ZIP (`PacificDriveHeadTracking-v<version>-nexus.zip`) carries only the
`.asi` and the `.ini`, for mod managers and for anyone who already has an ASI
loader installed.

## Setting Up OpenTrack

In OpenTrack, set **Output** to `UDP over network`, then open its options and
set the destination to host `127.0.0.1`, port `4242`. Pick your tracker under
**Input**, start tracking, and centre it with OpenTrack's Center bind while you
are looking straight at the screen.

### VR Headset Setup

1. Connect the headset to the PC with Air Link, Virtual Desktop or a link
   cable.
2. Launch SteamVR and let it see the headset.
3. In OpenTrack, set **Input** to `SteamVR`.
4. Set **Output** to `UDP over network`, destination `127.0.0.1` port `4242`.

### Webcam Setup

1. In OpenTrack, set **Input** to `neuralnet tracker`. It works off a plain
   webcam and needs no markers, clips or IR hardware.
2. Open its options and pick your camera and resolution.
3. Set **Output** to `UDP over network`, destination `127.0.0.1` port `4242`.
4. Start tracking, sit how you normally play, and press OpenTrack's Center bind.

### Phone App Setup

The mod takes the OpenTrack UDP protocol on port `4242` and nothing else, so a
phone app works here if it can send that protocol, either itself or through a
companion app on the PC. For one that can, what decides how you wire it up is
how much filtering it does before the packet leaves the phone.

- **App filters on-device:** point it straight at this PC's LAN IP address on
  UDP port `4242`. I made [Headcam](https://headcam.app) so decent tracking was
  free for anybody with a phone already in their pocket; it filters on-device,
  so it can send direct. Any app that filters enough noise works exactly the
  same way.
- **Raw or lightly filtered feed:** send it to OpenTrack instead, as a UDP
  input, and let OpenTrack's filters and curves clean it up before it forwards
  to `127.0.0.1:4242`. Do the same if you want OpenTrack's curve mapping
  regardless of the app.

The test is quicker than the reading: try direct, hold your head still, and if
the view drifts or shakes, route that app through OpenTrack. The mod's own
smoothing is sized to take the edge off a clean signal, not to rescue a noisy
one.

Centre the phone tracker with the app's own control, such as Headcam's CENTER
button.

A phone on WiFi is a remote connection and gets `RemoteSmoothing`. So does a
tracker running on this same PC if it sends to your LAN address instead of
`127.0.0.1`, because the mod classifies a transport and not a machine. Send to
`127.0.0.1` if you want `LocalSmoothing`.

## Controls

Two equivalent binding sets. Use whichever your keyboard has.

| Action | Nav-cluster | Chord |
|---|---|---|
| Toggle tracking | `End` | `Ctrl+Shift+Y` |
| Cycle tracking mode | `Page Up` | `Ctrl+Shift+G` |
| Toggle yaw mode | `Page Down` | `Ctrl+Shift+H` |

Cycling the tracking mode steps through full head tracking, then rotation only,
then position only, then back to full.

Toggling the yaw mode switches between world-space yaw, the default, where head
yaw turns the view about the world up axis so the horizon stays level however
the game camera is pitched, and camera-local yaw, where head yaw turns the view
about the camera's own up axis. The switch lasts until you restart the game;
`WorldSpaceYaw` in the INI sets what it starts as.

Centring is done in your tracker: OpenTrack's Center bind, SteamVR's reset view,
or the CENTER button in a phone app.

## Configuration

The mod writes `PacificDriveHeadTracking.ini` next to
`PenDriverPro-Win64-Shipping.exe`, in `<Game>\PenDriverPro\Binaries\Win64\`.
Delete it to restore defaults.

```ini
[Network]
; OpenTrack UDP port. Range 1024 to 65535; anything else falls back to 4242 and
; says so in the log.
Port=4242

[Tracking]
; Yes/no keys accept 1/0, true/false, yes/no or on/off. Anything else is
; reported in the log and the default is used.
EnableOnStartup=1
; Rotation sensitivities, range 0.1 to 3.0.
YawSensitivity=1.0
PitchSensitivity=1.0
RollSensitivity=1.0
InvertYaw=0
InvertPitch=0
InvertRoll=0
; Yaw mode: 1 = horizon-locked yaw about world up (default), 0 = camera-local.
WorldSpaceYaw=1
; Smoothing applied when the tracker runs on this machine (loopback).
; 0 = no smoothing, 1 = heavy.
LocalSmoothing=0.0
; Smoothing applied when the tracker is a remote device on the network, such as
; a phone on WiFi.
RemoteSmoothing=0.15

[Position]
; Position uses the same LocalSmoothing / RemoteSmoothing values as rotation.
Enabled=1
; Position sensitivities, range 0.0 to 5.0.
SensitivityX=1.0
SensitivityY=1.0
SensitivityZ=1.0
; Lean limits in metres, range 0.01 to 0.5.
LimitX=0.30
; LimitY is the UPWARD limit, LimitYDown the downward one. Delete LimitYDown to
; mirror LimitY, which is the symmetric case; set it lower than LimitY if
; crouching wants a tighter range than standing.
LimitY=0.20
LimitYDown=0.20
; Leaning forward gets more room than leaning back, so you do not clip through
; the seat.
LimitZ=0.40
LimitZBack=0.10

[Camera]
; Degrees ADDED to the game's own field of view. Pacific Drive has no FOV
; setting of its own, so this is the only way to change it. It is an offset
; rather than a fixed number because the game already uses a wider FOV in the
; car than on foot and widens it further with speed, and pinning one value
; would flatten all of that. 0 renders exactly what the game asked for.
; Range -40 to 60; anything outside is clamped and logged. The crosshair,
; interaction highlights and objective markers are all corrected for it, and
; the view point the game uses for interaction traces and AI is untouched.
FovOffset=0.0

[Controls]
; Windows virtual-key codes in hex, NOT key names: End is 0x23, not "End".
; The Ctrl+Shift chords are always active in addition to these.
; Range 0x01 to 0xFE. HeadTracking.log names the key each binding resolved to
; at startup, so check that line after changing one.
KeyToggle=0x23
KeyCycleMode=0x21
KeyYawMode=0x22
```

## Troubleshooting

The mod writes `HeadTracking.log` next to the game exe, recording loader attach,
config parsing, UDP binding and hook status. It starts fresh every launch, and
the previous session is kept as `HeadTracking.prev.log`.

**Mod not loading**

- Confirm `winmm.dll` and `PacificDriveHeadTracking.asi` are in
  `<Game>\PenDriverPro\Binaries\Win64\`, alongside the exe, not in the game's
  root folder.
- If `HeadTracking.log` does not exist at all, the loader never attached. Run
  `install.cmd` again and read where it reports putting the files.

**No tracking response**

- Check that your tracker is actually sending: OpenTrack's output must be
  `UDP over network` to `127.0.0.1` port `4242`, and tracking must be started.
- If the log says `UDP receiver could not bind port 4242 immediately; retrying
  in background`, another app already holds the port, usually a game left
  running with its own head tracking mod. Close it and this mod takes the port
  over within about half a second, with no need to restart Pacific Drive.
- Make sure tracking is not toggled off. Press `End` or `Ctrl+Shift+Y`.

**Jittery or unstable tracking**

- Raise `RemoteSmoothing` for a phone or WiFi tracker, or `LocalSmoothing` for
  a tracker on this PC.
- If a phone app sends a raw feed, route it through OpenTrack and use
  OpenTrack's filters rather than leaning on the mod's smoothing.
- For a webcam, improve the lighting and keep your face fully in frame.

**Wrong rotation axis**

- A single axis moves the wrong way: set `InvertYaw`, `InvertPitch` or
  `InvertRoll` to `1` in the INI.
- Yaw feels wrong when you look steeply up or down: toggle between world-locked
  and camera-local yaw with `Page Down` or `Ctrl+Shift+H`.
- The whole view sits off to one side: centre it in your tracker, using
  OpenTrack's Center bind, SteamVR's reset view, or the phone app's CENTER
  button.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd`. This removes the mod DLLs. The Ultimate ASI Loader is only
removed if the installer put it there. Use `uninstall.cmd /force` to remove it
anyway.

## Building from Source

Requires Visual Studio 2022 (MSVC x64), CMake 3.20 or newer, and
[pixi](https://pixi.sh). MinHook is fetched at configure time.

```powershell
git clone --recurse-submodules https://github.com/itsloopyo/pacific-drive-headtracking.git
cd pacific-drive-headtracking
pixi run build      # build/Release/PacificDriveHeadTracking.asi
pixi run test       # unit tests
pixi run package    # release/*.zip
```

## Community & Support

- [Discord](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch of head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your phone into a head tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

## Credits

- Ironwood Studios and Kepler Interactive - Pacific Drive.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG - the mod loader.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu - function hooking.
- [OpenTrack](https://github.com/opentrack/opentrack) - the tracking protocol and tracker.
- [cameraunlock-core](https://github.com/itsloopyo/cameraunlock-core) - shared head tracking pipeline.

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Ironwood Studios
or Kepler Interactive. Use at your own risk.
