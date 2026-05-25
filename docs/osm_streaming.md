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
    int (*relation_tags)(void *user, long long id, const PbfTag *tags, unsigned int tag_count);
    int (*relation)(void *user, const PbfRelation *relation);
} PbfStreamCallbacks;

#define PBF_STREAM_SKIP_NODE_TAGS (1U << 0)

int pbf_stream_entities(const char *path,
                        const PbfStreamCallbacks *callbacks,
                        void *user,
                        char *error,
                        size_t error_capacity);

typedef struct {
    const PbfStreamCallbacks *callbacks;
    size_t worker_user_size;
    int (*init_worker)(void *worker_user, unsigned int worker_index, void *shared_user);
    int (*merge_worker)(void *shared_user, void *worker_user);
    void (*destroy_worker)(void *worker_user);
    void *shared_user;
} PbfStreamParallelOptions;

int pbf_stream_entities_parallel(const char *path,
                                 unsigned int worker_count,
                                 const PbfStreamParallelOptions *options,
                                 char *error,
                                 size_t error_capacity);
```

Each callback receives data that is valid only for the duration of the callback. Consumers that need to keep tags, refs, members, or text must copy them.

`PBF_STREAM_SKIP_NODE_TAGS` lets node-only consumers skip dense-node tag decoding. When this flag is set, dense-node streaming decodes IDs and coordinates directly from the packed protobuf fields instead of first materializing temporary arrays. `osmnodeindex` and `osmrender` use this because they only need node IDs and coordinates.

`way_tags` is an optional prefilter. It receives decoded way tags before packed node refs are decoded. Returning `0` rejects the way and skips ref decoding; returning non-zero continues to the normal `way` callback. Renderers use this to avoid ref work for ways that do not match any visible style rule.

`relation_tags` provides the same tag-only path for relations. Tag-only consumers can use it without a `relation` callback to avoid decoding relation members.

Decoded entities include:

- Nodes: ID, latitude, longitude, and tags.
- Ways: ID, decoded node refs, and tags.
- Relations: ID, decoded members, roles, member types, and tags.

Coordinates are exposed as signed nanodegrees. Convert with:

```text
degrees = nano / 1000000000
```

`pbf_stream_entities` runs serially. `pbf_stream_entities_parallel` uses the threaded fileblock pipeline and gives each worker a separate callback user buffer. Worker callbacks must write only to their worker-local state or to explicitly shared read-only structures. After all worker threads exit, `merge_worker` runs serially for each worker so consumers can append results into their global output without callback-time locks.

Parallel entity streaming does not preserve source entity order. It is intended for order-independent consumers such as render-pack construction phases that collect worker-local ways or fill a prebuilt node lookup. If `worker_count` is `1`, the parallel API still uses one worker-local buffer and then calls `merge_worker`; this keeps serial and threaded code paths consistent.

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

## osmaddresses

`osmaddresses` extracts address tags from nodes, ways, and relations and writes tab-separated output. By default it emits only complete records that contain all three tags:

```text
addr:street
addr:housenumber
addr:postcode
```

Build and run:

```sh
make all
./build/freestanding-linux-x86_64/osmaddresses data/germany-060524.osm.pbf build/germany-addresses.tsv
```

Output columns:

```text
type    id    lat    lon    state    city    suburb    street    housenumber    postcode
```

Node rows include coordinates. Way and relation rows leave the coordinate columns empty because this extractor only reads address tags; geometry can be resolved separately with the node index when needed.

Options:

- `--include-incomplete`, emit records that have at least one of state, city, suburb, street, housenumber, or postcode.
- `--no-header`, omit the TSV header row.
- `--limit N`, stop after writing `N` rows when the limit is reached from node or relation callbacks.

The tool uses buffered TSV output and the `way_tags` / `relation_tags` prefilters. Address ways are written without decoding packed node refs, and address relations are written without decoding relation members. This keeps address extraction cheaper than a full geometry lookup.

Smoke validation on `data/hamburg-260524.osm.pbf`:

```sh
./build/freestanding-linux-x86_64/osmaddresses data/hamburg-260524.osm.pbf build/hamburg-addresses-sample.tsv --limit 5
```

Small bounded validation result:

```text
addresses_written: 5
node_addresses: 5
way_addresses: 0
relation_addresses: 0
```

Full Hamburg extraction after adding buffered output and relation tag prefiltering:

```text
addresses_written: 262415
node_addresses: 52076
way_addresses: 210108
relation_addresses: 231
elapsed: 0:01.72
maxrss_kb: 7168
output: 14 MB
```

## osmnodeindex and osmindex

`osmnodeindex` builds a compact binary node coordinate index:

```text
node_id -> lat_nano, lon_nano
```

Build and run:

```sh
make all
./build/freestanding-linux-x86_64/osmnodeindex data/brandenburg-260524.osm.pbf build/brandenburg.osmnidx
```

For rendering workflows, prefer the combined builder because it streams the PBF once and writes both node and way indexes with buffered output:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/brandenburg-260524.osm.pbf build/brandenburg.osmnidx build/brandenburg.osmwidx
```

The index format is intentionally simple:

- 32-byte header with magic, version, record size, and record count.
- 24-byte little-endian records: signed 64-bit node ID, latitude nanodegrees, longitude nanodegrees.
- Records are expected to be sorted by node ID. The builder validates monotonic IDs while streaming.

This keeps memory use low: the builders write buffered records as they stream through the PBF file and do not hold all nodes or ways in memory.

Measured builds:

| Command | Input | Counts | Elapsed | Max RSS | Output |
| --- | --- | ---: | ---: | ---: | ---: |
| `osmindex` | `data/hamburg-260524.osm.pbf` | 3,968,966 nodes; 693,074 ways; 5,003,670 refs | 0.83s | 9,024 KB | 91 MB + 55 MB |
| `osmindex` | `data/brandenburg-260524.osm.pbf` | 26,778,793 nodes; 4,447,642 ways; 36,061,753 refs | 4.98s | 12,864 KB | 613 MB + 377 MB |
| `osmindex` | `data/germany-260524.osm.pbf` | 433,974,413 nodes; 70,233,055 ways; 590,335,612 refs | 81.40s | 17,472 KB | 9.8 GB + 6.0 GB |

The combined Hamburg and Brandenburg indexes were compared byte-for-byte against separate `osmnodeindex` and `osmwayindex` builds; the files were identical.

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

`osmrender` is an experimental bitmap renderer that writes dependency-free 24-bit BMP output:

```sh
./build/freestanding-linux-x86_64/osmrender data/hamburg-260524.osm.pbf build/hamburg.bmp --bbox 9.70,53.30,10.40,53.80 --width 1024 --height 768 --style styles/osmrender-default.conf
```

Options:

- `--bbox MINLON,MINLAT,MAXLON,MAXLAT`
- `--width N`
- `--height N`
- `--style FILE`, load a simple `key=value` render style file
- `--node-points`, draw collected bbox nodes as pixels
- `--stop-after-nodes N`, bounded smoke mode for node drawing
- `--stop-after-drawn N`, bounded smoke mode for way drawing

The renderer now applies a small built-in cartographic style set while staying inside the freestanding C runtime:

- water, parks/green areas, forests, and buildings can be filled when they are closed ways
- roads are classified into major, medium, minor, and path styles
- roads and rails use soft casings, alpha blending, and round brushes instead of raw overwrite pixels
- rendering runs in two passes: closed areas are filled first, then roads, waterways, paths, and rails are stroked above them
- output remains plain 24-bit BMP; no font, image, graphics, or libc dependency is introduced

Style files use decimal color channels so they can be parsed without libc or a JSON parser:

```text
background = 242,239,232
water.fill = 154,196,214,230
water.line = 94,151,183,255
water.width = 2
primary.casing = 202,145,98,220
primary.casing_width = 6
primary.line = 246,194,118,255
primary.width = 4
```

Colors are `R,G,B` or `R,G,B,A`. Supported style groups are `water`, `waterway`, `forest`, `park`, `building`, `motorway`, `primary`, `secondary`, `minor_road`, `path`, and `rail`; supported properties are `fill`, `line`, `casing`, `width`, and `casing_width`.

The first city-scale target is Hamburg:

```sh
./build/freestanding-linux-x86_64/osmrender data/hamburg-260524.osm.pbf build/hamburg-city.bmp --bbox 9.70,53.30,10.40,53.80 --width 1600 --height 1100 --style styles/osmrender-default.conf
```

Validated smoke runs:

| Command shape | Result | Elapsed | Max RSS | Output |
| --- | --- | ---: | ---: | --- |
| Hamburg bbox, `--node-points --stop-after-nodes 5000` | `nodes_in_bbox=5000`, `bounded=yes` | 0:00.01 | 3,712 KB | valid 1024x768x24 BMP |
| Hamburg bbox, `--stop-after-drawn 1` | `nodes_in_bbox=3952481`, `ways_drawn=1`, `segments_drawn=2`, `bounded=yes` | 0:01.60 | 526,976 KB | valid 1024x768x24 BMP |
| Hamburg bbox, `--style styles/osmrender-default.conf --node-points --stop-after-nodes 5000` | `nodes_in_bbox=5000`, `bounded=yes` | smoke | smoke | valid 512x384x24 BMP, 3,037 non-background pixels |

Negative render experiments are also important:

- Random node-index lookup per way ref was too slow for rendering.
- Narrow-bbox full way rendering did not produce a BMP before being terminated.
- A 50-way bounded Hamburg render did not finish in the interactive benchmark window.
- Styled linework over the full Hamburg bbox is still too slow for an interactive smoke check because it scans the PBF for node collection and then scans again for layered strokes.

The current renderer depends on `Sort.Type_then_ID`: it collects bbox nodes first, then draws ways whose refs are already known. It now streams the file a second time for linework so areas do not cover roads and rails. This can validate output and simple city-scale cases, but it is not the final map-rendering architecture. Full practical rendering needs a spatial or fileblock-level index so the tool can avoid scanning and retaining nearly all nodes for a small viewport.

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