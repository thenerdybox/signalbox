# SignalBox Category Monitor

*by TheNerdyBox*

A free OBS Studio plugin for Windows that keeps your Twitch category matching
the game you're actually playing — and **asks first** when it isn't sure.
It doesn't take the wheel, it taps you on the shoulder.

Free. Staying free.

> **Preview build.** SignalBox works and is in daily use, but it has not been
> through a wide public test yet. Expect rough edges, and please report them.
> Every release is listed on the
> [release notes page](https://thenerdybox.com/patch-notes/signalbox).

## What it does

- **Detects what you're running** from each store's own metadata — Steam,
  Epic, GOG, Ubisoft Connect, plus a Windows Uninstall-registry fallback.
  Never filename guessing.
- **Sets your Twitch category** when it's confident, and **asks** when it
  isn't. The prompts are panels in the dock: never a pop-up, never a modal,
  never anything that can tab you out of a fullscreen game. Ignoring one is
  always safe — it holds your current category.
- **Asks at the moments that actually matter**: going live in the wrong
  category, a game closing while you're still live, a creative app being open
  rather than being streamed, and sitting with no game running at all.
- **Corrects itself permanently.** Got a detection wrong? *Fix This!* lets you
  pick the right Twitch category for that exact app, or tell SignalBox never
  to switch for it. No config files.
- **Stops when you tell it to.** *Stream Ending/Stop Detection* holds your
  category while you wrap up, and clears itself when the stream stops or a new
  game starts — so you can't forget to turn it back on.
- **Chapters your VOD** with a Twitch stream marker on every category change.
- **Stays out of the way.** It polls four times less often while nothing is
  happening, does no background network polling at all, and never blocks OBS's
  UI thread.

## Installing

1. Download the latest `SignalBox-Setup-x.y.z.exe` from
   [Releases](https://github.com/TheNerdyBox/signalbox/releases).
2. **Close OBS**, then run the installer.
3. Open OBS → **View → Docks → SignalBox Category Monitor**.
4. Connect your Twitch account in the dock. You'll get a short code to enter
   on Twitch's own site — SignalBox never sees or stores your password.

Requires **OBS Studio 30 or newer**, 64-bit Windows.

The installer is not code-signed yet, so Windows SmartScreen will warn you.
"More info" → "Run anyway", or build it yourself from source below.

Upgrading: run the new installer over the old one and answer **Yes** when it
asks whether to keep your settings and Twitch connection.

## Your data

Your Twitch tokens are encrypted with Windows DPAPI and stored locally, tied
to your Windows account. They are never sent anywhere except Twitch, never
written to your OBS scene collection, and there is no SignalBox server to send
them to. Settings live in `%APPDATA%\obs-studio\plugin_config\signalbox\`.

## Building from source

Requires CMake 3.28+, a Visual Studio 2022 (or newer) C++ toolset, and the
Windows 10 SDK. No local Qt or OBS install needed — the build fetches prebuilt
OBS headers/libs and Qt6 on first configure, the same mechanism
`obsproject/obs-plugintemplate` uses. That first configure downloads a few
hundred MB and is slow once, fast after.

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Output lands in `build_x64/rundir/<config>/`. `build-installer.ps1` does
build, test, installer and staging in one step.

There is a standalone test harness that runs the detection state machine, the
category resolver and the switch coordinator with no OBS process, no Qt event
loop and no live Twitch connection:

```
build_x64\<config>\signalbox-harness.exe
```

## Project layout

```
src/
  core/         Entry point, config, timing constants, detection state machine
  detection/    Install index, process scanning, per-store providers
  twitch/       Device code auth, Helix client, encrypted token storage
  ui/           Dock, non-modal prompts, settings, the fix-a-detection dialog
data/
  helpers.json  Helper/launcher executables to ignore (community-patchable)
  aliases.json  Game-name to Twitch-category corrections (community-patchable)
tests/harness/  Standalone correctness harness, no OBS or Qt required
```

`src/core/TimingConstants.h` is the one place every detection and automation
timing lives — read its header comment before adding a delay anywhere else.

## Contributing

`data/helpers.json` and `data/aliases.json` are meant to be
community-patchable. If SignalBox mis-detects a launcher as a game, or picks
the wrong Twitch category for one, a pull request against either file is a
genuinely useful contribution on its own — no C++ required.

Bug reports are most useful with the relevant lines from your OBS log
(`Help → Log Files → Show Log Files`); SignalBox writes what it detected and
what it did on every change.

`DESIGN.md` covers the detection state machine, the prompt model and the
Twitch API decisions. `rate-limits.md` covers the research behind the timing
defaults.

## Clean-room statement

SignalBox was designed and written exclusively from public, documented
sources: the OBS Studio plugin API and official `obs-plugintemplate`,
Microsoft Win32 documentation, Valve's published Steam file formats, the Epic
Games Launcher on-disk manifest format, publicly documented GOG and Ubisoft
registry conventions, and the Twitch Helix and OAuth documentation. **No
third-party plugin source code was consulted at any point.** Sources are cited
inline throughout `DESIGN.md`.

## License

MIT — see `LICENSE`. `THIRD-PARTY-NOTICES.md` covers the build scaffold's
provenance and the GPL considerations that come with linking `libobs`.
