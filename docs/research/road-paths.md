# NFSU2 road paths: the graph traffic, AI and the GPS drive on

Research notes, 2026-11-13. Game files were only read. Scripts are in `tools/paths/`.
Addresses are SPEED2.EXE (v1.2 US). Tags: **[V]** verified (how is given), **[I]** inferred,
**[?]** unknown.

## 0. Answers in short

- **The road graph is not in `PathsFreeRoam.bin`.** That file is a `0x80034147` container
  with only `0x3414A` (zones) and `0x3414D` (race barrier lines). The roads live in
  `TRACKS/ROUTES<region>/RoutesFreeRoam.bin`: chunk `0x34121` (routes = polylines of nodes
  carrying widths and lanes), `0x34122` (signposts = directed links between route nodes) and
  `0x34123` (intersections = crossings with traffic lights). In L4RA that is 855 routes,
  10,603 nodes, 184 km, 3,289 signposts and 38 intersections. **[V]** loader `0x5DEB80`; file
  names at `0x7A0B70..0x7A0C24`.
- **Traffic, AI racers and the GPS all use this one graph.** The navigation code at
  `0x40C000..0x42F000` (the GPS search object is `0x81CBC0`, started by `0x529480 → 0x413120 →
  0x40C440`) reads the route table `0x88A334`, the signposts `[0x883DB0]` and the
  intersections `[0x883DBC]`. The traffic and AI path code at `0x5D3000..0x5EA000` reads the
  same tables. **[V]** by cross-references. The BUN chunks are something else. `0x80034020`
  holds moving props (`ZCV_DrawBridgeA`...), `0x8003B200` is animation tracks ("Track 0"),
  and `0x8003B600` holds bucketed records loaded by `0x60B810`. None of them is read by the
  route code. **[V]** (no references; content checked)
- **During a race the game loads `Routes<n>F.bin` or `Routes<n>B.bin` instead**: `B` when
  the race runs backwards, `F` otherwise (`0x57F4F0..0x57F579`). In free roam it loads
  `RoutesFreeRoam.bin`. A new road only gets traffic in races if it is also added to those
  files. **[V]** code.
- **Writing is possible.** Records are reached by route id and node index, never by file
  offset. The file is read whole into a heap buffer whose size comes from the file. The game
  keeps pointers into that buffer (`0x5DE1C0`). Growing the file is fine within these limits
  **[V]** code:
  - route ids 1..1499, because the table at `0x88A334` has 1,500 slots;
  - at most 1,300 routes of kind 0;
  - at most 4,000 signposts, because the load remap at `0x5E2BD0` keeps a u16 per signpost in
    a 0x1F48-byte stack frame;
  - at most 62 intersections (6-bit field, 0x3F = none);
  - each node has 2 signpost slots.
- **Trap found while writing:** in the shipped L4RA file, 232 node slots hold signpost
  indices past the end of the table (3290..3752). The game ignores them only because they are
  ≥ the count. Append signposts without clearing them and they silently point at the new
  ones. `nfsroutes.drop_dangling_slots()` clears them.
- **Proof:** `validate_paths.py` parses all 339 `Routes*.bin` and `Paths*.bin` files of
  every region and rebuilds each **byte for byte** from the parsed fields. The free-roam
  files pass every cross-reference check. The PNGs line up with the minimap streets:
  `road-paths-L4RA-map.png`.

## 1. Files and runtime globals

| What | Where |
|---|---|
| `Tracks\Routes%s\RoutesFreeRoam.bin` | free roam (`0x7A0B70`) |
| `Tracks\Routes%s\Routes%d%s.bin` | race number, then `F` or `B` (`0x7A0B94`) |
| `Tracks\Routes%s\PathsFreeRoam.bin` / `Paths%d.bin` | path container (`0x5D9A90`) |
| route manager | `0x88A320`: `+0` list of kind-0 routes, `+8` other kinds, `+0x14` table `route*[1500]` by id (`0x88A334`), `+0x1784` active list, `+0x467C` bytes, `+0x4680` routes, `+0x4684` nodes |
| signposts | `[0x883DB0]` = chunk + 0xC, `[0x883DB4]` live count, `[0x883DB8]` file count |
| intersections | `[0x883DBC]` = chunk + 8, `[0x883DC0]` count = size / 276 |
| cross-traffic emitters `0x34124` | `[0x883DC4]`, 264 B each. No U2 file has them |

The chunks are top level and back to back: `0x34121`, `0x34122`, `0x34123`. There is no
`0x11` padding, and every record is a multiple of 4 bytes. The game needs nothing more than
4-byte alignment. **[V]** all files rebuild exactly from records alone.

## 2. `0x34121` routes: n × (0x8C header + count × 0x38 node)

### Route header (0x8C)

| Off | Type | Meaning | |
|---|---|---|---|
| 0x00 | u32 ×2 | list links, `0xB, 0xB` in the file; the loader rebuilds them | [V] `0x5D9430` |
| 0x08 | u16 | 1 | [V] constant |
| 0x0A | u16 | **route id**, the key used everywhere. Files are sorted by id | [V] `0x5DE212` |
| 0x0C | u32 | kind: 0 goes in the kind-0 list (max 1300), anything else in a second list (max 200). Always 0 | [V] `0x5D94BD` |
| 0x10 | char[32] | group name `TrackRoutesA10`..`A44`; the exe has `TrackRoutesA10..A64` and `TrackRoutesTrafficA15..A65`. Probably only the in-game route editor uses it | [I] |
| 0x30 | f32 | 1.0 | [?] constant |
| 0x34 | i16 | **node count** | [V] loader |
| 0x36 | i16 | node count again | [V] equal in all files |
| 0x38 | u32 ×2 | runtime; zeroed at load | [V] `0x5E4204` |
| 0x40 | f32 | **length** = Σ segment lengths = Σ node `+0x10`/256 − 5.0 | [V] all routes, exact |
| 0x44 | u16 ×4 | **events where the route is switched off**: race number, `0xFFFF` = none. Free roam tests 4000 (`0xFA0`). Only 4 routes use it (4087, 4111, 4121, 4122) | [V] `0x5E4210` |
| 0x4C | u8 ×8 | `0xFF`; probably 4 more event slots | [?] |
| 0x54 | f32 | 0 on 721 routes; on the other 134 it sits near the route's lowest height (6, 15, 19...), at overpasses and stacked roads | [I] |
| 0x58 | u8 ×8 | 0 | |
| 0x60 | f32 ×4 | **box** of both road edges: min x, min y, max x, max y, using node ± right × `+0x16` and node ∓ right × `+0x14` | [V] exact on all routes |
| 0x70 | u32 | flags, `0x007FE141` in the file (`0x007FE541` on 215 laneless routes; bits 2 and 3 on a few). Bit 6 = active (recomputed from `+0x44`). Bits 1, 3, 4, 7, 9, 11 and 12 are rewritten at load | [V] `0x5D9650`, `0x5E4220`, `0x5EA020`, `0x5D9882` |
| 0x74 | u32, f32, u32, f32 | `0x100, 999.0, 0x100, 999.0` | [?] constant |
| 0x84 | u8 ×8 | **lane slot present**: 1 if any node has lane slot j, slots 0..3 forward and 4..7 backward. The nearest-road query `0x40F1B0` filters on "has any lane" (`0x5D4460`) | [V] 855/855; code `0x40F5B4` |

The earlier U1 port notes call `+0x84..+0x8B` "users". It is per-lane presence.

### Node (0x38)

The direction of travel along a route is "forward" = increasing node index. Right of
travel is `(dy, −dx)` (Z up, x east, y north).

| Off | Type | Meaning | |
|---|---|---|---|
| 0x00 | f32 ×2 | x, y | [V] matches the minimap |
| 0x08 | f32 | race distance; 0 in free roam, overwritten at load | [V] `0x5E41F0` |
| 0x0C | u32 | flags. Bits 0..10 route id. Bit 11 mode 0 forward. Bit 12 set on 95% of nodes (no reader found). Bits 13..17 = 0 except 0/4/5/7/8/13 on 6% of nodes (no reader found). Bit 18 mode 0 backward. Bits 19..24 intersection index (0x3F = none). Bits 25 and 26 mode 1 forward and backward. Bits 27 and 28 mode 2 forward and backward. Bit 29 runtime race-corridor mark | [V] `0x5DD770`, `0x5D8920`, `0x5DD96A` |
| 0x10 | u16 | distance to the next node × 256; the last node holds 1280 (5.0 m) | [V] all nodes, error < 0.004 m |
| 0x12 | i16 | **height z × 8** | [V] code `0x418861` (×8192/65536); start markers match within 0.3 m |
| 0x14 | u16 | left half-width × 256 | [V] box |
| 0x16 | u16 | right half-width × 256 | [V] box |
| 0x18 | i16 ×2 | unit direction to the next node × 32767 (the last node: same as the one before, or set by hand) | [V] exact |
| 0x1C | u16 ×2 | **signpost slots**: index into `0x34122`, `0xFFFF` = empty; slot 1 always filled first. Values ≥ count are ignored | [V] 3289/3289 slots match their signpost's source; `0x5DE360` |
| 0x20 | i8 | cross slope, probably ×1/128 | [I] |
| 0x21 | i8 | slope along the road = 128·Δz/Δs to the next node | [V] all nodes within 2 units |
| 0x22 | i8 ×2 | racing line, forward and backward: lateral offset / 128 × max(half-widths). Read like lane index 0/1 | [V] read by `0x401C40` index 0/1; meaning [I] |
| 0x24 | i8 ×4 | **forward lanes**: offset = v/128 × max(`+0x14`, `+0x16`) m, positive = right of travel; `−1` = no lane | [V] `0x401C40`; edges contain 99.9% |
| 0x28 | i8 ×4 | **backward lanes**, same units, measured in the reversed direction (positive = left of the node direction) | [V] fits the left half-width |
| 0x2C | u8 ×2 | 100, 100 (racing-line speed) | [I] |
| 0x2E | u8 ×8 | **lane speed**, per lane slot: 6 = unused, 6..20 used, lower on bends (mean 18 straight → 7 at 3°/m). Probably m/s | [V] read `0x401CA0`; unit [I] |
| 0x36 | u16 | 0 (one node 0xC1) | |

Right-hand traffic is **[V]**: on divided roads, which are two one-way routes, the
opposite-direction carriageway lies on the left in 654 of 660 samples. One-way roads
(motorways, divided avenues) are separate routes with only forward lanes. A two-way street
is one route with lanes in both lists.

**Modes** (the node permission bits, signpost A/B nibbles). The car class at
`[[car+0x14]+4]` picks the mode: 1 → mode 2, 3 → mode 1, anything else → mode 0
(`0x413120`, `0x4188C0`). Mode 1 nodes coincide with lane-bearing nodes, so **mode 1 is
traffic** [I strong]. The player and GPS (class of the player car) use mode 0 [I]. Mode 2 is
another AI class [I].

## 3. `0x34122` signposts: u32 0, then n × 0x20

A signpost is a one-way jump from a node of the source route to a node of the destination
route. The planner (`0x5DE240`, `0x5DD550`) walks a route node by node. At each node it
looks at the node's slots. A signpost whose direction matches and whose type says go moves
the car to (dst route, dst node). With no accepted signpost at the route's end, the path
ends.

| Off | Type | Meaning | |
|---|---|---|---|
| 0x00 | f32 | runtime race distance and sort key (`0x5D42B0`); 0 in the file and zeroed at load | [V] `0x5D9913` |
| 0x04 | u16 | destination node index | [V] |
| 0x06 | u16 | source node index (the node whose slot holds this signpost) | [V] 3289/3289 |
| 0x08 | u16 | destination route id | [V] |
| 0x0A | u16 | source route id | [V] |
| 0x0C | u32 | Bits 0..3 **A** (modes 0 and 2) and bits 4..7 **B** (mode 1 = traffic): working copies. Bits 8..11 and 12..15: authored A and B, copied into the working nibbles at load. Bit 16/17 per-AI hint (0). Bit 18 **direction**: valid while travelling the source route forward (0) or backward (1). Bit 23 **flip**: travel direction on the destination = source direction XOR flip. Bits 24..27 navigation flags (GPS code reads 25 and 26; meaning [?]; 0 on 76%). Bit 29 runtime | [V] `0x5D98C0`, `0x5DD5EA`, `0x5DD68F`; bit 18 [V] 220/220 end signposts |
| 0x10 | i32 | −1 (0 on the 29 A=B=7 self-links) | [?] |
| 0x14 | u32 | 0 | |
| 0x18 | u16 ×4 | on A=4 signposts only (1,433 of them): route a, route b, node on a, node on b. Usually a = source route and b = destination route, about 26 m beyond the source and destination nodes. Looks like the end of a lane-change window. `0xFFFF` ×4 otherwise | [V] indices valid; meaning [I] |

Types in the nibbles, as the code treats them **[V]** `0x5DE398..0x5DE47F` and
`0x5DE5C0`:

| Value | Meaning |
|---|---|
| 2 | follow |
| 7 | never |
| 8 | follow in mode 2, never in mode 0 |
| 3 | end of route: destination = source |
| 4 | optional or lane-change link (decided at runtime) |
| 5, 10 | mark the destination route as a dead end in that direction (`0x5EA020` sets route `+0x70` bit 3/4) |

Pairs (A, B) in L4RA: 4/7 985, 2/2 886, 2/7 793 (racers and GPS only), 4/4 264, 4/10 191,
7/7 87.

The game reorders signposts at load (`0x5E2BD0`, by `+0`). It remaps the node slots in the
same pass and drops some signposts (`0x5E27D0`, `0x5D8860` lower `[0x883DB4]`). File order
does not matter. Race files keep the full free-roam signpost table, including links into
routes they leave out. The game tolerates that.

## 4. `0x34123` intersections: n × 0x114

| Off | Type | Meaning | |
|---|---|---|---|
| 0x00 | u32 | 1 (`eINTERSECTION_TRAFFIC_LIGHT`) | [I] |
| 0x04 | u16 ×3 | route ids (third = `0xFFFF`) | [V] 38/38 |
| 0x0A | u16 ×3 | node index on each route (third = 0) | [V] each named node carries this index in flag bits 19..24, and no other node does |
| 0x10 | i32 | −1 | [?] |
| 0x14 | 16 × {u32 0, i32 −1, f32 −1, f32 −1} | runtime light state | [I] |

Intersections are optional. L4RA has 38 of the 62 possible. Race files carry the full table.

## 5. `PathsFreeRoam.bin` (and `Paths<n>.bin`)

This is the `0x80034147` container, loaded by `0x5D9A90`. It is **not** used for road
following.

- `0x3414A` zones: variable length (0x44 + 8 × corners), as in `docs/NFSU2-FORMATS.md`.
  The layout is confirmed end to end, and every zone's box equals the box of its corners.
  Types (`0x800D80`) in L4RA:

  | Type | Count |
  |---|---|
  | RESET_TO_POINT | 1 |
  | GUIDED_RESET | 24 |
  | TUNNEL | 39 |
  | OVERPASS | 79 |
  | OVERPASS_SMALL | 42 |
  | SHORTCUT | 4 |
  | STREAMER_PREDICTION | 131 |
  | RACE_START | 196 |
  | SHOP | 23 |
  | **TRAFFIC_TYPE** | 4 |
  | NEIGHBOURHOOD | 13 |
  | SMS | 56 |
  | BANK | 59 |

  The four TRAFFIC_TYPE zones are large areas with parameters at `+0x30` (e.g. 0, 3, 6, 2).
  A new road inside them gets that area's traffic setting [I].
- `0x3414D` barrier lines, 36 B: {f32 x0, y0, x1, y1; u8 0; u8 count 1..4; u8 flag (1, 0 on
  45); u8 0; u32 group-hash[4]}. The hashes are barrier group names, the same as L4RA.BUN
  `0x3412F`. L4RA has 1,788 lines and 125 groups. These are the race-only walls that AI
  steers around. **[V]** layout, byte-exact rebuild.
- Race files only: `0x34148` race-path nodes (24 B), `0x34149` path segments (220 B),
  `0x3414C` race outlines (272 B). They give the race distance (`+0x08` of route nodes, signpost
  `+0`), not road topology. Byte-accounted as opaque records.

## 6. What the game recomputes at load (so the file need not be exact)

The post-load sequence is `0x5E9E80 → 0x5E9F2C..0x5EA0CF`. Each step **[V]**:

1. Signposts: A/B ← authored copy; `+0 = 0`.
2. Routes: `+0x70` bit 6 from the event list `+0x44`; `+0x38`, `+0x3C` and node `+0x08`
   zeroed.
3. Node permission bits are **ORed in** by a flood from the signposts (`0x5DE5C0 →
   0x5DE240`). Bits in the file are never cleared, except bits 11/18/27/28 on routes with
   `+0x70` bit 9, which none have.
4. Signposts are reordered by `+0` with the node slots remapped; dropped ones are cut from
   the count.
5. Type 5/10 signposts set dead-end bits on the destination route; route `+0x70` bits for
   region overlap are refreshed.

Lengths, directions, slopes, box and lane flags are **not** recomputed. They must be right
in the file, and `finish_route()` computes them.

## 7. How to write it

Use `tools/paths/nfsroutes.py`. Each step below names the helper that does it.

### New road

1. **Load** the region's `RoutesFreeRoam.bin` (`Network.load`). Serve the result through
   the loader overlay; never write the game folder.
2. **Pick an id**: `free_route_id()`, the smallest unused id (≥ 101, ≤ 1499). Insert the
   route so ids stay sorted (`add_route`).
3. **Build the nodes** (`make_route(id, points, half_left, half_right, fwd_lanes,
   back_lanes)`):
   - Points every 5..50 m along the centre line, at road height. 13..50 m is typical; put
     denser points on bends.
   - Half-widths from curb to curb.
   - Up to 4 lane offsets per direction, in metres, positive = right of that direction's
     travel. For a two-lane two-way road with 12 m width: `fwd_lanes=[3], back_lanes=[3]`.
   - The node flags it writes: route id | bit 12 | intersection 0x3F | all six permission
     bits. Leave out bits 25/26 if the road should have no traffic.
   - Fields it fills: slots `0xFFFF`, speeds `[100, 100]`, lane speed ~20 (less on bends),
     6 for unused lane slots.
   - Header: constants from `default_route_header()`, plus length, box and lane-present
     flags.
   - One-way road: lanes in one list only.
4. **Connect both ends.** A route end without a signpost stops every path there.
   - At a T junction with an existing road (`connect_end_to_route`), the existing road needs
     a node at the junction with **two free slots**. `junction_node()` reuses such a node
     within 4 m, or splits the nearest segment.
   - Four signposts are written:
     - On the existing node, one per direction of travel on it, into the new road's end,
       with flip set so the car drives into the new road.
     - On the new end node, two back out, one for each direction on the existing road.
   - Use `A = 2`. Use `B = 2` if traffic may take the link, `B = 7` if not. Authored nibbles
     equal the working ones; `signpost_flags()` builds the word.
   - A straight continuation of an existing route end works the same way. Link its last
     node to the new node 0 (direction 0, flip 0), and add the reverse link from new node 0
     (direction 1) back to the old end (flip 0 if the old route continues backwards).
5. **Before appending any signpost**, clear dangling slot values (`drop_dangling_slots`;
   `add_signpost` does it).
6. **Optional**: an intersection record if the crossing should have traffic lights. Append
   a 0x114 record naming the two (route, node) pairs, and set bits 19..24 of those two nodes
   to its index.
7. **Rebuild** with `Network.to_bytes()`. The three chunk headers carry the new sizes and
   nothing else needs patching: no offsets, counts or grids outside the chunks. Run
   `Network.check()`.
8. For race traffic, repeat in `Routes<n>F.bin` and `Routes<n>B.bin`. Optional.

### Splitting an existing edge

Use `insert_node(net, route, index, x, y)`, which does all of the following:

- Inserts a node and copies width, lanes and flags from the previous node.
- Interpolates the height.
- Increments the node count (`+0x34` and `+0x36`) and recomputes lengths, directions,
  slopes, box and route length.
- Renumbers every reference to that route's later nodes: signpost source and destination
  node, the A=4 extra nodes, and intersection nodes.
- Changes no byte offset anywhere, because everything is by (route id, node index).

### Checks before shipping a file

- Every slot points at a signpost whose source is that (route, node).
- No node has more than 2 signposts.
- Signposts ≤ 4000, route ids < 1500, intersections < 63.
- Last node length 1280; header length = Σ.
- Lane-present flags match the nodes.

`Network.check()` tests all of these. `demo_add_road.py` builds a 125 m two-way road between
two L4RA streets. It splits both, adds 8 signposts and grows the file by 844 bytes. It passes
`check()` and re-parses identically. A simulation with the game's walking rules reaches the
new road from both directions of the first street and leaves it onto both directions of the
second (`road-paths-demo-junction.png`). **Not yet tried in game.**

## 8. Open points (ranked for an in-game test)

1. In game: traffic entering the demo road, and the GPS routing through it (set a GPS
   target past it).
2. Node flag bits 12..17 and signpost bits 24..27: no reader found in the route code. The
   writer copies the common values (bit 12 = 1, others 0).
3. A=4 links and the `+0x18` extras (lane-change windows?). The writer never makes type 4.
4. Lane speed unit (m/s assumed) and the `+0x20` cross slope; header `+0x30`, `+0x54`,
   `+0x74..+0x83`.
5. Whether the load-time pruning (`0x5E2130` loop, `0x5E15F0`) turns links into a new road
   into type 7 if it has a dead end. Avoid dead ends.

## 9. Scripts

| Script | Use |
|---|---|
| `tools/paths/nfsroutes.py` | Routes parser and writer: Node/Route/Signpost/Intersection, `check()`, `make_route`, `add_signpost`, `insert_node`, `junction_node`, `connect_end_to_route` |
| `tools/paths/nfspaths.py` | Paths container parser (zones, barriers, race chunks), byte-exact rebuild |
| `tools/paths/validate_paths.py` | parses and rebuilds all 339 files; `-v` prints the free-roam summaries |
| `tools/paths/render_paths.py` | PNGs: whole graph, `--map` over TRACKMAP4000, `--zoom ... --lanes` |
| `tools/paths/demo_add_road.py` | adds a test road with two split junctions into an output folder |

Images:

- `road-paths-L4RA.png` (graph)
- `road-paths-L4RA-map.png` (over the minimap; blue = lanes both ways, orange = one way,
  grey = no lanes, green and purple = signposts, red = intersections)
- `road-paths-L4RA-lanes.png` (lanes and edges, downtown)
- `road-paths-demo-junction.png` (the demo road in yellow)
