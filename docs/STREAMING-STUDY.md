# Test drive that behaves like the game: streaming, models, collision (feasibility study)

SPEED2.EXE v1.2 (installed copy checked: the pool and constant tables are unpatched). The addresses come from
`D:\Outros\Underground2\UG2.txt`. The data figures come from `F:\Games\NFSU2\TRACKS\L4RA.BUN`, as installed
(the editor may have written to it), and from `ROUTESL4RA\PathsFreeRoam.bin`. The analysis scripts are in this
`tools/streaming-study/`: `dump_stream.py`, `lists2.py`, `adj.py`, `pz.py` and `pe.py`.

Tags: **[C]** confirmed (read in the code, or measured in the data), **[I]** inferred, **[O]** open, with the
experiment that would settle it.

---

## 0. Summary

1. **The game uses three different "zones" at once, and the editor has only one.** Each zone is looked up with
   the drivable outlines in `0x34152`.
   - **Loading zone:** where the car is, or where it will be 1.5 s later (at most 100 m ahead) inside a
     *streamer prediction zone* (`PathsFreeRoam.bin`, type 0x0C). It decides what is in memory.
   - **Drawing zone:** where the camera is. It decides which loaded sections are drawn.
   - **Collision zone:** where the car is, with no prediction. It decides the single collision pack that the
     wheels and the walls use.
2. **Loaded set = X0 + Y0 + Z0 + (per-track Z section) + the 0x34153 list of the loading zone.** The list is
   replaced by a union of several lists when the zone belongs to a "CC" group (`0x34155`). Everything else is
   unloaded at once when the zone changes [C].
3. **Outside every outline the zone is 0.** The loaded set then shrinks to X0/Y0/Z0, so the whole city, its
   collision and its scenery are unloaded. The camera draws only loaded sections numbered `n%100 < 10` [C].
   Any road or object the editor builds outside the outlines therefore fails in the game, even though it
   works in the editor.
4. **Models and textures are bound by hash at run time, globally, over every loaded pack** [C]:
   - Load order decides which pack wins when two packs hold the same hash.
   - A model that is not loaded draws nothing. It appears by itself when a pack that holds it is loaded.
   - A texture that is not loaded falls back to `DefaultTextureInfo` (hash 0x66994F4A).
5. **Wheels and walls use the same mechanism.** One `TopologyCoordinate` per car binds to the tree in slot
   `Slot(zone)`. The port already has this logic. What differs is the set of trees that exist: the port has
   every section's tree, the game has only the activated sections' trees. The slot rule also differs in edge
   cases (see §3.3).
6. **Pool:** 60 000 KiB (one player, every region except L4RG). The worst L4RA zone wants 38.5 MiB at full
   file size, so there is about 20 MiB of headroom. On overflow the streamer drops *wanted* sections in a fixed
   priority order, silently. It does not crash and does not wait. Geometry sections 20..39 are never dropped.
7. **Two more culling layers that the editor does not reproduce:**
   - The per-section instance tree `0x34104` (frustum culling).
   - The per-instance 1024-bit camera-cell masks `0x34106` (precull).

   On top of those, a pixel-size test uses SceneryInfo `+0x3C` (radius) and picks the LOD from `+0x30/+0x34/+0x38`.
   A placement can be invisible in the game for any of these reasons.

---

## 1. TrackStreamer

`TheTrackStreamer` is at 0x88EB00 and `TheVisibleSectionManager` at 0x883E70.

### 1.1 Objects and fields [C unless marked]

**TrackStreamer**

| Offset | Meaning |
|---|---|
| +0x00 / +0x04 | section array (the `0x34110` table used in place, stride 0x50) and count |
| +0x08 / +0x0C | `0x34113` load groups (empty in L4RA) |
| +0x10 | `0x34111` (8 zero bytes in L4RA; reader not found) |
| +0x14 / +0x18 | count and pointer of `0x34112` = **streaming barriers**: 16-byte 2D segments (6 in L4RA). The editor names this chunk `kChunkTrackBounds`, which is wrong. |
| +0x20 | reads in flight (at most 2) |
| +0x24 | sections activated |
| +0x28 | sections that "did not fit" |
| +0x30 + 64·i | file names; type 1 = `TRACKS\STREAM<region>.BUN` (Init at 0x5DA970) |
| +0xB0 + 0x5C·p | streaming position p = 0..1 (two players): +0 car index or -1, +4 pos xy, +0xC vel xy, +0x14 "set" flag, +0x16 current zone, +0x18 last zone, +0x1C frame counter |
| +0x168 | loading phase 0..6 |
| +0x16F | debug text "A21 - A22" |
| +0x184 / +0x188 | sections dropped for lack of memory |
| +0x288 | refresh request |
| +0x298 / +0x29C | wanted list |
| +0x69C / +0x6A0 / +0x6A4 | pool memory, size, pool object |
| +0x6A8 | user allocations currently held |
| +0x6AC..0x6B2 | 4 extra pinned section numbers (the setter was not found; only the replay snapshot touches them) |

**Section entry** (table entry, 0x50 bytes)

| Offset | Meaning |
|---|---|
| +0x08 | i16 number (letter·100 + n) |
| +0x0B | wanted bits (bit p = position p) |
| +0x0C | status: 0 none, 1 allocated, 2 reading, 3 loaded, 4 activated |
| +0x10 | file type |
| +0x14 | offset |
| +0x18 | size (allocation size while loading) |
| +0x1C | size without pixel data [I: the resident size after activation] |
| +0x20 | order key |
| +0x24 / +0x28 | centre xy |
| +0x2C | **radius** (closes the open item "centre z is not the ground" in world.md §7) |
| +0x34 | frame when last wanted (LRU) |
| +0x38 | frame when unloaded |
| +0x3C | frame when loaded |
| +0x40 | load priority |
| +0x44 | memory |
| +0x48 | group |
| +0x4C | current size |

**VisibleSectionManager**

- +0x00: drivable outlines (`0x34152` with letter A..T and `1 <= n%100 < base`).
- +0x08: other outlines.
- +0x10: `0x34153` list records. Fixed 0xA4-byte stride, so **at most 72 sections per list**: +8 zone,
  +0xC pointer to its outline, +0x10 count, +0x14 i16 list.
- +0x20: `0x34155` CC groups, 76 bytes each: +8 name[16], +0x18 count1, +0x1A zones[16], +0x3A count2,
  +0x3C sections[8].
- +0x28: `0x34154` per-track records, 32 bytes each: +8 track id, +0xC count, +0x10 sections[8].
- +0x4C: `0x34151`. Its first u32 = **base** (`dword_800DFC`, 20 in L4RA); then a count and a u16 list.

The chunks are read by `sub_5E9C10`, which `LoaderVisibleSections` 0x5EA0F0 calls.

### 1.2 What drives streaming [C]

`UpdateCameraMovers`, from 0x44F0D6:
- For each player view, the streaming position follows the car that the active camera mover is attached to
  (`sub_5D6020`).
- Without a car, it follows the camera position and velocity (`sub_5D6040`: camera+0x40 and +0x1E0).

`TrackStreamer::ServiceGameState` 0x5E86E0 runs from `World::DoTimestep` (+0x59D) every simulation step:
1. `sub_5E5300` (DetermineCurrentZones) computes the zone of each position. A change of zone, or a refresh
   request, calls **`SwitchZones` 0x5E7AF0**. The switch is immediate: there is no time hysteresis.
2. Then `sub_5E6C10` activates **one** loaded geometry section per step.

**Zone of a position**, `sub_5E4F20`:
- `pos` and `vel` come from `pCurrentWorld+0x1C[idx]` (+0x60/+0x64 and +0x70/+0x74).
- `speed = |vel.xy|`.
- **Prediction zones.** For every `TrackPathZone` of type **0x0C** that contains `pos`
  (`TrackPathManager::FindZone`):
  - Predicted point: `pred = pos + vel·1.5` (`flt_7A47A4`), clamped to 100 m (`flt_7A47A8`). When
    speed > 178.81 (`flt_7A52F4`, 400 mph), `pred = pos`.
  - Look up `pred`'s zone with `sub_5E00D0`. If that zone is one of the 4 ints at path-zone +0x30..+0x3C,
    it is used.
  - L4RA has **131** type-0x0C zones, each naming 2..4 zones (`pz.py`).
- **Barrier check.** If a predicted zone was taken and `pred != pos`, every `0x34112` segment is tested
  against pos→pred (`sub_5D4D50`, segment intersection [I]). A crossing cancels the prediction.
- **Fallback.** Otherwise the zone is `sub_5E00D0(pos)`:
  - MRU list of 0x34153 records whose outline contains the point (box, then crossing test).
  - Else the nearest outline, accepted only within 0.1 (`sub_5DB770`, `_zero_point_one`).
  - **Else 0.**

### 1.3 What gets loaded [C]

`sub_5E5110` builds the wanted list:
1. Always: **X0 (2400), Y0 (2500), Z0 (2600)**.
2. The `0x34154` sections of the loaded track id (`LoadedTrackInfo+0x8A`): in L4RA tracks 4001.. → Z1..Z12.
   None for free roam.
3. `ScenerySectionToBlink` (debug tool), and the 4 extra sections at +0x6AC.
4. Per position, with zone > 0:
   - If the zone is in a CC group's zones[] (`0x34155`): the union of the `0x34153` lists of every zone in
     that group, plus sections[] (a drivable id also adds id+base). This is `sub_5DFF50`, capped at 0x180 entries.
     - L4RA has 107 CC groups covering 160 zones, 10 of them with extra sections. Example: CC21 = {R9, R13} + R5.
   - Otherwise the zone's own `0x34153` list.
5. Split screen only (`dword_89E7C0 == 2`, inferred to be the number of players): X/Y numbers below 50 are
   remapped to n+50. This selects the low-detail libraries X50..X75 (`sub_5DACB0`), which single player never loads.

**Data facts for L4RA** (`lists2.py`):
- 255 lists, one per drivable zone; longest 68 entries.
- Every list contains its own zone's geometry section (zone+20).
- Near neighbours appear both as id (Cn) and as geometry (C(n+20)); far ones only as geometry.
- A list may name a zone id that has no table entry (e.g. A1). It is skipped.
- **Section numbering** (base 20):
  - 1..19: drivable zone ids. 17 of them are real sections: C1, C4, C5, C7, C10, C12, C18, D7, G1, G16,
    M12, M19, N11, O6, R7, R9, R13. Each has the same centre as its n+20 twin and is loaded only when that
    zone is a near neighbour. **M12 is a near-detail add-on to M32, not an alternative to it.**
  - 20..39: the 255 geometry+collision sections.
  - 40..99: 95 extra sections, e.g. C41..C93 props, A98/A99/C97..99/E94..99 large-radius backdrops, M41..M51.
  - The "alternate set" (order key ≥ 20000) is exactly the sections numbered 1..19 and 40..99.
  - X1..X25 and Y1..Y25 libraries are listed per zone (e.g. "X1 Y1" in zone A1).
  - **Never loaded in single player:** X50..X53, X58..X60, X75, J99.

### 1.4 SwitchZones and the loading phases [C]

**SwitchZones** 0x5E7AF0:
1. Records the new zone and rebuilds the wanted list.
2. **Immediately deactivates and frees every activated section that is no longer wanted** (`sub_577B10` +
   `sub_5DAAC0`), except X/Y libraries. Those are freed later, in phases 3 and 5.
3. Phase = 1, then computes load priorities (`sub_5DF580`).

**HandleLoading** 0x5E6A70 is a phase machine:

| Phase | Work |
|---|---|
| 1 | allocate Y libraries + X0/Y0/Z0 |
| 2 | read + activate |
| 3 | free unwanted Y, allocate X libraries |
| 4 | read + activate |
| 5 | free unwanted X, allocate everything else |
| 6 | read + activate → done (`sub_5DB010`) |

Notes on the phases:
- **Read order** inside a phase: lowest `+0x40 = urgency·100000 + order key`. Urgency 0..2 comes from
  `sub_5DF3A0`, a time-to-reach of the section's outline along the velocity; libraries get 0 [I for the
  formula's details]. The order keys are 10 (X0), 20 (Z0), 30 (Y0), 2001.. Y, 3001.. X, 10000+ main,
  20000+ alternate.
- **Reads:** `AddQueuedFile` (0x5E6335), at most 2 in flight.
- **Activation** (`sub_5DAA40` → `sub_577C70` = load chunks, keep the permanent part):
  - Libraries, Z sections and sections numbered 1..19 / 40..99 activate as soon as their read finishes.
  - **Geometry sections 20..39 wait.** `sub_5E6C10` activates one per `ServiceGameState` (`sub_5D6190(0)`).
  - Collision trees, scenery and models appear at activation.
- **HandleLoading pauses** while user memory is held (+0x6A8 > 0), for example a race preview, an NIS or a
  car viewer.

### 1.5 Memory pool [C]

- `RegionLoader::FinishedLoading` 0x57A580 → `TrackStreamer::InitMemoryPool` 0x5E4E70.
- Size = `TrackStreamingPoolSizes` 0x79DC54 = **{60000, 43000, 36000} KiB**:
  - index 0: normal;
  - index 1: `dword_89E7C0 == 2` (split screen [I]);
  - index 2: region `L4RG`.
- The pool is a dedicated bMalloc pool ("Track Streaming", flags 0x2011). `sub_43E8E0(1)` returns the free
  bytes and `bLargestMalloc(1)` the largest block.

**When the wanted set does not fit** (`sub_5DF1C0` / `sub_5DADB0`):
1. Free loaded-but-unwanted sections, least recently wanted first.
2. If `requested + 16 KiB > free`, **drop wanted sections** one at a time with `sub_5D5EB0`, until it fits.
   Fragmentation gets the same treatment. Highest score is dropped first:

   | Score | Sections |
   |---|---|
   | 999 (+1 if not loaded) | A..T numbered 40..99 |
   | 10 | A..T numbered 1..19 |
   | 2 | X/Y libraries |
   | 1 | X0 and Y0 |
   | never | A..T geometry 20..39, Z, U..W |

3. Dropped sections go to the "didn't fit" list (+0x188) and count as done (+0x28). They are retried in the
   next phase and then stay missing until the next zone switch.
4. **No wait, no crash.** If even the geometry does not fit, the rest is simply not loaded. The exception is
   `bMalloc` failing after `bLargestMalloc` said it fits: then `bBreak`, then a NULL read. That should not happen.

**Shipped L4RA headroom** (`dump_stream.py stats`, full file sizes `+0x18`, X0/Y0/Z0 included):
- Per zone: 32.7 MiB (P17) to **38.5 MiB (C3)**, against a pool of 58.6 MiB.
- Y0 alone is 24.4 MiB at full size, but only 166 KiB permanent. The resident steady state is much lower than
  the full size [I: `+0x1C` = permanent size; to confirm in the game].

### 1.6 Blocking [C]

**Normal driving never blocks.** The car is never frozen, and missing sections just pop in later.

The blocking paths:
- `sub_5E79D0` loops `HandleLoading` + `ServiceResourceLoading` until every wanted section is activated or failed.
  - Through `sub_5E9650`: **`Player::HandleTeleportToTriggerZone`** (0x600F72) and `CAnimPlayer::Load` (NIS).
  - Through `AllocateUserMemory` 0x5E8670 → `sub_5E8560`.
- The race loading state waits on `AreAllSectionsActivated` 0x5D6160.
- Replays force the recorded section through a blocking loop (`sub_5E6C10`).

### 1.7 Drawing set [C]

`sub_5E99C0` / `sub_5E96B0`, per view, run from the scenery cull at about 0x5E9BE0:
- Zone of **the camera position** (view+0x40 → +0x40) with `sub_5E00D0`.
- **Zone found:** draw every loaded Z0..Z99 section, plus every section of *that zone's* `0x34153` list that is
  loaded (`dword_883EC0[number]` = per-number scenery table, +4 = pack).
- **No zone:** draw only loaded sections whose `n%100 < 10`.
- So a chase camera in another zone draws that zone's list, but only what the car's loading zone brought in.

Per section, `sub_5D69B0` walks the `0x34104` node tree:
- Nodes are 36 bytes: box 6f, i16 count at +0x18, i16 children at +0x1A (≥0 instance, <0 node).
- Frustum culling uses `eView::GetVisibleState`.

Then `sub_5D6570` per instance:
- **Precull bit:** if `0x34106[instance+0x1C]` has the view's cell bit set, the instance is skipped. The cell is
  `((y>>5)&31)·32 + ((x>>5)&31)`, a 32-unit grid wrapping every 1024 units [I for the exact coordinates]. The
  gPrecullerBooBoo script can disable it per cell.
- Instance flags (+0x1A, xor 0x60) are tested against the view's mask (hidden = 0x10).
- The instance box is tested against the frustum when its node was partly visible.
- **Pixel size** from SceneryInfo `+0x3C` radius + 6.0 (`sub_5D64D0`). The model is skipped below 17 px in
  main views (23 in other views, 16 for 0x20 views, 32 for reflection views).
- **LOD:**
  - `+0x34` (`_1B_`) by default.
  - `+0x30` (`_1A_`) only when the size/triangles ratio ≥ 4 **and** the detail setting `sub_5BE1C0() == 2`.
  - `+0x38` (third hash) in the 0x18 views (reflection/env), when instance byte +0x1B bit 2 is clear.
  - **A null eModel draws nothing** (test at 0x5D673C).
  - This answers the "LOD" open item of world.md §7.

---

## 2. Models and textures at run time [C]

### 2.1 Scenery

- The `0x80034100` scenery loader is `sub_5DF8A0`, run at section activation.
- For each SceneryInfo (68 bytes), it creates up to 3 `eModel`s from the hashes at +0x20/+0x24/+0x28, stored at
  +0x30/+0x34/+0x38. A zero hash gives no eModel. SKYDOME and 0x72923C81 are skipped.
- `eModel::Init` 0x493600 → `eFindSolid(hash, NULL)` 0x48DF90:
  - It searches **every loaded solid pack** (`SolidListHeaderList`, in load order, first hit wins; filtered by
    `SolidLoadedTable[hash & 0x1FFF]`).
  - When no pack has the hash, the eModel waits in the unattached list `dword_82DBC0`.
  - `sub_490650` attaches waiting eModels when a pack loads.
  - When a pack unloads, its eModels go back to waiting (`sub_493650`).
- So **a model can come from any loaded section**, not only its own section or the libraries. If the solid is
  in no loaded section, the instance draws nothing, with no fallback [I that the render path checks
  `eModel+0xC`; `GetBoundingBox` 0x48D7E0 does].
- **For ModelCopy and placement:** an instance draws in the game only where a section that holds its hash is
  loaded, i.e. in every zone whose *camera* draw list includes the instance's section. Copying the model into the
  instance's own section, as the editor does, is the safe rule. "Model in another positioned section" works only
  where both sections happen to be loaded. Duplicate hashes resolve to whichever pack loaded first, which differs
  per zone.

### 2.2 Textures

`GetTextureInfo(hash, wantDefault, …)` 0x4901D0:
1. Searches every loaded texture pack (MRU list `dword_830AF0`).
2. Then the remap table `dword_82D530 → dword_82D5C0`.
3. Then **`DefaultTextureInfo`**, the texture whose hash is 0x66994F4A (set at 0x48FDF3).

Loading a pack sets `eDirtyTextures` (0x48FDE7), so solid texture tables are re-resolved [I: in `eFixUpTables`].

Consequences:
- A texture whose pack is not loaded draws as the default texture, not invisible [I that solids ask for the
  default; to confirm with a screenshot].
- Per-section TPKs (the 0xB3300000 chunk in the section) and the Y libraries (Y0 = 24 MB, always loaded) are
  equivalent once loaded. A Y1..Y25 pack is loaded only where a zone list names it.

### 2.3 Collision chunk loading

- `sub_5E78A0` handles `0x80034130` and `0x3412F`.
- The tree's slot comes from **the pack header's section number** (first u16 of the aligned data), not from the
  section that holds the pack.
- `slots[Slot(n)] = tree` is **unconditional: the last one loaded wins**.
- Unloading (`sub_5E8210`) clears the slot and frees *whatever tree is in that slot*.
- So two loaded packs with the same slot corrupt each other.

---

## 3. Collision

### 3.1 The zone rule, completed [C]

`TopologyCoordinate::SetSectionPosition` 0x5E68D0:
1. Once the point has moved (Manhattan) more than `slack + radius`, it runs
   `VisibleSectionCoordinate::FullUpdate` 0x5DFEB0 → `sub_5DB850`.
2. Zone = the outline's number.
3. If the coordinate is not connected: `GetIndex(zone)` and connect, if a tree exists.
4. If it is connected to a tree with `number - base != zone`: reconnect to `GetIndex(zone)`, which may be NULL.
5. `sub_5E6980` connects waiting coordinates when a tree loads.

`Slot(n)` = `sub_5D3C10`: `(letter-'A')·20 + (n%100 in [base,2base) ? n%100-base : n%100)`. Consequences:
- **Only numbers 20..39 fold correctly.**
- Zone ids 1..19 share the slot of their n+20 twin (M12 ≡ M32).
- **40..99 alias into the next letters' slots.** C41 → slot 81 = E1.
- So collision is only safe in packs whose header number is zone+base, in sections 20..39.

### 3.2 Wheels and walls use the same mechanism [C, consistent with the port]

The car's one coordinate is at `Car+0x5C4`:
- The wheels use it through `NewGetElevation` 0x5E72E0, slack 9.
- The GJK body uses it through `sub_5E7100`, slack 0 at the box centre, whose interest polygons are taken from the
  same tree.

The port (`TopologyU2.cpp`: `SetSectionPosition`, `FullUpdate`, `Slot`, `LoadTree`/`UnloadTree`) mirrors this.
Ground and walls therefore both come from the one tree bound to the car's zone, and zone-list membership is
irrelevant to collision.

### 3.3 What the editor's "everything loaded" `BuildTrackWorld` changes

| | Game | Test drive today |
|---|---|---|
| Trees present | the activated sections only | every positioned section with a pack |
| Car's zone pack not loaded (zone switch, pool drop, zone 0) | the coordinate binds to NULL: no ground, no walls; it reconnects at activation | always connected |
| Outside every outline | zone 0: no collision, **and the loaded set collapses** | no collision, but the scenery is still drawn |
| Slot key | pack header number, last activated wins, unload clears the slot | table number, main set first, then the first alternate |
| Library / Z section packs | would load (and alias to slots ≥ 460) | skipped |
| Several packs in one section | each registers, the last wins | takes `collision.back()`, which is the same result |

The shipped data never hits the slot differences, because there are exactly 255 packs in 255 sections numbered
20..39 with matching headers. The editor's own writes can. Being always connected hides two real game failures:
- (a) a pack that is not loaded at a zone crossing;
- (b) roads or ground outside the outlines.

**Border risk measured statically** (`adj.py`):
- 1 614 pairs of drivable outlines lie within 1 m of each other.
- In 356 of them, the neighbour's geometry section is not in the current zone's list; still 319 after the CC
  unions.
- If such a border is crossable, the new zone's collision appears only after the read and activation, unless a
  type-0x0C prediction zone switched the loading zone earlier.
- Whether these borders are crossable cannot be told statically [O; see E1].

---

## 4. Feasibility for the editor and a plan

### 4.1 Building blocks (no more RE needed)

**A. `GameStreamModel`, pure data, about 1–2 days.**
- Inputs: layout with spans (section table, `0x34151` base, `0x34152` outlines, `0x34153` lists,
  `0x34154`/`0x34155`, `0x34112` barriers), the type-0x0C zones of `PathsFreeRoam.bin` (the reader exists:
  `nfs/FreeRoam.cpp`, 0x44 header + polygon, sections at +0x30..+0x3C), and the pool size read from the exe at 0x79DC54.
- Functions:
  - `ZoneAt(pos)`: MRU + 0.1 m rule. The game is not "smallest containing", which is what `Preculler::ZoneAt`
    does today.
  - `LoadingZone(pos, vel)` (§1.2).
  - `WantedSet(zone, trackId)` (§1.3).
  - `CameraDrawSet(camPos, loaded)` (§1.7).
  - `CollisionZone(pos, current)`: keep the current zone while inside.
  - `PoolFit(wanted)`: full and permanent sizes, plus the §1.5 drop order.

**B. Test drive in "game mode", about 3–5 days in total.**
1. **HUD**, half a day: loading / camera / collision zone, CC group, prediction active, tree number connected,
   wanted count, MiB wanted vs pool, dropped sections. Useful right away.
2. **Collision from loaded sections only**, about 1 day:
   - Build every tree once.
   - Call `TopologyManagerU2::LoadTree`/`UnloadTree` when the simulated set changes (both are already ported).
   - Key the slots by the pack header and let the last one win.
   - Option: "no ground → stop the car and warn" instead of letting it fall.
3. **Rendering of the camera zone's draw set ∩ loaded set**, 1–2 days, depending on how the renderer filters
   sections:
   - Model resolution limited to loaded packs: precompute, per instance, the sections whose solid packs hold
     each LOD hash; the instance is drawn if any provider is loaded.
   - Texture resolution: same, with providers per texture hash; otherwise the game's default texture (or magenta).
4. **Timing**, optional:
   - "Instant": wanted = loaded.
   - "Game-like": unload at once, then load in phase and priority order, about 2 reads in flight with a
     configurable MB/s, and one geometry activation per step. This makes pop-in and missing collision at
     crossings visible. The exact timing depends on the disk and cannot be reproduced exactly.

**C. "What the game would load here", outside the drive, about 1–2 days on top of A.** Click a point, or audit
the whole map, and report:
- Zone and CC group; wanted list with sizes (full / permanent) and pool use; list room (72 maximum).
- Collision zone, pack and slot. Warnings:
  - slot alias (header number not in 20..39);
  - two packs on one slot;
  - zone without its pack;
  - points of editor roads or pieces outside every outline (zone 0).
- Instances whose model hash or texture hash is not provided by any section loaded where the instance is drawn.
- SceneryInfo with `+0x24 == 0`, or a zero or too small `+0x3C` radius (invisible or culled in the game).
- Placements not reachable through the `0x34104` tree, or with a precull index whose mask hides them from
  nearby cells. The editor's `SceneryEdit` already maintains the tree; check its bounds after moves.
- Neighbour borders not preloaded and not covered by a prediction zone.

### 4.2 Needs more RE, and how to settle it

| Item | Why | How |
|---|---|---|
| Permanent vs full size after activation (`+0x4C` after `sub_5DAA40`) | the pool model | logger E3 |
| Precull cell coordinates (0x5E9B69: camera position source) and the 0x34106 mask semantics for new or moved instances | invisible placements | read about 50 lines more at 0x5E9A70; experiment E6 |
| `sub_5DF3A0` urgency formula | read order only | port it if the timing mode needs it |
| `FindZone` polygon test for type-0x0C zones | prediction | read `TrackPathManager::FindZone` (small) |
| Does `sub_5D6570`'s caller skip eModels without a solid; is the default texture used for solids | expected look of missing assets | E4, E5 screenshots |
| Is the 0x34111 / 0x34113 data ever non-empty in other regions | completeness | run `dump_stream.py` on L4RB..L4RR |

### 4.3 In-game verification: a small ASI logger (`FRSStreamLog.asi`)

Base it on the SpeedLoader/fastloader hook infrastructure. The user runs the game.

**Poll once per frame** (e.g. from the existing Present/EndScene hook, or a hook of `ServiceGameState`
0x5E86E0) and log on change:
- Current zone: `*(i16*)(0x88EB00+0xC6)`. Debug text: `0x88EB00+0x16F`.
- Phase: +0x168. Counters: +0x20/+0x24/+0x28.
- Wanted list: +0x298/+0x29C → each entry +8 number, +0xC status, +0x4C size.
- Pool: `sub_43E8E0(1)` free, `bLargestMalloc(1)` 0x43E910 largest block (both cdecl `int(int pool)`).
- Dropped list: +0x184/+0x188.
- User memory: +0x6A8.
- Camera zone: `dword_800DF0`, which `sub_5E96B0` updates for view 1.
- Player car's coordinate: `Car+0x5C4` → +0x08 tree → +0x30 number; and +0x0C+8 boundary → +8 number.

**Experiments:**
- **E1, crossings:** drive across a listed "not preloaded" border (e.g. C3→C5, C5→C3, C6→C9). Log the
  frames where the zone changed and the tree is NULL. Crash- and freeze-free by construction.
- **E2, pool:** stand in C3 and C8; then add heavy content to a C3-listed section and compare the free bytes
  and dropped list.
- **E3, sizes:** log `+0x18` vs `+0x4C` for a section before and after activation.
- **E4, models:** place an instance whose model lives only in a section that the zone does not list. Expect it
  invisible there, and visible after driving to a zone that loads that section.
- **E5, textures:** remove a texture's pack from the zone; screenshot.
- **E6, precull:** move a shipped instance about 100 m and look at it from several camera cells.
- **E7, zone 0:** drive an editor road out of every outline. Expect the city to unload (log the wanted list)
  and collision to be lost.
