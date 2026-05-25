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

Administrative city boundary geometry is not embedded in the v1 pack yet. For `--city` renders, the current renderer can resolve the boundary from external `.osmridx`, `.osmwidx`, and `.osmnidx` files, use its bbox before scanning pack features, apply the outside-boundary fade, and draw the boundary overlay. If those index files are absent, the pack geometry still renders without the boundary/fade layer.

Tools:

```sh
cd experimental
make
./build/freestanding-linux-x86_64/osmrenderpack ../data/germany-260524.osm.pbf ../build/germany.osmrpack
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
