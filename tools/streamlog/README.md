# FRSStreamLog.asi

An in-game logger for NFSU2 PC v1.2. It records what the track streamer does, to check
`docs/STREAMING-STUDY.md` in the game (§4.3, experiments E1–E7).

It only reads memory. It patches nothing, hooks nothing and calls no game function. A thread of its own polls
the streamer every few milliseconds and logs what changed, plus one status line per second while driving. If
the exe is not v1.2 (the prediction constants are checked at start), it logs one line and stops.

## Build and install

```
build.bat
```

This makes `build\Release\FRSStreamLog.asi` (x86, Release, static CRT). To install it, copy that file into
`<game>\scripts\`, next to the other `.asi` mods. To uninstall, delete it.

The log goes to `<game>\scripts\FRSStreamLog.log`. Each game start moves the previous log to
`FRSStreamLog.prev.log`, so **copy the log out after each session** if you run several.

**F12** (only while the game window has focus) writes `MARK n` and a full snapshot. Write down what each mark
number was for.

## Reading the log

Every line starts with `[time fFRAME]`. FRAME is the game's `RealTimeFrames`.

| Line | Meaning |
|---|---|
| `STAT` | once a second: car position and speed; `coll` = the car's collision zone; `tree` = the collision pack bound to the car (NULL = no ground, no walls); `load` = loading zone; `cam` = camera zone; `pred` = prediction ON/off with the predicted point; phase, reads, counters; wanted/loaded/active/dropped counts; pool used/free/largest |
| `ZONE` | the loading zone changed (SwitchZones), with position, velocity and predicted point |
| `CAMZONE` | the camera's zone changed |
| `PRED` | the loading zone moved ahead of the car's own zone (prediction took over), or came back |
| `COLL` | the car's collision zone or bound tree changed |
| `GAP start/end` | the car is in a zone whose collision pack is not bound (tree NULL or another zone's), and how many frames and ms it lasted |
| `WANTED`, `WANT+`, `WANT-` | the wanted list changed: sections added and removed |
| `UNLOAD`, `ALLOC`, `READ`, `LOADED`, `ACTIVE` | section status changes. Sizes are in KiB; `ACTIVE C25:full/current` gives the file size (+0x18) and the size kept after activation (+0x4C) |
| `DROPPED` | sections the streamer gave up for lack of pool memory |
| `PHASE`, `PHASE6` | loading phase; `PHASE6` (done) prints a status line |
| `USERMEM` | user memory held (loading is paused while it is above 0) |
| `SNAP` (after F12) | full state: every wanted section with status and sizes, loaded-but-unwanted sections, the dropped list, and the collision slot for the car's zone |

Zone and section names are written like the editor's: `C5` is zone C5, and `C25` its geometry and collision
section (zone + base, where base is 20 in L4RA).

## Experiments

Start each one in free roam. Press F12 at the moments listed. Send back `FRSStreamLog.log`, the screenshots, and
a list of what each MARK number was.

**E1, border crossings (does collision go missing at zone borders?).**
Drive across the borders C3→C5, C5→C3 and C6→C9, each once slowly and once at full speed. Press F12 just before
each crossing. Look for `GAP` lines: the study expects a short gap when the new zone's pack was not preloaded and
no prediction zone covered the crossing. Also note whether the car dipped or fell through anything.

**E2, pool (how full does it get, and does anything get dropped?).**
1. Stop in C3 and wait for `PHASE6`, then press F12.
2. Drive to C8, wait for `PHASE6`, then press F12.
3. In the editor, add heavy content to a section on C3's list (for example many large objects in C23), install
   it, and repeat steps 1 and 2.

Compare pool free/largest and the `DROPPED` lines.

**E3, sizes after activation.**
Nothing special to do. Drive through a few zones; the `ACTIVE` lines give each section's full and resident size,
and every F12 snapshot lists them. This settles whether the pool keeps only the permanent part.

**E4, models from another section.**
In the editor, place an object whose model exists only in a section that the object's zone does not list. In the
game, go to it, press F12 and take a screenshot (expected: invisible). Then drive into a zone that loads the
model's section, come back into view, press F12 and take another screenshot (expected: visible).

**E5, textures.**
Make an object use a texture whose pack is not loaded in its zone. Press F12 and take a screenshot (expected: the
default texture, not invisible).

**E6, precull.**
In the editor, move a shipped object about 100 m. In the game, look at it from 3–4 different camera spots around
it. Press F12 and take a screenshot at each spot. The log gives the camera zone; the screenshots show whether
it is drawn.

**E7, zone 0.**
Drive along an editor road that leaves every zone outline. Press F12 once you are outside. Expected: `ZONE ... -> 0`,
a long `WANT-` list, `UNLOAD` of the city sections, and a `COLL` line with zone 0 or tree NULL. Stop before the car falls
too far, or let it fall and note what happened.
