# Mouse camera diagnostics

Goal: find out whether the plugin can tell **exploring the world** (where the game needs the cursor
re-centering for the mouse camera) apart from **menus, dialogs and battles** (where the re-centering is
annoying). If it can, a future version could keep the mouse camera working and still stop the cursor
from jumping to the center in menus.

The diagnostic build changes nothing in the game. It only records:

- every `SetCursorPos` call: time, target position, where the cursor really was, and which place in the
  game code made the call,
- which places in the game code call `GetCursorPos`,
- whether the game registers and reads raw mouse input,
- numbered markers written when you press **F9**.

## Findings (first run, Linux, Proton with Wine 11.0)

The game has two modes and calls exactly one of these functions every frame:

| Mode   | When                                    | Called every frame |
|--------|-----------------------------------------|--------------------|
| Camera | world, NPC dialogs                      | `SetCursorPos(960,540)` (center of a 1920×1080 window) |
| Cursor | title screen, menus, battles, quitting  | `ClipCursor(NULL)` |

- The switch happens within a single frame, and the two never overlap.
- In camera mode, the camera turns by how far the cursor moved since the last frame (`GetCursorPos`
  right after `SetCursorPos`). The game also reads raw mouse input.
- All `SetCursorPos` calls come from one place in the game code.
- The cursor "jumps to the center" in menus only because camera mode left it there.

This led to the `MouseCamera=1` mode: the plugin lets the re-centering through and, on the first
`ClipCursor(NULL)` after camera mode, puts the cursor back where it was when cursor mode last ended.

## Setup

1. Close the game.
2. In the game folder, back up your current `DigimonCursorFix.asi` and `DigimonCursorFix.ini`
   (e.g. rename them to `.bak`).
3. Copy the diagnostic `DigimonCursorFix.asi` and the diagnostic `DigimonCursorFix.ini`
   (from the `diagnostics` folder) into the game folder.
4. Keep `dinput8.dll` and the Steam launch option as they are.

## Test run

The exact moment of pressing F9 does not matter. Press it, **then** do the action. The pauses in the
steps keep the phases apart in the log. If something takes a different number of steps, just keep
the order and note it.

| Marker | Do this |
|--------|---------|
| F9 → 1 | In the open world. Do not touch the mouse or keyboard for **5 s**. |
| F9 → 2 | Turn the camera with the mouse for **5 s**, slowly and then quickly. |
| F9 → 3 | Stop for 2 s, then open the main menu. Leave the mouse alone for **5 s**. |
| F9 → 4 | Still in the menu: move the cursor around for **5 s**, click something. |
| F9 → 5 | Close the menu. Back in the world, do not touch the mouse for **5 s**. |
| F9 → 6 | Talk to an NPC. While the dialog is on screen, move the cursor for a few seconds. Finish the dialog. |
| F9 → 7 | Back in the world: turn the camera with the mouse for **5 s**. |
| F9 → 8 | Start a battle. During the battle, leave the mouse alone for 5 s, then move it and pick an action. |
| F9 → 9 | After the battle, back in the world: turn the camera with the mouse for **5 s**. |
| F9 → 10 | Quit the game. |

Then send `DigimonCursorFix.log` from the game folder. It will be a few MB, which is expected.

Anything unusual (the camera did not react, the cursor did something odd, the game stuttered)
is worth a note with the marker number.

## Afterwards

Put your backed-up `DigimonCursorFix.asi` and `DigimonCursorFix.ini` back. The diagnostic build
lets the re-centering through, so the original problem is back while it is installed.
