# 32-bit Mac games from Steam (experimental)

macOS stopped running 32-bit (i386) apps in Catalina, and Rosetta never ran
them, so a 32-bit Mac game in your Steam library no longer starts. AArchX can
run some of them on Apple silicon: [m32](m32.md) translates the game and runs it
against the Mac's own frameworks, and its Steam bridge connects the game to the
Steam you already have installed, so it signs in and reaches the Steam API as
it did on an Intel Mac. Nothing of Valve's is bundled and no Steam emulator is
involved.

This is experimental. It has been tested mostly with two games, Batman: Arkham
Asylum and Call of Duty: Modern Warfare 3, and with Left 4 Dead 2. Other 32-bit
games may run, stop early or misbehave; the
[games table](#games) says what has been tried, and grows as more is tested.

## What you need

- An Apple silicon Mac with macOS 26 or 27, and the Xcode Command Line Tools
  (`xcode-select --install`).
- Steam for Mac, signed in to the account that owns the game.
- A network connection for the one-time setup, which downloads Valve's
  Steamworks headers (about 50 MB, from Valve's Proton repository) and the
  32-bit C++ runtimes games built with GCC or Apple's clang need (from MacPorts).

## One-time setup

```sh
git clone https://github.com/mont127/AArchX.git
cd AArchX
make -j
make apis32                   # the 32-bit API databases; also makes the 64-bit ones (make apis)
make steam32                  # the Steam interface database the bridge reads
tools/fetch_guest32_cxx.sh    # the 32-bit GNU and LLVM C++ runtimes, for games that use them (Batman, MW3)
```

Each step is generated from your own Mac's SDK or fetched at a pinned version;
none of it is committed. Run the setup again after updating AArchX or Xcode.

## Playing a game

1. **Install the game in Steam** as usual. Steam marks 32-bit games with a
   warning ("Your current macOS version is unable to run 32-bit games"); it
   still installs them and lets you press Play. If Steam installs a game's
   Windows version instead, a tool that changes which platform Steam picks
   (one that runs Windows games through Steam) is probably active; turn it off
   for the install.
2. **Set its launch options.** In Steam, right-click the game, choose
   **Properties**, and under **General → Launch Options** enter, with the path
   to your AArchX folder:

   ```
   "/Users/you/AArchX/ocerz" -native %command%
   ```

   The path must be absolute. Steam replaces `%command%` with the game's app,
   or, for a game that starts through a script (Left 4 Dead 2), with the
   script; ocerz runs the script and has it start the game under ocerz.
3. **Press Play.**

Always start the game from Steam. Steam tells a game where its client lives
only when Steam launched it; started from a terminal, the game's Steam library
cannot find Steam and asks Steam to relaunch it.

## Games

| Game | Status | Notes |
| --- | --- | --- |
| Batman: Arkham Asylum Game of the Year Edition (Feral, 2014) | Plays | About 60 frames a second on an M4 Max. Signs in to Steam and gets the Steam interfaces it asks for. See below. |
| Call of Duty: Modern Warfare 3 (Aspyr, 2011), campaign | Plays | Launcher, intro movies with sound, and the campaign, on an M4 Max. Signs in to Steam. See below. |
| Left 4 Dead 2 (Valve) | Plays | Campaigns from Steam, on an M4 Max: 80 to 120 frames a second at the default settings, fewer in the largest fights. Signs in to Steam. See below. |

**Batman: Arkham Asylum.** Its options window (Feral's launcher) opens as a
black window; the game starts after it. The first run writes default settings,
key bindings included, and starts full screen. Sound, keyboard, mouse and
gameplay work. Not yet verified through the bridge: achievements, statistics
and cloud saves. The Steam overlay is not expected to appear.

**Call of Duty: Modern Warfare 3.** Aspyr's launcher opens first; its Play
button starts the campaign, which plays its movies and sound. Multiplayer is a
separate Steam app and has not been tried. Not yet verified through the bridge:
achievements and cloud saves.

**Left 4 Dead 2.** Steam starts it through a script, `hl2.sh`, which runs the
game under whatever `GAME_DEBUGGER` names; given the script, ocerz sets that
variable to itself, so the usual launch option works. The folder holding ocerz
must have no space in its path, because the script splits the variable at
spaces. The campaigns play with sound and music, ragdolls included, and
settings and key bindings are saved. The menu's background movies do not play:
they need QuickTime, which macOS no longer has. Some letters are spaced a
little wide. Typing in the developer console doubles each letter; a pasted
command ending in `;` runs once (its copy runs again and is harmless). Exit to
main menu crashes; quit from the game's menu instead. When the game has to
rebuild its sound caches (`sound/sound.cache`) it stays on the loading poster;
start it again and the menu appears.

Tried another 32-bit game? Open a
[compatibility report](https://github.com/mont127/AArchX/issues/new/choose)
with what happened and a log (below), so it can be added here.

## If something goes wrong

Steam starts the game without a terminal, so its output goes nowhere. To keep a
log, save this as `steam32.sh` beside `ocerz`, run `chmod +x steam32.sh`, and
use `"/Users/you/AArchX/steam32.sh" %command%` as the launch options instead:

```sh
#!/bin/sh
# runs a Steam game under ocerz with the Steam bridge's trace, writing ocerz's output beside this script
exec env OCERZ_M32LOG=steam "$(dirname "$0")/ocerz" -native "$@" > "$(dirname "$0")/steam32.log" 2>&1
```

- **The game quits at once.** Look at `steam32.log`. `has no` or `a superclass
  is missing` means the API databases are missing: run `make apis32`.
  `no Steam interface database` means `make steam32` has not been run.
- **The game relaunches itself in a loop, or says Steam is not running.** It
  was started outside Steam, or Steam is not signed in.
- **`... is not in the interface database`.** The game asks for a Steam
  interface version the database lacks; please report it with the log.
- **`..., which the bridge cannot cross, was called`.** The game used a Steam
  method the bridge does not support yet (most often one that registers a
  function). It is answered with 0; if the game misbehaves after it, report it.

How m32 and the bridge work is described in [32-bit Mac programs](m32.md).
