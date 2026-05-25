# OSM Entity Streaming and Lookup

The PBF parser now exposes decoded OpenStreetMap entities as a stream. This is the base layer for lookup, extraction, indexing, and later rendering tools.

## Stream API

The public API is declared in `src/shared/pbf.h`:

```c
typedef struct {
    unsigned int flags;
    int (*node)(void *user, const PbfNode *node);
    int (*way_tags)(void *user, long long id, const PbfTag *tags, unsigned int tag_count);
    int (*way)(void *user, const PbfWay *way);
    int (*relation)(void *user, const PbfRelation *relation);
} PbfStreamCallbacks;

#define PBF_STREAM_SKIP_NODE_TAGS (1U << 0)

int pbf_stream_entities(const char *path,
                        const PbfStreamCallbacks *callbacks,
                        void *user,
                        char *error,
                        size_t error_capacity);
```

Each callback receives data that is valid only for the duration of the callback. Consumers that need to keep tags, refs, members, or text must copy them.

`PBF_STREAM_SKIP_NODE_TAGS` lets node-only consumers skip dense-node tag decoding. When this flag is set, dense-node streaming decodes IDs and coordinates directly from the packed protobuf fields instead of first materializing temporary arrays. `osmnodeindex` and `osmrender` use this because they only need node IDs and coordinates.

`way_tags` is an optional prefilter. It receives decoded way tags before packed node refs are decoded. Returning `0` rejects the way and skips ref decoding; returning non-zero continues to the normal `way` callback. Renderers use this to avoid ref work for ways that do not match any visible style rule.

Decoded entities include:

- Nodes: ID, latitude, longitude, and tags.
- Ways: ID, decoded node refs, and tags.
- Relations: ID, decoded members, roles, member types, and tags.

Coordinates are exposed as signed nanodegrees. Convert with:

```text
degrees = nano / 1000000000
```

The stream parser currently runs serially. The threaded fileblock pipeline remains available for summary-style consumers such as `pbfinfo`; entity callbacks need an ordering and synchronization policy before they should be parallelized.

## osmlookup

`osmlookup` is the first stream consumer. It filters entities while scanning the PBF file and prints matching records.

Build:

```sh
make all
```

Usage:

```sh
./build/freestanding-linux-x86_64/osmlookup FILE.osm.pbf [options]
```

Options:

- `--type node|way|relation|all`
- `--tag KEY`
- `--tag KEY=VALUE`
- `--name VALUE`
- `--id node:ID`, `--id way:ID`, or `--id relation:ID`
- `--bbox MINLON,MINLAT,MAXLON,MAXLAT`
- `--node-index FILE`, required for way bbox filtering and way geometry output
- `--geometry`, prints way node coordinates when a node index is provided
- `--limit N`, where `0` means no limit

Multiple tag filters are combined with AND semantics.

Examples:

```sh
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type node --tag amenity --limit 3
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type way --tag highway --limit 3
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type relation --tag type=multipolygon --limit 3
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --name Alexanderplatz --limit 10
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --id node:16541597
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type node --bbox 13.30,52.50,13.40,52.60 --tag amenity --limit 3
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type way --tag highway --bbox 13.0,52.0,14.0,53.0 --node-index build/brandenburg.osmnidx --geometry --limit 1
```

Output is line-oriented text. Nodes include coordinates; ways include decoded ref counts; relations include member counts.

## osmnodeindex

`osmnodeindex` builds a compact binary node coordinate index:

```text
node_id -> lat_nano, lon_nano
```

Build and run:

```sh
make all
./build/freestanding-linux-x86_64/osmnodeindex data/brandenburg-260524.osm.pbf build/brandenburg.osmnidx
```

The index format is intentionally simple:

- 32-byte header with magic, version, record size, and record count.
- 24-byte little-endian records: signed 64-bit node ID, latitude nanodegrees, longitude nanodegrees.
- Records are expected to be sorted by node ID. The builder validates monotonic IDs while streaming.

This keeps memory use low: the builder writes each node record as it streams through the PBF file and does not hold all nodes in memory.

Measured builds:

| Input | Nodes | Elapsed | Max RSS | Output |
| --- | ---: | ---: | ---: | ---: |
| `data/brandenburg-260524.osm.pbf` | 26,778,793 | 1:36.72 | 10,112 KB | 613 MB |
| `data/hamburg-260524.osm.pbf` | 3,968,966 | 0:13.84 | 6,272 KB | 91 MB |

The Brandenburg index was compared byte-for-byte against the earlier builder output after the dense-node fast path was added; the files were identical.

## Validation

Validated on `data/brandenburg-260524.osm.pbf`:

```sh
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type node --tag amenity --limit 1
```

produced a decoded node with coordinates and tags:

```text
node 16541597 lat=52.546314700 lon=13.345599000 amenity=fuel ...
```

Way and relation decoding was checked with:

```sh
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type way --tag highway --limit 3
./build/freestanding-linux-x86_64/osmlookup data/brandenburg-260524.osm.pbf --type relation --tag type=multipolygon --limit 3
```

The `pbfinfo` regression check still reports the known Brandenburg/Berlin totals:

```text
dense_node_groups: 3348
nodes: 26778793
ways: 4447642
relations: 59572
```

## Rendering Path

`osmrender` is an experimental bitmap renderer that writes 24-bit BMP output:

```sh
./build/freestanding-linux-x86_64/osmrender data/hamburg-260524.osm.pbf build/hamburg.bmp --bbox 9.70,53.30,10.40,53.80 --width 1024 --height 768
```

Options:

- `--bbox MINLON,MINLAT,MAXLON,MAXLAT`
- `--width N`
- `--height N`
- `--node-points`, draw collected bbox nodes as pixels
- `--stop-after-nodes N`, bounded smoke mode for node drawing
- `--stop-after-drawn N`, bounded smoke mode for way drawing

Validated smoke runs:

| Command shape | Result | Elapsed | Max RSS | Output |
| --- | --- | ---: | ---: | --- |
| Hamburg bbox, `--node-points --stop-after-nodes 5000` | `nodes_in_bbox=5000`, `bounded=yes` | 0:00.01 | 3,712 KB | valid 1024x768x24 BMP |
| Hamburg bbox, `--stop-after-drawn 1` | `nodes_in_bbox=3952481`, `ways_drawn=1`, `segments_drawn=2`, `bounded=yes` | 0:01.60 | 526,976 KB | valid 1024x768x24 BMP |

Negative render experiments are also important:

- Random node-index lookup per way ref was too slow for rendering.
- Narrow-bbox full way rendering did not produce a BMP before being terminated.
- A 50-way bounded Hamburg render did not finish in the interactive benchmark window.

The current one-pass renderer depends on `Sort.Type_then_ID`: it collects bbox nodes first, then draws ways whose refs are already known. This can validate output and simple cases, but it is not the final map-rendering architecture. Full practical rendering needs a spatial or fileblock-level index so the tool can avoid scanning and retaining nearly all nodes for a small viewport.

Bitmap or vector map rendering should continue to build on this stream API, but it should not try to load the full PBF into memory. Ways usually contain only node IDs, so geometry rendering needs node coordinates.

The reusable node index now provides:

```text
node_id -> lat_nano, lon_nano
```

Then a renderer can stream ways and relations, resolve only needed node coordinates, and draw or emit geometry.

Recommended next order:

1. Add a block or spatial index that records which PBF data blocks contain nodes/ways intersecting a coarse tile grid.
2. Use the index to build a compact viewport node table instead of retaining nearly all extract nodes.
3. Expand style rules for roads, paths, water, rail, buildings, and boundaries.
4. Add vector output after clipping and simplification rules are stable.

This keeps lookup, indexing, and rendering as separate consumers of the same PBF stream.