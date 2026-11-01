# DigimonCursorFix

An ASI plugin for **Digimon Story: Time Stranger** that stops the game from snapping the mouse cursor
back to the center of the screen on every load, menu and dialog.

It was made for keyboard players. The re-centering is the game's own behavior, so the plugin should help
on Windows as well. On Linux (Steam + Proton, Wayland) it also removes the lag and visual artifacts that
the constant re-centering causes there.

## The problem

The game keeps calling `SetCursorPos` to move the cursor to the center of the screen. It does this on every
location change, battle load, menu and dialog window, in bursts of up to dozens of calls per second.
The in-game cursor jumps along with it, so it keeps popping up in the middle of the screen right over the text.

## What the plugin does

The plugin uses [MinHook](https://github.com/TsudaKageyu/minhook) to intercept these functions in `user32.dll`:

| Function        | What the plugin does                                                     | Default |
|-----------------|--------------------------------------------------------------------------|---------|
| `SetCursorPos`  | does nothing and returns `TRUE`, so the game cannot re-center the cursor. **This is the fix.** | on |
| `GetCursorPos`  | reports a fixed position (`-9999,-9999` by default)                      | on      |
| `ShowCursor`    | keeps the system cursor hidden, whatever the game asks for               | on      |
| `GetCursorInfo` | reports the same fixed position as `GetCursorPos`, everything else stays real | on |
| `ClipCursor`    | does nothing, so the game cannot lock the cursor inside its window       | off     |

The in-game cursor still exists and still follows the mouse. It just stays wherever you leave it.
Move it to a corner once and it will not get in the way again.

Every hook can be switched on or off in `DigimonCursorFix.ini` without rebuilding.

## Installation (Linux, Steam + Proton)

1. Download the latest release zip from the [Releases](../../releases) page and extract it.
2. Download **Ultimate ASI Loader** for x64 from
   <https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases> (`Ultimate-ASI-Loader_x64.zip`).
   It contains `dinput8.dll`.
3. In Steam, right-click the game → *Manage* → *Browse local files*. Copy these files into the folder that contains
   `Digimon Story Time Stranger.exe`:
   - `dinput8.dll` (the ASI loader),
   - `DigimonCursorFix.asi`,
   - `DigimonCursorFix.ini`.
4. In Steam, right-click the game → *Properties* → *General* → *Launch Options* and enter:

   ```
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```

5. Start the game.

### Windows

Copy the same three files into the game folder. No launch option is needed: Windows loads `dinput8.dll`
from the game folder automatically.

The plugin itself has been tested on Windows 11, but not together with the game yet. Feedback is welcome.

### Uninstall

Delete `dinput8.dll`, `DigimonCursorFix.asi`, `DigimonCursorFix.ini` and `DigimonCursorFix.log` from the game folder,
and remove the launch option.

## Checking that it works

After the game starts, `DigimonCursorFix.log` appears next to the `.asi`:

```
[14:46:03.162] DigimonCursorFix 1.0.0 — start
[14:46:03.162] Process:  S:\common\Digimon Story Time Stranger\Digimon Story Time Stranger.exe (PID 308)
[14:46:03.163] INI:      S:\common\Digimon Story Time Stranger\DigimonCursorFix.ini (found)
[14:46:03.163] Wine:     11.0
[14:46:03.163] Settings: Mode=1 OffscreenX=-9999 OffscreenY=-9999 LogIntervalSeconds=10
[14:46:03.168] Hooks:
[14:46:03.175]   GetCursorPos   OK (0x00006FFFFF56F530)
[14:46:03.183]   SetCursorPos   OK (0x00006FFFFF54A9FC)
[14:46:03.203]   ShowCursor     OK (0x00006FFFFF54AA2C)
[14:46:03.229]   GetCursorInfo  OK (0x00006FFFFF54BC10)
[14:46:03.229] Active hooks: 4
[14:46:13.238] Total calls: GetCursorPos=1666 SetCursorPos=27 ShowCursor=1 ClipCursor=0 GetCursorInfo=1
```

The *Total calls* line is written every 10 seconds, only when the numbers change. A growing `SetCursorPos`
count means the game tried to re-center the cursor and the plugin blocked it.

The log is small and is overwritten on every start. You can turn it off with `Log=0`.

## Troubleshooting

**No log file appears.** The ASI loader was not loaded. Rename `dinput8.dll` to one of the names below and
change the launch option to match. Keep only one copy of the loader in the folder.

| Loader file name | Launch option                               |
|------------------|---------------------------------------------|
| `version.dll`    | `WINEDLLOVERRIDES="version=n,b" %command%`  |
| `winmm.dll`      | `WINEDLLOVERRIDES="winmm=n,b" %command%`    |

**The game crashes or does not start.** To rule out the plugin, set all `Hook...` values in the INI to `0`
or temporarily rename the `.asi` file. Then open an issue (see below).

**Something about the cursor still misbehaves.** Try `HookClipCursor=1` or `Mode=2`. The INI file describes
every option.

## Known limitations

- The plugin is meant for keyboard play. The in-game cursor still follows the mouse, but playing with the mouse
  has not been tested with the plugin active.
- Mouse control of the Steam overlay (`Shift+Tab`) may not work, because the overlay runs inside the game process.
  The overlay can still be used with the keyboard.

## Compatibility

Tested on Linux (Wayland) with Proton (Wine 11.0) and Ultimate ASI Loader as `dinput8.dll`, using the default INI.
The re-centering stopped on loads, location changes, battles, menus and dialogs.

Not yet tested with the game on Windows or on the Steam Deck.

## Reporting issues

Please open an [issue](../../issues) and include:

- `DigimonCursorFix.log`,
- your OS, and your Proton version if you play on Linux,
- what you did and what happened.

This is a hobby project maintained in my spare time. I will look at issues when I can, but I cannot
promise a timeline.

## Building from source

Requirements: Visual Studio 2026 with the *Desktop development with C++* workload.

1. Open `DigimonStoryTimeStranger_CursorFix.slnx`.
2. Select the **Release** configuration and the **x64** platform.
3. Build → Build Solution (`Ctrl+Shift+B`).

The output folder `bin\x64\Release\` contains everything that goes into a release: `DigimonCursorFix.asi`,
`DigimonCursorFix.ini`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` (plus a `.pdb` with debug symbols that is
not part of the release).

The DLL links the C runtime statically and depends only on `kernel32.dll` and `user32.dll`, so it does not
need a Visual C++ Redistributable in the Proton prefix.

MinHook v1.3.4 is included as source code in `external/minhook` and is compiled together with the project.
To update it, replace `include/` and `src/` with a newer version and update `external/minhook/VERSION.txt`.

## License

DigimonCursorFix is released under the [MIT License](LICENSE.txt). It is provided "as is", without warranty
of any kind. You use it at your own risk.

It includes [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, licensed under the BSD 2-Clause
License. See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

This is an unofficial fan project. It is not affiliated with or endorsed by Bandai Namco Entertainment.
*Digimon* and *Digimon Story: Time Stranger* are trademarks of their respective owners. The plugin contains
no game code or assets.
