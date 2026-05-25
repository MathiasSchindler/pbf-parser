# OSMRPACK Experimental Render Pack Format

`OSMRPACK` is an experimental render-friendly cache for city rendering from large OSM extracts. It is intentionally kept outside the stable tool tree while the format is still changing.

The canonical source remains the `.osm.pbf` file. A pack is a disposable derived file that can be rebuilt when the source extract or format changes.

## Goals

- Keep `data/germany-*.osm.pbf` as the source of truth.
- Spend a few minutes once to prepare a file that makes many city renders much faster.
- Avoid full Germany PBF scans during rendering.
- Preserve enough classification and metadata to add more map features later.
- Use a tile directory so `osmrender` can jump directly to the tiles intersecting a city bbox.
- Stay no-libc and dependency-free.

## Current Status

Version 1 currently writes a flat way-derived geometry payload behind a single tile-directory record. It is renderable, but not yet spatially tiled: `osmrenderpackrender` still scans the feature payload once, filters features by the requested bbox, projects the visible geometry, and then draws from memory.

Version 2 is now written by `osmrenderpackv2`. It is not rendered by `osmrenderpackrender` yet. The first v2 builder reads the PBF directly, writes a bbox-only place directory, writes a sorted directory of non-empty geographic tiles, and stores each way-derived feature in every tile touched by its feature bbox.

Administrative city boundary geometry is not embedded in the v1 pack yet. For `--city` renders, the current renderer can resolve the boundary from external `.osmridx`, `.osmwidx`, and `.osmnidx` files, use its bbox before scanning pack features, apply the outside-boundary fade, and draw the boundary overlay. If those index files are absent, the pack geometry still renders without the boundary/fade layer.

Tools:

```sh
cd experimental
make
./build/freestanding-linux-x86_64/osmrenderpack ../data/germany-260524.osm.pbf ../build/germany.osmrpack
./build/freestanding-linux-x86_64/osmrenderpackv2 --tile-zoom 10 ../data/germany-260524.osm.pbf ../build/germany.rpack
./build/freestanding-linux-x86_64/osmrpackinfo ../build/germany.osmrpack
```

Buildings are skipped by default to keep the first experimental packs small enough for iteration. Pass `--buildings` to include building footprints.

## File Layout

All integers are little-endian. Offsets are absolute byte offsets from the start of the file.

```text
OSMRPACK file
  header                      fixed 160 bytes
  tile directory              tile_count * 80 bytes
  feature data                variable, tile payloads
  string table                variable, shared strings
```

The v1 header uses magic bytes:

```text
OSMRPK01
```

Header fields:

```text
offset size field
0      8    magic = "OSMRPK01"
8      4    version = 1
12     4    header_size = 160
16     4    tile_record_size = 80
20     4    tile_zoom
24     4    flags
28     4    layer_count
32     8    tile_count
40     8    tile_directory_offset
48     8    feature_data_offset
56     8    feature_data_size
64     8    string_table_offset
72     8    string_table_size
80     8    source_fileblocks
88     8    source_data_blocks
96     8    source_nodes
104    8    source_ways
112    8    source_relations
120    8    source_compressed_bytes
128    8    source_uncompressed_bytes
136    24   reserved
```

Current flags:

```text
bit 0: empty geometry, set by the header-only v1 builder
```

## Tile Directory

The planned tile record is 80 bytes:

```text
offset size field
0      8    tile_id
8      4    z
12     4    x
16     4    y
20     4    feature_count
24     4    layer_mask
28     4    reserved
32     8    payload_offset
40     8    payload_size
48     8    min_lon_nano
56     8    min_lat_nano
64     8    max_lon_nano
72     8    max_lat_nano
```

`tile_id` should be stable and sortable. The current recommendation is:

```text
tile_id = (z << 58) | (x << 29) | y
```

This supports zooms up to 29 in the bit layout, although the current experimental format caps build-time zoom at 18.

## Planned Layers

Initial layers should match what the renderer already draws:

```text
water_area
water_line
forest_area
park_area
motorway
primary
secondary
minor_road
rail
boundary
building_area
```

The format should not throw away future possibilities too aggressively. Each feature should eventually keep:

```text
source object id
source type: way or relation
style class
flags
bbox
optional name string id
optional compact tag bits
geometry offset and size
```

Useful future tag bits include `building`, `amenity`, `landuse`, `natural`, `leisure`, `man_made`, `tourism`, `place`, road rank, and admin level.

## Planned Geometry Encoding

The current geometry payload favors simplicity over compression:

```text
u64 feature_count
feature_count repetitions:
  u32 style_id
  u32 flags
  u32 point_count
  u32 reserved
  i64 min_lon_nano
  i64 min_lat_nano
  i64 max_lon_nano
  i64 max_lat_nano
  point_count repetitions:
    i64 lon_nano
    i64 lat_nano
```

Current feature flags:

```text
bit 0: closed area feature, fillable by the renderer
```

Coordinates are absolute WGS84 nanodegrees. This is larger than tile-local deltas, but makes the first renderer simple and lets one pack render any bbox inside the extract. Compression and tile-local coordinates can come after the access pattern is proven.

Large features are currently stored once in the flat payload. Once true tiles are added, large features can initially be duplicated into every tile their bbox touches. That costs space, but makes rendering simple and fast. Later versions can add shared feature payloads if duplication becomes too expensive.

## Build Strategy

A practical builder can reuse the existing indexes:

```text
.osmnidx  node id -> lon/lat
.osmwidx  way id -> refs
.osmridx  administrative city boundary lookup
.osmspidx way id -> bbox
```

The first useful builder milestone should:

1. Stream the PBF once to classify way and relation tags.
2. Use way refs and node coordinates from indexes to materialize render geometry.
3. Assign each feature to intersecting tiles.
4. Write tile directory and tile payloads.

The target budget is about five minutes for Germany-scale input on this machine. That is plausible if the pack build combines classification and geometry work carefully and avoids excessive random node lookup.

## Render Strategy

A pack-only renderer should:

1. Resolve a city bbox from pack metadata or the existing relation index.
2. Convert bbox to tile range.
3. Read only tile directory records in that range.
4. Read only those tile payloads.
5. Draw layers to PNG.

This removes the current full Germany PBF node, way, and relation scans from city rendering. That is the path from the current 90 second Germany Potsdam render toward 5-10 second renders.

The current v1 renderer is an intermediate step: it avoids PBF scans but still uses the existing indexes for relation-derived city boundaries until boundary geometry is either embedded in the pack or materialized into true per-tile payloads.

PNG output is not part of the pack format, but the experimental renderer writes compact PNGs by default using adaptive scanline filters, fixed-Huffman deflate, and a generated indexed palette. Exact RGB output remains available with `--png-rgb`.

Use `osmrenderpackrender --profile` when changing the format or renderer. In the current flat v1 pack, the renderer streams the payload through a buffered scanner and reports how much feature data was inspected versus retained for drawing. This keeps the flat scan tolerable, while still making the cost of missing true tile partitioning visible.

## Version 2

Version 2 turns `OSMRPACK` from a flat render cache into a seek-friendly spatial render database. The main goal is that a request such as `--city Berlin` or `--place Hamburg-Harburg` can resolve a bbox from a small directory, map that bbox to tile records, and then read only the tile payloads needed for the render.

The v2 pack should remain disposable and rebuildable. It does not need to be a general OSM database. It should only store enough metadata and geometry to render quickly.

### V2 File Layout

All offsets are absolute byte offsets from the start of the file. Variable-size sections should be independently skippable.

```text
OSMRPACK v2 file
  header                         fixed, larger than v1
  place directory                place_count * fixed record
  tile directory                 tile_count * fixed record
  optional tile range index      compact lookup for z/x/y -> tile directory span, not written yet
  tile payloads                  variable, one payload per non-empty tile
  string table                   UTF-8 strings for names and admin context
  optional metadata blocks       future extension area
```

The header should contain offsets, sizes, counts, and record sizes for every section. Record sizes must be in the header so a future v3 can append fields while older tools can reject unsupported layouts cleanly.

Magic:

```text
OSMRPK02
```

The current header is 256 bytes and contains these fields:

```text
offset size field
0      8    magic = "OSMRPK02"
8      4    version = 2
12     4    header_size = 256
16     4    flags
20     4    tile_zoom
24     4    tile_halo
28     4    layer_count
32     4    place_record_size = 112
36     4    tile_record_size = 96
40     4    feature_record_size = 48
48     8    place_count
56     8    tile_count
64     8    place_directory_offset
72     8    place_directory_size
80     8    tile_directory_offset
88     8    tile_directory_size
96     8    tile_range_index_offset, 0 when absent
104    8    tile_range_index_size, 0 when absent
112    8    tile_payload_offset
120    8    tile_payload_size
128    8    string_table_offset
136    8    string_table_size
144    8    source_nodes
152    8    source_ways
160    8    source_relations
168    88   reserved
```

`tile_halo` is the number of neighboring tiles the renderer should include around a requested bbox tile range. A default of `1` is a pragmatic first choice: it catches labels/lines/polygons that cross a tile boundary without needing immediate geometry clipping.

### Place Directory

The place directory is a bbox/name lookup table. It should not store full boundary geometry in v2. Boundary geometry can be read from tile payloads, from a later dedicated boundary section, or from external indexes while the format is still experimental. Keeping place records bbox-only makes the directory small and cheap to scan.

The directory should be flexible enough for cities, districts, federal states, and future larger packs. Examples include:

```text
Berlin
Regensburg
Weimar
Hessen
Hamburg-Harburg
```

Names are ambiguous by design. The renderer should not assume that a name maps to exactly one place. It should return all matching records or pick a best default only when there is a clear score rule.

Current place record:

```text
offset size field
0      8    source_id              OSM relation/node/way id when known
8      4    source_type            0 unknown, 1 relation, 2 way, 3 node
12     4    place_kind             city, town, village, suburb, district, state, country, etc.
16     4    admin_level            0 when not applicable
20     4    rank_score             larger wins for default disambiguation
24     8    min_lon_nano
32     8    min_lat_nano
40     8    max_lon_nano
48     8    max_lat_nano
56     8    name_offset
64     4    name_size
68     8    parent_name_offset      optional, 0/size 0 when absent
76     4    parent_name_size
80     8    country_code_offset     optional, useful for Europe-scale packs
88     4    country_code_size
92     4    flags
96     16   reserved
```

The first builder stores relation-derived places only. Names are stored in the string table and offsets are absolute file offsets.

This record is intentionally not a perfect administrative model. It is a render lookup hint. A future Europe pack can store multiple `Weimar` records and let the CLI show something like:

```text
Weimar, Thueringen, DE        relation 62422   admin_level 6
Weimar, Hessen, DE            relation ...     admin_level ...
```

Suggested lookup behavior:

1. Exact case-sensitive or case-folded name match in the string table.
2. If one match exists, use it.
3. If many matches exist, sort by `rank_score`, `place_kind`, `admin_level`, and bbox area.
4. Print candidates and ask for a more specific selector when no unambiguous default is acceptable.

Future selectors can be plain strings first, then grow into forms such as:

```text
--city Weimar --admin Thueringen
--place Hamburg-Harburg
--place-id relation:62422
```

### Tile Directory

The tile directory maps geographic tiles to payload offsets. Tiles should be sorted by `tile_id` so lookup can use binary search or a compact range index.

Suggested tile id:

```text
tile_id = (z << 58) | (x << 29) | y
```

Current tile record:

```text
offset size field
0      8    tile_id
8      4    z
12     4    x
16     4    y
20     4    feature_count
24     4    layer_mask
28     4    flags
32     8    payload_offset
40     8    payload_size
48     8    min_lon_nano
56     8    min_lat_nano
64     8    max_lon_nano
72     8    max_lat_nano
80     8    payload_uncompressed_size, 0 when not compressed
88     8    reserved
```

The initial implementation can skip `tile_range_index` and binary-search the sorted tile directory. If the directory becomes large, add a compact lookup table later:

```text
for each tile row at tile_zoom:
  first tile directory index
  tile count in row
```

For Germany at zoom 10-12, even a plain sorted directory should be manageable.

### Tile Payloads

The first v2 tile payload should reuse the current simple feature encoding. That keeps the builder and renderer changes focused on partitioning rather than compression or geometry redesign.

```text
u64 feature_count
feature_count repetitions:
  u32 style_id
  u32 flags
  u32 point_count
  u32 reserved
  i64 min_lon_nano
  i64 min_lat_nano
  i64 max_lon_nano
  i64 max_lat_nano
  point_count repetitions:
    i64 lon_nano
    i64 lat_nano
```

Coordinates can remain absolute WGS84 nanodegrees for v2. Tile-local delta encoding can wait until the access pattern is proven and pack size becomes the bottleneck.

### Feature Assignment

The first v2 builder should assign each feature to every tile intersecting the feature bbox. This intentionally duplicates features that cross tile boundaries. It is simple, seek-friendly, and makes rendering local.

Renderer behavior:

1. Resolve requested bbox.
2. Convert bbox to tile range.
3. Expand the tile range by `tile_halo`, initially `1` tile in every direction.
4. Read those tile payloads.
5. Apply the exact render bbox check again while collecting features, so halo tiles do not draw unrelated distant geometry.

This handles most boundary-crossing geometry cheaply. It does not require clipping features at tile boundaries in the first implementation.

Large features may duplicate into many tiles. That is acceptable for v2 if measured pack size remains reasonable. If duplication becomes too expensive, add a shared feature table later:

```text
tile payload stores feature refs
shared feature table stores geometry once
```

That is a later optimization, not a v2 requirement.

### Render Flow

For `--city Berlin`:

```text
read header
read/place-scan place directory
find all name matches for "Berlin"
choose or ask user to disambiguate
use place bbox for framing and tile selection
convert bbox to tile z/x/y range
expand by tile_halo
lookup tile records
seek/read selected tile payloads only
collect features intersecting render bbox
draw layers
write PNG
```

For raw `--bbox`, the renderer skips the place directory and starts at tile range conversion.

### Open V2 Choices

- Tile zoom: start with 10, 11, or 12 and measure Berlin/Potsdam/Regensburg. Higher zoom reads fewer unrelated features but increases tile count and duplication.
- Halo size: start with `1`; make it a header field and maybe a CLI override for testing.
- Place matching: begin with exact name matches and printed candidates; add smarter normalization later.
- Boundary overlay/fade: bbox-only place records are enough for tile selection. Boundary geometry can come from tile payloads initially if boundary ways are included as features, or from a later dedicated boundary payload section.
- Compression: leave tile payloads uncompressed first. The current bottleneck is read volume and seek locality, not storage size. Add per-tile compression only after tiled access works.
