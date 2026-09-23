# Pacific Drive Head Tracking

![Pacific Drive running with this mod](https://raw.githubusercontent.com/itsloopyo/pacific-drive-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for Pacific Drive that moves the view with your head while your mouse or controller keeps steering, driven by OpenTrack over UDP, with no VR headset required.

## Features

- **Decoupled look and aim** - head tracking moves the camera; aim, interaction and driving stay on your mouse or controller
- **6DOF positional tracking** - lean and peek with head position, not just yaw, pitch and roll
- **Works with any OpenTrack compatible tracker** - free options available for PC, iOS and Android

## Requirements

- [Pacific Drive](https://store.steampowered.com/app/1458140/) on Steam, or on
  Xbox Game Pass.
- A head tracking source that speaks the OpenTrack UDP protocol: [OpenTrack](https://github.com/opentrack/opentrack) with a webcam, a VR headset, or a phone tracking app.
- Windows 10 or 11, 64-bit.

## Installation

### Lopari

Download [Lopari](https://lopari.app), choose **Pacific Drive**, and click
**Play with head tracking**.

### Standalone Installer

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

### If you own both copies

The Steam and Xbox Game Pass builds are different executables in different folders,
and the installer writes to one copy per run: whichever it finds first, which is
Steam when both are installed. To cover the other one, run `install.cmd` again
with its path, as above. It reports the folder it wrote to every time, so read
that line rather than assuming.

### Manual Installation

The installer places the Ultimate ASI Loader and the mod plugin next to the game
executable. That is a different folder on each store:

| Store | Folder | Executable |
|-------|--------|------------|
| Steam | `<Game>\PenDriverPro\Binaries\Win64\` | `PenDriverPro-Win64-Shipping.exe` |
| Xbox Game Pass | `<Game>\PenDriverPro\Binaries\WinGDK\` | `PenDriverPro-WinGDK-Shipping.exe` |

The Xbox app asks which drive to install to, so the Xbox Game Pass folder is under
`<that drive>\XboxGames\Pacific Drive\Content\`. Windows also publishes the
same directory under `C:\Program Files\WindowsApps\` as a junction; either
route reaches the same files. To do the install by hand:

1. Open the folder for your store from the table above, the one holding the
   executable.
2. Copy `dinput8.dll` from the ZIP's `vendor\ultimate-asi-loader\` folder into
   that directory and rename it to `winmm.dll`. Skip this step if you already
   run an ASI loader for Pacific Drive.
3. Copy `PacificDriveHeadTracking.asi` and `PacificDriveHeadTracking.ini` into
   the same directory.

The Nexus ZIP (`PacificDriveHeadTracking-v<version>-nexus.zip`) carries only the
`.asi` and the `.ini`, for mod managers and for anyone who already has an ASI
loader installed.

## Setting Up OpenTrack

The mod listens for OpenTrack pose data on UDP port `4242`, on every network
interface. One datagram is six little-endian 64-bit floats in the order
`x, y, z, yaw, pitch, roll`: position in centimetres, rotation in degrees, 48
bytes in total. Anything that sends that to that port drives the view.
OpenTrack's **UDP over network** output sends exactly this, and the steps below
set it up.

1. Install [OpenTrack](https://github.com/opentrack/opentrack/releases).
2. Pick a tracker under **Input**, using the notes below.
3. Set **Output** to **UDP over network**, host `127.0.0.1`, port `4242`.
4. Press **Start**. Tracking and the game can start in either order.

### Webcam

OpenTrack ships a `neuralnet tracker` input that reads a plain webcam. Select it
under **Input**, pick your camera in its settings, and use the output settings
above. How well it tracks depends on your camera and your lighting, so try it
before buying anything.

### Phone

A phone app can reach the mod directly, with no OpenTrack on the PC, if it sends
the datagram described above. Point it at this PC's IP address (run `ipconfig`
to find it) on port `4242`. Not every phone tracker speaks this protocol, so
check yours for an OpenTrack or UDP output option first. [Headcam](https://headcam.app)
sends it, and I wrote it so decent tracking is free for anyone who already owns
a phone.

Sending direct works when the app filters its own signal on the device. The
mod's smoothing is sized to take the edge off a clean signal rather than to
rescue a noisy one, so a raw feed sent direct will jitter. If it does, point the
app at OpenTrack's **UDP over network** *input* on some other port, say 5252,
and let OpenTrack's filters and curves clean it up before its output forwards to
`127.0.0.1:4242`.

Anything arriving from outside `127.0.0.0/8` counts as a remote connection and
is smoothed with `RemoteSmoothing` rather than `LocalSmoothing`. That includes a
tracker on this very PC that sends to the machine's own LAN address, because the
mod reads the source address and not the machine.

### Headset or other hardware

If your device has an OpenTrack input driver, select it under **Input** and use
the same output settings. OpenTrack's own **Input** list is the authority on
what it can read; the mod only ever sees what OpenTrack sends.

### Centring

Centring belongs to your tracker. The mod subtracts no centre of its own: it
applies the pose it receives exactly as it arrives, so a stream of zeros holds
the view where the game itself puts it. Press the centre control in your tracker
(OpenTrack's **Center** bind, or the CENTER button in Headcam) and the tracker
zeroes its own output, which leaves the view centred with the mod doing nothing.

That is why there is no centre hotkey here and nothing to re-centre in game. Two
centres in series would drift apart, because each side re-centres at moments the
other cannot see, and you would end up pressing twice to centre once. If the
view sits off to one side, centre it in the tracker.

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

The mod writes `PacificDriveHeadTracking.ini` next to the game executable, in
the folder for your store listed under [Manual
Installation](#manual-installation). Delete it to restore defaults.

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

- Confirm `winmm.dll` and `PacificDriveHeadTracking.asi` are alongside the game
  executable - `Binaries\Win64\` on Steam, `Binaries\WinGDK\` on Xbox Game Pass -
  and not in the game's root folder.
- If the log exists but says the build is unknown, the game has been patched to
  a build this release does not have a profile for. The mod stays dormant and
  the game runs vanilla; check the releases page for an updated mod.
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

- Discord: [Loop's Head Tracking Hangout](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports, and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install and launch for the released head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your iPhone or Android phone into the head tracker

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
