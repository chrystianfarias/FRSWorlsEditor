# NFS Underground 2 track file formats

Everything here was derived by reading the shipped files in `TRACKS/` and
checking each rule against all 118 MB of `STREAML4RA.BUN` (1145 solid packs,
10 735 objects, 1836 textures) plus spot checks on the other regions. Offsets
are hexadecimal and byte offsets from the start of the structure. Fields still
unidentified are marked as such rather than guessed at.

## 1. Container format (BChunk)

Every asset file is a flat sequence of chunks:

```
struct ChunkHeader {
    u32 id;        // high bit set => the payload is a list of child chunks
    u32 size;      // payload size in bytes, not counting this 8-byte header
};
```

Two details make or break a parser:

**Alignment filler.** A payload may be preceded by a run of `0x11` bytes that
pushes the real data onto an alignment boundary. The boundary depends on the
chunk:

| Chunk | Alignment |
| --- | --- |
| `0x00134B01` (vertex buffer) | 128 |
| `0x33320002` (texture pixel data) | 128 |
| `0x00134012` (texture references) | 8 |
| everything else | 16 |

Getting one of these wrong is silent. The pixel block carries 120 bytes of
filler in every one of a region's 206 packs, and reading it 120 bytes early
displaces every texture in the game by a fraction of a row — the result still
looks like texture, so it survives a glance, while nothing in the world lines
up. The check that settles it: after skipping the filler, the texture slices
fill the payload exactly, with zero bytes spare, in all 206 packs.

The filler is only written when the writer needed it. Some chunks sit on an
unaligned offset and begin with real data that happens to be zero, so a parser
must check that the bytes it is about to skip really are `0x11` and skip nothing
otherwise. Skipping blindly corrupts the texture tables; skipping every `0x11`
it finds eats the first byte of four vertex buffers whose data legitimately
starts with `0x11`. The alignment is computed from the **absolute file offset**,
so nested readers have to track where their buffer starts.

**Zero gaps.** Streaming sections are padded out to a 2 KiB boundary with zeros.
A top-level reader has to walk past those runs instead of treating `id = 0,
size = 0` as a chunk.

## 2. Region layout — `L4Rx.BUN`

| Chunk | Contents |
| --- | --- |
| `0x00034112` | region bounds |
| `0x80034150` | visibility / preculler data |
| `0x00034110` | **streaming section table** |
| `0x80034020`, `0x8003B200`, `0x8003B600` | AI paths, traffic, events |

### Section table entry — 80 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `char[4]` | section tag, ASCII, zero padded (`"C29"`, `"G26"`, `"R99"`) |
| `0x08` | `u32` | section number |
| `0x14` | `u32` | offset into `STREAML4Rx.BUN`, always 2 KiB aligned |
| `0x18` | `u32` | total size in bytes |
| `0x1C` | `u32` | size excluding the texture pack; equal to `0x18` when there is none |
| `0x20` | `u32` | streaming order key |
| `0x24` | `float[3]` | section centre in world space |
| `0x30` | `u32` | hash |

`L4RA` has 393 entries. The first few describe shared data — global textures,
the backdrop, common props — and carry a zero position; the rest are the
positioned world blocks. The centre at `0x24` agrees with the geometry: section
`A30` is listed at `(-1225, 2557, 304)` and its terrain spans
`(-1377..-1065, 2343..2777, 232..360)`.

## 3. Geometry — `0x80134000` solid list

```
0x80134000  SolidListPack
├── 0x80134001  header
│   ├── 0x00134002  pack path at +0x10 ("Location4\RegionA\Pipeline\PC\...")
│   └── 0x00134003  (hash, offset) lookup pairs
└── 0x80134010  SolidObject                       (repeated)
    ├── 0x00134011  object header
    ├── 0x00134012  texture references
    └── 0x80134100  mesh
        ├── 0x00134900  descriptor
        ├── 0x00134B01  vertex buffer
        ├── 0x00134B02  material table
        └── 0x00134B03  index buffer
```

### Object header — `0x00134011`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x0C` | `u32` | version (`0x16` throughout) |
| `0x10` | `u32` | object hash |
| `0x14` | `u32` | triangle count |
| `0x20` | `float[3]` | bounds minimum |
| `0x30` | `float[3]` | bounds maximum |
| `0x40` | `float[16]` | 4x4 world matrix, row major, translation in row 3 |
| `0xA4` | `char[]` | zero-terminated name, runs to the end of the chunk |

Objects that the game never needs bounds for store the **empty-box sentinel**:
minimum `+FLT_MAX`, maximum `-FLT_MAX`. Treat `min > max` as "no bounds" or the
box explodes to the float range.

The name prefix is the artists' classification and is worth honouring:

| Prefix | Meaning | Transform |
| --- | --- | --- |
| `TRN_` | roads and terrain of a section | identity; vertices already in world space |
| `PAN_`, `SKYDOME` | painted backdrop | placed, but 8 000 to 19 000 units across |
| everything else | buildings, signs, street furniture | placed by the matrix |

Of the objects inside positioned sections, 4349 are `TRN_` with an identity
translation and 2650 are props with a real one.

### Descriptor — `0x00134900`

| Offset | Meaning |
| --- | --- |
| `0x10` | material count |
| `0x24` | triangle count |
| `0x34` | vertex count |

### Vertex buffer — `0x00134B01`

Stride is `payloadSize / vertexCount`. Only two layouts exist across every
shipped region:

| Stride | Layout |
| --- | --- |
| 24 | `float3 position`, `u32 colour (ARGB)`, `float2 uv` |
| 36 | `float3 position`, `float3 normal`, `u32 colour (ARGB)`, `float2 uv` |

10 585 objects use the 24-byte layout and 115 the 36-byte one. The colour is the
baked night lighting; the original renderer combines it with the texture at 2x
(`D3DTOP_MODULATE2X`), and plain modulation leaves the city almost black.

### Material table — `0x00134B02` — 60 bytes per entry

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `float[3]` | bounds minimum |
| `0x0C` | `u32` | index count |
| `0x10` | `float[3]` | bounds maximum |
| `0x1C` | `u32` | index into the object's texture reference table |
| `0x20` | `u32` | flags, always `0xFF` in the shipped data |
| `0x34` | `u32` | first index |

The ranges tile the index buffer contiguously: entry *n* starts where entry
*n-1* ended. The texture index is in range for all 28 018 materials of `L4RA`.

### Index buffer — `0x00134B03`

`u16` triangle list, `triangleCount * 3` entries. The chunk is padded past that,
so trust the count rather than the byte size.

**Winding.** The meshes are wound for Direct3D's default left-handed convention,
where back faces are the counter-clockwise ones. A renderer that uses
right-handed view and projection matrices — as this editor does, to keep the
game's Z-up world without flipping an axis — inverts the parity a triangle ends
up with on screen, and has to cull clockwise instead (`D3DCULL_CW`). Getting
this backwards culls the front faces: the city still draws, but every surface is
inside out and it reads as a scatter of loose shells rather than solid buildings.

### Texture references — `0x00134012`

Eight bytes per entry: a `u32` texture hash followed by four reserved bytes.

## 4. Textures — `0xB3300000` texture pack

```
0xB3300000  TexturePack
├── 0xB3310000  info
│   ├── 0x33310001  pack name and path
│   ├── 0x33310002  hash table, 8 bytes per texture, sorted ascending
│   ├── 0x33310004  texture headers, 124 bytes each
│   └── 0x33310005  format table, 32 bytes each
└── 0xB3320000  data
    └── 0x33320002  pixel data for every texture, mip levels largest first
```

### Texture header — `0x33310004` — 124 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x0C` | `char[20]` | name |
| `0x24` | `u32` | hash, the value materials reference |
| `0x28` | `u32` | pack id, identical for every texture in a pack |
| `0x30` | `u32` | offset into the pixel data |
| `0x38` | `u32` | byte size covering every mip level |
| `0x40` | `u32` | byte size of mip 0 |
| `0x44` | `u16` | width |
| `0x46` | `u16` | height |
| `0x48` | `u8` | log2 width |
| `0x49` | `u8` | log2 height |
| `0x4E` | `u8` | mip level count |

The hash is EA's string hash. Names that differ only in their last characters
produce hashes that differ by the same amount, which is what confirms the field:
`ARC_WHOUSE_DOOR_01/02/04/05` hash to `0x52DF42/43/45/46`.

### Format table — `0x33310005` — 32 bytes

`0x14` holds a `D3DFORMAT`. In `L4RA` that is `"DXT1"` for 1417 textures,
`"DXT3"` for 171 and `41` (`D3DFMT_P8`, 8-bit palettised) for 23.

### Palettised textures

Only 23 textures in a region are palettised, which makes them easy to write off
— but they are `RDP_LANEA`, `RDP_INTERSECTMIDTILE`, `ARC_SIDEWALK_E` and the
rest of the road and pavement set, together 3986 of the 27 987 material slots.
Skipping them leaves every street in the city untextured.

Two header fields are zero for DXT textures and filled in for these:

| Offset | Meaning |
| --- | --- |
| `0x34` | palette offset, addressed like the pixel offset at `0x30` |
| `0x3C` | palette size, always 1024 = 256 entries of 4 bytes |

The palettes sit in the gap the pixel block leaves before its first texture
slice — 21 504 bytes in `L4RA`, enough for all of them, packed one after
another from offset zero.

Each entry is a `D3DCOLOR`, so its low three bytes go straight into an
A8R8G8B8 surface. The fourth byte is **not** opacity: it holds a noisy gloss
pattern the game uses for wet road reflections, ranging over most of 0..255
with no bimodal structure. Treating it as alpha leaves every road nearly
invisible; road surfaces are opaque.

## 5. Scenery — `0x80034100`

This is where most of the city lives. L4RA holds 10 704 solid objects but
**48 270 scenery instances**: the trees, signs, railings, street furniture and
repeated buildings are each stored once and placed many times.

```
0x80034100  Scenery
├── 0x00034101  header
├── 0x00034102  SceneryInfo   -- 68 bytes, the models this section can place
├── 0x00034103  SceneryInstance -- 64 bytes, one placement each
├── 0x00034104  groups
└── 0x00034106  overrides
```

### SceneryInfo — 68 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `char[32]` | model name |
| `0x20` | `u32` | hash of the SolidObject to place |
| `0x24` | `u32` | the same hash again |
| `0x3C` | `float` | radius |

Every one of the 48 hashes in a sample section resolves to a solid object
somewhere in the region.

### SceneryInstance — 64 bytes

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `float[3]` | world bounds minimum |
| `0x0C` | `float[3]` | world bounds maximum |
| `0x18` | `u16` | index into the SceneryInfo table |
| `0x1A` | `u16` | flags |
| `0x20` | `float[3]` | translation |
| `0x2C` | `i16[9]` | 3x3 matrix, row major, fixed point with 1.0 = 0x2000 |

Scale is baked into the 3x3 matrix, so its rows are only unit length for
instances drawn at 1:1. A row of `(5843, -5742, 0)` has length 8192.1, and where
a row measures 6863 the instance is drawn at 6863/8192 = 0.838 scale, which is
exactly the ratio between its stored bounds and the model's.

The transform is `rotation * scale` from `0x2C` with the translation from
`0x20`, applied to the model's own local geometry. The model's header matrix is
**not** composed with it. Rebuilding each instance's world bounds this way
reproduces the stored box with a median centre error of 0.03 units across all
44 158 instances that resolve; composing the header matrix instead gives a
median error of 1571 units.

### The rest of the chunk, and adding placements

Measured over the 606 scenery chunks of L4RA (and the 1986 of all regions for
the writer's round trip):

- **Header `0x00034101`**, 60 bytes: all zero except the section number at
  `0x0C`. A section usually carries two chunks: one with its own number and a
  larger one numbered 20 lower (A21 = 121 also holds 101), which is not a
  section in the table.
- **Only `0x00034103` is aligned**: its records start on a 16-byte file
  boundary (12 bytes of `0x11` in the shipped files). Every other child
  starts straight after its header — one tree even begins with a real `0x11`
  byte, which a filler check would eat.
- **SceneryInfo `0x28`** is zero; `0x2C` and `0x2E` are u16 counts, `0x2C`
  equal to the model's triangle count for 74% of entries (the rest look like a
  second level of detail). The same model's entry differs between sections in
  942 of 6406 cases, so a new entry is best copied from one the game ships.
- **SceneryInstance `0x1C`** is a u32, 1-based index of a 128-byte block in
  `0x00034106` (0 = none); `0x3E` is always 0. The blocks are 32x32-bit masks
  whose meaning is still open; block 0 is all zeros.
- **`0x00034104` is a bounding-volume tree** over the records. 36-byte nodes:
  `float3 min, float3 max, u16 count, u16 child[5]`. A child below `0x8000`
  is a record index; above, it is node `0x10000 - child`. Node 0 is the root.
  Every record appears once, every other node is referenced once, and each
  box is the union of its children's (605 of 606 packs; the last one only
  looked otherwise to a parser that stripped its leading `0x11`). The shipped
  trees are minimal 5-ary trees, `ceil((n - 1) / 4)` nodes.
- **Models placed are local or library, never foreign.** Of 12 858 model
  entries, 7095 name a model stored in a library section (X/Y/Z, no world
  position, always loaded) and 5763 one stored in the same section. None
  names a model that lives in another positioned section — the game would
  only have it while that section happened to be streamed.

The editor adds placements (`nfs/SceneryEdit.cpp`) by appending records after
the shipped ones, so no index held elsewhere moves, and inserting each into
the shipped tree with the smallest change: into the first node on the way
down with a free slot, else down the child node that grows least, else a new
node pairing the new record with its nearest sibling. Bounds grow along the
path. Re-encoding a pack with nothing added reproduces it byte for byte; with
additions the result passes the same tree checks (`ValidateScenery`, run
before every save). Saves always start from the shipped chunk, so records an
earlier save appended are written again from the editor's list rather than
stacked, and the section is spliced through the same 128-byte-preserving path
as the collision import.

### Edits as a list, sections rebuilt from the shipped bytes

A span holds a whole section with every edit applied, which is what the game
needs but not what a person wants to undo. The edits are recovered by
comparing the section the editor shows with the shipped one
(`io/SectionEdits.cpp`), chunk by chunk and matched by id and order: in a solid
list, an object whose bytes changed is an edited mesh and one the shipped list
lacks was copied in; in a texture pack, textures added or changed; a changed
collision pack; scenery records past the shipped count are placements. Every
save -- placements, collision import, mesh import -- and every revert rebuilds
the section **from the shipped bytes** with the edits that are to remain, each
laid on the file position modulo 128 it was made for. So no kind of edit can
undo another, and a section left with none goes back to the shipped file
(its spans are removed). Checked on a Q38 carrying 15 edits: rebuilding with
all of them gives the same 15 back, with none gives the shipped bytes, and
each of the 15 left out in turn gives exactly the other 14.

A mesh import brings one rebuild per mesh, each from the shipped section with
that mesh alone, so the meshes of a section are gathered before the section is
written once. Every mesh is rebuilt from the OBJ whole -- faces, UVs, colours and
materials. There used to be a shortcut for a mesh whose vertex count had not
changed: only its positions were written back, as a `.frsmesh` overlay at the
vertices' shipped file position. A matching count says nothing about order
(Blender lists vertices by its `v` lines, a rebuilt mesh stores them by face
corner), so a mesh whose count happened to come out equal got its positions on
the wrong vertices and kept its old materials; and the overlay missed the game
once the section was served from a span elsewhere. Existing `.frsmesh`
overlays of a mesh are removed when it is imported again.

**Game textures on import.** A TGA is the format the export writes the game's
textures in, so on import a TGA -- beside the OBJ or painted by a material --
whose name matches a texture of the region is that texture, not a new one: it
is referenced by hash, and copied as it is (compressed levels untouched) into
the section's pack only if the section cannot already see it (it is neither in
a library pack nor in the section's own). A `tex_<hash>` material resolves the
same way. Any other image format is a new texture and goes through the DXT
compressor. Windows' imaging component has no TGA decoder, so TGAs are read by
the editor itself; before that, a TGA beside the OBJ made the import of every
mesh that went through a rebuild fail.

### Taking a model to another section

A solid list's header says how many objects it holds — u32 at `0x0C` of
`0x00134002` — and lists them in `0x00134003` as `(hash, 0)` pairs sorted by
hash (1142 of 1145 packs; the other three have extra pairs). Every pack has a
zero-id padding chunk between the header and the first object.

To place a model stored in another positioned section, the editor copies its
`0x80134010` chunk byte for byte into the target's first solid list, on the
same file position modulo 128 as the original (every alignment inside an
object divides 128), with the count raised and the pair inserted in order;
the padding after the header is remade so the shipped objects keep their
positions modulo 128. Textures the model uses that are neither in a library
pack nor in the target's own are copied into the target's pack as they are,
mips included. Example: `XB_OILTANK_1B_BS_00` lives in H24 and H29 (a
never-instanced model on the storage grid, 180 triangles); three of its four
textures are in Y0, the fourth (`9412C4BF`, 128x128 DXT1) only in H24/H29.

### Placements, and drawing each one once

Every solid object also carries a real world placement in its own header matrix
— none of them are parked at the origin — and scenery instances add further
ones. Two traps follow:

- A region stores some meshes twice at the same spot. In L4RA 1794 hashes appear
  on two objects, at identical positions and with identical triangle counts;
  section X50 is a verbatim partial copy of X0. Drawing both makes every shared
  surface z-fight, which looks like the city breaking into flickering fragments.
- A few hundred instances repeat a placement that an object's header matrix
  already covers — 532 of the terrain instances, for example, are simply the
  section's own roads listed again with an identity transform.

The rule that handles both: **draw each (model, transform) pair exactly once.**
Suppressing an object because something instances it is wrong and costs about
2500 correctly placed meshes, roads among them.

## 5a. Collision — `0x80034130`

The car does not drive on the render mesh. Each positioned streaming section
carries one collision pack of its own: simplified ground polygons plus barrier
lines, in 16-bit fixed point, bucketed on a 32-unit grid. Every rule below was
checked against every pack of all eight regions (L4RA: 255 packs, 153 625
polygons, 21 699 barriers) with zero exceptions.

```
0x80034130  collision pack
├── 0x00034131  header, 48 bytes
├── 0x00034134  polygons, 32 bytes each
├── 0x00034136  cell references, 4 bytes each
└── 0x00034137  grid cells, 4 bytes each, gridWidth * gridHeight of them
```

### Header — `0x00034131`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `u32` | section number, equal to the table's `0x08` |
| `0x04` | `u16` | grid width (cells along x) |
| `0x06` | `u16` | grid height (cells along y) |
| `0x08` | `float[2]` | grid origin, a multiple of 32 |
| `0x10` | `float[3]` + pad | bounds minimum |
| `0x20` | `float[3]` + pad | bounds maximum |

The bounds include the barriers, so their z runs to `-127` / `+127`
regardless of the real terrain.

### Polygon — `0x00034134`

| Offset | Type | Meaning |
| --- | --- | --- |
| `0x00` | `u8` | **race barrier group**, 0 = always present (below) |
| `0x01` | `u8` | **extreme-corner code** (below) — not a surface id |
| `0x02` | `u8` | **surface** (below): 1 `ROAD` on most ground, `0x14` `SOLID_WALL` on most barriers |
| `0x03` | `u8` | flags: `0x10` quad, `0x02` vertical (see below); barriers are always `0x16`; `0x01 0x08 0x40` unknown |
| `0x04` | `i16` | height base, whole units |
| `0x06` | `u16` | always `0xFFFF` |
| `0x08` | `i16[4]` | x of each corner, in eighths of a unit |
| `0x10` | `i16[4]` | y of each corner, in eighths of a unit |
| `0x18` | `i16[4]` | z of each corner, in 1/256 of a unit, added to the base |

So `x = X / 8`, `y = Y / 8`, `z = base + Z / 256`. The height is finer than the
plan position on purpose: the car rests on it. Checked against the render
mesh of section A21, every ground corner lands within 0.17 of a road vertex in
plan (the 1/8 quantisation) and within 0.02 of its height (median 0.002).

A **triangle** repeats its third corner in the fourth slot and has flag `0x10`
clear; a **quad** has four distinct corners in perimeter order and splits into
`(0,1,2)` and `(0,2,3)`.

A **barrier** is recognised by its z values `-32512, +32512, +32512, -32512`
with a zero base: a wall from corner 0 to corner 2 (corners 1 and 3 repeat
them) that is infinitely tall for any practical purpose. The file gives it no
real height, so the editor stands it on the nearest ground corner.

**Extreme-corner code (byte 1).** Two bits each name the corner (0-3) lying
furthest toward -x (bits 0-1), -y (2-3), +x (4-5) and +y (6-7), in the stored
eighths; a tie goes to the later corner. A triangle repeats its third corner,
so corner 3 wins its ties. It is a bounding box the game tests a car against
before the polygon itself, and it holds for every polygon of the eight regions
(about 432 000: ground, vertical faces and barriers). A polygon imported with
a code copied from another -- the material name used to carry it -- fails that
test wherever the two boxes disagree: in the game that was holes in new ground
that dropped the car (and reset it to the road), and walls it drove through.
The encoder computes it.

For a barrier, corners (a, a, b, b), the same rule gives the four heading codes
first found for walls:

**Barrier direction code.** Byte 1 of a barrier is computed from its segment
(corner 1 to corner 2, in the stored eighths): start from `0xFF` and clear bit 1
if dx > 0, bit 3 if dy > 0, bit 5 if dx < 0, bit 7 if dy < 0. So `F5` runs
0–90°, `D7` 90–180°, `5F` 180–270°, `7D` 270–360°, and `FD F7 DF 7F` are exactly
along an axis. It holds for all 51 202 barriers of the eight regions. The game
ignores a wall whose code disagrees with its heading: two side walls imported
with the `F5` of the wall they were copied from let the car through while the
walls that really ran 0–90° held. The encoder now writes the code itself.

**Barriers are one-sided.** In L4RA the road lies to the left of the segment's
direction (corner 0 to corner 2) for 13 868 barriers and to the right for 315
(the rest have ground on both sides or none). A wall imported facing the
wrong way let cars straight through in the game, so the importer writes every
barrier it reads in both directions.

**Vertical faces.** Every polygon with flags `0x12` — 17 788 of them in L4RA,
and no other — has zero area in plan: it is a wall with a real height (a kerb,
a building side), stored as an ordinary quad with its own z values. Barriers
(`0x16`) are the same thing with `0x04` added and the height made infinite.

**Race barrier group (byte 0).** Non-zero on 12 049 L4RA polygons: an index
into the region's `0x3412F` list (126 records of 32 bytes `{hash, count, 1,
name}`, names `BARRIERS_<race>` and `PLAYER_BARRIERS_<race>`; the counts match
the polygons carrying each index). The game switches a group on only in the
races that name it, so in free roam those polygons do not exist -- 97% of them
stand on an `XO_TRACKBARRIER*` placement. Anything the editor creates for free
roam must carry 0. (Found converting Bayview to Underground 1, where nothing
switched them off and they became invisible walls everywhere.)

**Surface (byte 2).** An index into SPEED2's surface table at `0x800EE0`
(`0x98` bytes per entry, name pointer first): 0 `NONE`, 1 `ROAD`, 2
`ROAD_WET`, 3 `ROAD_DRIFT`, 4..6 `ROAD_SMOKE_1..3`, 7 `ROAD_BRIDGE`, 8 `DIRT`,
9 `GRAVEL`, 10 `ROUGH_ROAD`, 11 `COBBLESTONE`, 12 `STAIRS`, 13 `PUDDLE`, 14
`DEEP_WATER`, 15 `GRASS`, 16 `SIDEWALK`, 17 `WOOD`, 18 `PLASTIC`, 19 `GLASS`,
20 `SOLID_WALL`, 21 `SEE_THROUGH_WALL`, 22 `PLANT`, 23 `POST`, 24 `PILLAR`, 25
`METAL_GRATE`, 26 `METAL`, 27 `CHAINLINK`, 28 `CAR`. Prop classes (`0x34026
+0xD4`) index the same table.

**Polygons repeated across packs.** 21 834 polygon shapes of L4RA are stored
in two or more packs (446 section pairs, mostly neighbours: the polygons along
a section edge). An edit that removes or moves such a polygon in one pack
leaves the other copy colliding; new geometry near an edge needs to go into
every pack the car may be using there.

### Export for editing

With the collision shown (Ctrl+K) a click selects collision polygons instead of
meshes: Ctrl-click adds or removes one, Shift-click takes the section's whole
pack. Ctrl+E then writes them as OBJ (`io/CollisionExport.cpp`): one object per
pack named `collision_<section>`, one face per polygon in file order, corners
welded where they coincide, Y-up and moved by the manifest's `origin` like the
mesh export. The material name carries bytes 0–3 of the record
(`ground_00_34_01_00`, `barrier_00_5F_14_16`) so a face keeps its attributes
through Blender; barriers are written as the viewport's wall quads, of which
only the bottom edge means anything. The JSON manifest lists, per object, the
pack's polygon indices in face order. Reading an exported Q38 (1560 polygons)
back and comparing with the file gave identical attributes and shapes and a
largest position error of 0.00008.

### Grid — `0x00034137` and `0x00034136`

Each cell is `(u16 first, u16 count)` into the reference list, row by row
(`index = cy * width + cx`), the lists contiguous. Each reference is
`(u16 polygon index, u16 mask)`; bit `sy * 4 + sx` of the mask is the 8-unit
sub-cell `(sx, sy)` of that cell.

The whole pack can be rebuilt from the polygons alone. These rules reproduce
every non-empty pack of all eight regions **byte for byte** (`EncodeCollision`,
checked with `FRS_COLLISION_SELFTEST=1`):

- **Header bounds** are the min/max of the stored corners, barriers counting
  as z = ±127.
- **Grid origin** is `floor(min / 32) * 32`; the size is
  `floor(max / 32) - floor(min / 32) + 1` cells on each axis.
- **Height base** of a ground polygon is `floor((zmin + zmax) / 2)` over its
  corners. (Not the floor of the lowest corner: that fails on 5% of them.)
- **Cell membership** is a polygon-against-square overlap in plan (separating
  axis test, polygon outline as stored, barriers and vertical faces as
  segments). A cell owns its low edges and not its high ones: a polygon whose
  lowest x or y equals the cell's far boundary belongs to the next cell only.
  Touching counts as overlap, with a tolerance of 1e-4 — without it float
  noise drops the polygons that meet a grid line exactly.
- **Mask**: inside a member cell each sub-cell is tested the same way, closed
  on all sides.
- **Order within a cell** is not file order. The tool lists polygons grouped
  (ground, vertical faces, barriers — by some source order the file does not
  record), but every pack is consistent with one global order, which a
  topological sort of the consecutive pairs in the cells recovers.
- **An empty pack** is still written: a 1x1 grid at the origin, bounds
  `+FLT_MAX` / `-FLT_MAX`, no polygons and one empty cell (84 bytes).

### Import

`io/CollisionImport.cpp` reads an edited export back (Ctrl+I recognises it by
its `collision_<section>` object names). Faces replace the exported polygons in
their slots, extra ones are appended, the pack is re-encoded, and the section
is rebuilt by splicing the new chunk in place of the old one, followed by a
zero-id padding chunk that keeps everything behind it on its 128-byte
alignment. It starts from the bytes the editor shows — a span from an earlier
mesh import included — and replaces that span file rather than stacking a
second one on the same section.

Face to polygon: the material name gives the attribute bytes; the quad bit
(0x10) and the vertical bit (0x02, zero plan area) are set from the shape; a
quad with a reflex corner in plan is cut in two through it; larger faces are
fanned into triangles. A barrier face becomes the segment its bottom edge
spans, directed by `normal x up` so the wall keeps the side it faced.

### Editing in the editor

`io/CollisionEdit.*` makes and places polygons; `ui/CollisionTools.*` is the
Collision menu over it. What they rely on:

- **Making.** Ground is a triangle or a quad convex in plan (an outline of
  more points is ear-clipped into triangles); a barrier is the two-point form
  above; a wall with height is a quad stored like the shipped `0x12` faces --
  (A foot, A top, B top, B foot), corners 0/1 and 2/3 sharing their plan
  position -- with its foot sunk 0.5 below the drawn points. Walls of either
  kind are written in both directions and cut into pieces of at most 8 units.
  New polygons carry race group 0 and the surface chosen (default `ROAD` for
  ground, `SOLID_WALL` for walls).
- **Placing.** A polygon goes into every pack that has ground under one of its
  corners or its middle -- the same rule that makes the shipped edge copies --
  and, over no ground at all, into the nearest pack.
- **Copies.** Two polygons are the same when their stored shape is (corners in
  eighths, heights in 1/256, barrier or not); delete, move, surface and race
  group changes find and change every copy.
- **From an object.** `io/CollisionGenerate.*` reads the placed mesh. A ramp or
  a deck -- an upward face that is not level and reaches down to the ground,
  or anything under 0.6 tall -- becomes ground on its upward faces. Anything
  else is solid: walls with height round the convex hull, in plan, of the
  vertices up to 2.5 above its foot (a tree's trunk, not its crown), plus
  ground on upward faces higher up so a car can land on a roof. Which way a
  face points is voted from the stored vertex normals, or from the highest
  faces when a model has none. A polygon a pack already holds is not added
  again, so making it twice changes nothing.
- **Saving.** Edited packs live in the loaded sections until Ctrl+Shift+S,
  which re-encodes each changed pack (`EncodeCollision`) as the section's
  collision edit, next to its placements, through the same rebuild as every
  other edit. `FRS_COLLISION_TOOLS_SELFTEST=1` runs the whole path on the
  loaded region without writing anything and logs the result.

## 6. Open questions

- What the unknown collision flag bits (`0x01 0x08 0x40`) select.

- Palette location for the 24 `D3DFMT_P8` textures.
- ~~Which materials want alpha blending rather than the alpha test.~~ Answered:
  it lives in the texture header, `+0x55` and `+0x56` (see below).
- Meaning of the second and third size fields in the section table for sections
  that carry a texture pack.
- The remaining fields of the object header between `0x80` and `0xA4`.

## Pre-placed meshes

A solid object's vertices are not always local to the object. Terrain -- road
and ground chunks, everything named `TRN_*` -- is authored already standing in
the world: a chunk's vertices are world coordinates and its header bounding box
sits wherever the piece belongs, a thousand units or more from the origin.
Props, buildings and trees are the opposite: their boxes straddle their own
origin.

This matters because scenery instances reference both kinds through the same
field. Applying an instance's translation to a mesh that already carries a
position adds the two together and throws the piece far from where it belongs.
In L4RA that was most of the terrain, with a median error of about 1,500 units
-- the city filled with real geometry in wrong places while the right pieces
were missing from the streets.

The two populations separate cleanly by measuring the header box against
itself: a mesh is pre-placed when the distance from the origin to its box
centre is greater than the box's own diagonal. In L4RA the terrain box centres
are a median 1,293 units out and the props' 7.3, so nothing sits near the
boundary. A pre-placed mesh has to be brought back to its own origin before the
instance transform is applied.

The check that catches this is in `WorldRenderer::CheckReferences`: rebuild each
instance's bounding box from the model it references and compare with the box
the file stores for that instance. Note that only the **centre** is worth
judging -- the file's box is tighter than one built by enclosing a rotated
model's transformed corners, so a size ratio above one is normal and says
nothing. Comparing the stored box against a copy of itself, which an earlier
version of this check did, proves nothing at all.

Median distance from the stored box, over L4RA's 48,074 instances: 198 units
before the fix, 3.6 after.

## Race routes and track maps

`TRACKS/ROUTESL4RA/Paths<id>.bin` is one `0x80034147` container per race. It holds
the free-roam road graph (`0x3414A`, `0x3414D`, identical to `PathsFreeRoam.bin`)
plus three race-specific chunks: `0x34148`, `0x34149` and `0x3414C`.

`0x34148` holds the race's route nodes, 24 bytes each:

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | f32 x2 | world X, Y |
| 0x08 | u16 | segment id (nodes in the same segment are listed in order) |
| 0x0A | u8 x2 | unknown |
| 0x0C | u16 x3 | links into the road graph (`0xFFFF` = none) |
| 0x12 | u16 | padding |
| 0x14 | f32 | distance along the race |

Some segments are side roads that the race does not use, so they are not part of the drawn route.

`TRACKS/TRACKMAP<id>.BIN` is a texture pack holding one 512x512 DXT3 minimap per
race. Every L4RA city map uses the same framing:
`px = 0.0917 * x + 312`, `py = -0.0917 * y + 298.5`.
Drag and closed tracks (L4RB..G) use their own zoom. `tools/race_map.py` builds
`races-map-L4RA.png` from these.

## Race start triggers

The career chunk `0x80034A10` in `GLOBAL/GLOBALB.BUN` (the first one is the full
career) holds a string table `0x34A1D` and the race list `0x34A11`, 136 bytes per
race. In that list, u32 0 is the name offset into the string table
(`S3_SPRINT_6`), the low u16 of u32 6 is the track id (`4164`), and u32 10 is the
bin hash (`h = h*33 + c`, seed `0xFFFFFFFF`) of the trigger name (`EV_14`,
`EVENT_LOCATOR_02`).

Each trigger is a zone in the free-roam road graph (`0x3414A` in
`PathsFreeRoam.bin`), with the hash as the record anchor at offset `h`:
`h-0x30` u32 kind (`0x0D`), `h-0x2C` f32 x, y (the centre, where the start icon
sits), `h-0x20` f32 radius, `h-0x10` f32 bbox min x, min y, max x, max y. An 8x8
square polygon follows the hash. `tools/race_events.py` lists them all.
222 of the 224 races resolve to 116 points. The two `DDAY` intro races have none.

## Free-roam zones: `0x3414A` in `PathsFreeRoam.bin`

The free-roam road graph `TRACKS/ROUTES<region>/PathsFreeRoam.bin` holds the
region's zones in chunk `0x3414A`: shops, race triggers and the city's named
neighbourhoods. `src/nfs/FreeRoam.*` reads them.

`0x3414A` is a run of variable-length records that fill the chunk exactly (671
in L4RA, checked by walking it end to end):

| Offset | Type | Meaning |
|---|---|---|
| 0x00 | u32 | zone type: `0x0D` race trigger (196), `0x0E` shop (23), 12 others |
| 0x04 | f32 x2 | centre X, Y -- where the beacon stands. No Z: the zones are flat |
| 0x0C | f32 x2 | facing, a unit vector in plan |
| 0x14 | f32 | height (z): follows the ground, 10-61, while the footprints stay one size (the editor still reads it as a radius) |
| 0x18 | u32 | 1 on every shop, 0 on race triggers |
| 0x1C | u32 | 0 |
| 0x20 | f32 x4 | plan box of the polygon: min x, min y, max x, max y |
| 0x30 | u32 | hash the career data names the zone by |
| 0x34 | i32 x3 | the **GPS target**, x, y, z in whole units (`0x529410`/`0x529480` route to it): the centre on race triggers, a point on the street 13-115 units away on shops. Off the road, the GPS finds no route |
| 0x40 | u16 | polygon corner count |
| 0x42 | u16 | record size, `0x44 + 8 * count` |
| 0x44 | f32 x2 per corner | the footprint polygon |

Type `0x11` zones are the city's named neighbourhoods (Pigeon Park, Coal
Harbour East, Jackson Heights...): thirteen in L4RA. The hash at `0x30` is the
bin hash of the name as the game spells it (`JACKSON_HEIGHTS`); the names
themselves are nowhere in the file. `src/nfs/CityZones.*` matches them and
groups them into the five districts, which is the editor's own grouping.

## Solved: the alpha test ate the asphalt

Track surfaces come out see-through, with only their lane markings left. The
cause is the editor alpha-testing every material at `ALPHAREF 128` while the
alpha channel of many track textures is not opacity at all.

Measured in L4RA:

- `RDP_LANEB_WD_AA_KT` (DXT3, used by **713** material slots) has only **5.5%**
  of its texels at alpha >= 128. Testing against it removes 94.5% of the road.
- `ARC_CONCRETEA_VK` 2.9% (84 slots), `ARC_COURTGD_04` 5.3% (229 slots),
  `SGN_CLOCKGLOW` 0.0% (13 slots).
- Genuine cut-outs sit at the other end: `SGN_NEON_OUTLINE` and
  `SGN_OPEN_24_HOURS_NEON` pass 100%.
- The P8 palette's fourth byte is already known to be gloss rather than opacity;
  the low DXT3 alpha looks like the same channel in another format.

What is NOT the answer: a "the alpha never passes anywhere" rule. It rescues
exactly one texture in the region (`SGN_CLOCKGLOW`) and leaves the road alone.
That rule is implemented in `TextureCache::Create` via `AlphaNeverPasses` and is
harmless, but it is not the fix.

The promising lead is the material flags field at +0x20 of the 60-byte material
entry, which is what the game would switch on. A first pass over 1,509 materials
found the value 255 everywhere, so either the field is somewhere else in the
entry or the switch is not per material. Worth resolving before inventing a
threshold on "fraction of texels that pass", which would be a guess.

**Resolution.** The switch is per texture, not per material: `+0x55` of the
124-byte texture header is the alpha usage (0 none, 1 punch-through, 2
modulated) and `+0x56` the blend (0 copy, 1 blend, 2 additive). Over the 1 821
textures of the eight track streams both only hold 0..2. `RDP_LANEB_WD_AA_KT`
is 2/0: its alpha is not opacity and it is drawn solid. Trees and chain-link
fences are 1/0 (alpha test), smoke and the race barriers' tape 2/1 (blended),
every neon sign, flare and light beam 2/2 (additive, black is invisible). The
editor draws 0/x and 2/0 opaque, 1/0 alpha tested, and 1 and 2 in a second pass
without depth writes, farthest first. `AlphaNeverPasses` is gone.

The race barriers (`XO_TRACKBARRIER*`) and a few effect sprites draw from packs
the game keeps loaded for every track: `GLOBAL\InGameCommon.bun` (which holds
`OBJ_TRACKBARRIERB_*`, `SFX_NEONBAR*`, `SFX_SMOKE_*`, `SFX_LIGHT_BEAM*`) and
`GLOBAL\GLOBALB.BUN`. The editor reads them as a display-only fallback.

A measurement trap to avoid: `tools/dump_textures.py` does
not decode DXT3 alpha, so any alpha figure taken from it reads as fully
transparent. Read the nibbles out of the block bytes instead.

## Adding a texture to a section — what the game insists on

Everything here was established against `L4RA` section `Q38` by changing one
thing at a time and watching the game, because the editor accepts several
layouts the game does not. Each rule below is one the game enforced by crashing
or by drawing nothing.

**The object header carries its own texture count.** One byte at `+0x19` of
`0x00134011` holds the number of entries in the object's `0x00134012` list. The
list's length is already implied by that chunk's size, so the field looks
redundant — but it is what the game sizes the object's texture array from.
Leave it behind when the list grows and a material referring to the new entry
indexes one past the end; the game dies on the spot. Confirmed equal to the
entry count in 155 of 155 objects in the section.

**The pack's three tables are searched, not scanned.** In every shipped pack the
texture headers run in ascending order of hash, with the hash table exactly
parallel to them. Appending a new texture at the end and leaving the order alone
does not crash — the game simply draws nothing for that texture, which is what a
binary search that cannot find a hash looks like. Headers, format entries and
hash table have to be sorted together.

**Those tables carry no filler.** Their payloads sit at offsets that are not
multiples of sixteen (84, 116, 112 in this pack), so the game reads them from the
first byte after the chunk header. Giving them the default sixteen-byte
alignment puts `0x11` bytes where the first texture header should be.

**Texture pixels are laid end to end.** The second texture in this pack ends at
131 104 and the third begins there; 131 104 is not a multiple of 128. Only the
pixel block itself is aligned, and it is aligned by its absolute position in the
file.

**The pack must not move what follows it.** It sits near the front of a section,
so growing it shifts every object behind it — and those objects are copied
verbatim, filler included, with filler sized for where they used to be. A vertex
chunk is aligned by absolute file offset, so the pack has to absorb the
remainder itself and grow by a whole number of 128-byte units.

**The table's second size field excludes pixel data only.** `streamSize` less
the payload of every `0xB3320000` block: 1 251 728 − 401 584 = 850 144, exactly
what the shipped table holds. The pack's own tables count as geometry.

### Fields in a 124-byte texture header

| Offset | Meaning | How it was established |
| --- | --- | --- |
| `+0x24` | name hash | matches `h = 0xFFFFFFFF; h = h*33 + c` |
| `+0x28` | a category, not the texture | two of three textures share a value |
| `+0x48/49` | `log2` of width and height | holds for all 26 textures |
| `+0x4A` | `0x22` for DXT1, `0x24` for DXT3 | tracks format, not size |
| `+0x4E` | mip count, stopping at eight pixels a side | holds for all 26 |
| `+0x55` | alpha usage: 0 none, 1 punch-through, 2 modulated | 1 on the two cut-out textures, 0 on the opaque ones |
| `+0x56` | alpha blend: 0 copy, 1 blend, 2 additive | 2 on every neon and flare, 1 on smoke and barrier tape |

Everything else is copied from a texture of the same format that the game
already loads. Those are values the game accepts for a texture of this kind,
which is more than can be said for anything invented.
