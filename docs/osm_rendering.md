# OSM Rendering Validation

`osmrender` is still experimental. Validate rendering in small steps before judging map aesthetics from a full extract.

## 1. Deterministic Tiny Fixture

Use a tiny generated PBF with four nodes, one closed water polygon, and one primary road. This checks geometry decoding, polygon collection, line collection, layer painting, BMP writing, and node-index equivalence.

Expected counters for `build/tiny-render.osm.pbf` at `--bbox 9.99,53.54,10.02,53.57 --width 128 --height 128`:

```text
nodes_in_bbox: 4
ways_seen: 2
ways_decoded: 2
ways_drawn: 2
segments_drawn: 7
polygons_collected: 1
segments_collected: 7
visible_pixels: 2303
```

The indexed and non-indexed BMPs should compare byte-identical:

```sh
./build/freestanding-linux-x86_64/osmnodeindex build/tiny-render.osm.pbf build/tiny-render.osmnidx
./build/freestanding-linux-x86_64/osmrender build/tiny-render.osm.pbf build/tiny-render.bmp --bbox 9.99,53.54,10.02,53.57 --width 128 --height 128 --style styles/osmrender-default.conf --stop-after-ways 2
./build/freestanding-linux-x86_64/osmrender build/tiny-render.osm.pbf build/tiny-render-indexed.bmp --bbox 9.99,53.54,10.02,53.57 --width 128 --height 128 --style styles/osmrender-default.conf --node-index build/tiny-render.osmnidx --stop-after-ways 2
cmp -s build/tiny-render.bmp build/tiny-render-indexed.bmp
```

## 2. Real Extract Checkpoint

Build a node index once, then run bounded render checkpoints. The node index avoids collecting bbox nodes by streaming all nodes from the PBF on every render.

```sh
./build/freestanding-linux-x86_64/osmnodeindex data/hamburg-260524.osm.pbf build/hamburg.osmnidx
timeout 30 ./build/freestanding-linux-x86_64/osmrender data/hamburg-260524.osm.pbf build/hamburg-way-checkpoint.bmp --bbox 9.97,53.54,10.02,53.57 --width 512 --height 512 --style styles/osmrender-default.conf --node-index build/hamburg.osmnidx --stop-after-ways 250
```

Current Hamburg checkpoint counters:

```text
nodes_in_bbox: 138387
ways_seen: 250
ways_decoded: 250
ways_drawn: 4
segments_drawn: 18
polygons_collected: 2
segments_collected: 18
visible_pixels: 283
node_index: yes
bounded: yes
```

This confirms the sparse BMP is expected for that early checkpoint: only 4 of the first 250 renderable ways intersect the viewport. Larger checkpoints currently hit timeout limits, so improving way filtering or spatial indexing should come before further style tuning.

## 3. Green-Focused Berlin Checks

Berlin has a dense tree-node layer and many green area tags. Use `--green-only` to suppress roads/buildings/rail and keep water plus green area styles. Use `--tree-points` or `--stop-after-trees` when validating `natural=tree` nodes.

```sh
timeout 60 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-tree-points-5000.bmp --bbox 13.05,52.33,13.75,52.68 --width 1024 --height 1024 --style styles/osmrender-default.conf --green-only --stop-after-trees 5000
```

Current expected counters:

```text
nodes_in_bbox: 814238
tree_nodes_drawn: 5000
visible_pixels: 3590
bounded: yes
tree_points: yes
green_only: yes
```

The Berlin green-data inventory is in `docs/berlin_green_data.md`. Major recognizable green landmarks such as Grunewald and Spandauer Forst are multipolygon relations, so full recognizability depends on relation geometry support.

Build node and way indexes once, then target a single green relation with `--relation-id` to validate member-way rendering without mixing in unrelated green ways. Grunewald is relation `3410`:

```sh
./build/freestanding-linux-x86_64/osmnodeindex data/berlin-260524.osm.pbf build/berlin.osmnidx
./build/freestanding-linux-x86_64/osmwayindex data/berlin-260524.osm.pbf build/berlin.osmwidx
timeout 120 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-grunewald-relation-3410-wayidx.bmp --bbox 13.17,52.44,13.30,52.54 --width 1024 --height 768 --style styles/osmrender-default.conf --node-index build/berlin.osmnidx --way-index build/berlin.osmwidx --green-only --relation-id 3410
```

Current expected counters:

```text
relations_seen: 1
relation_members_collected: 112
relation_ways_matched: 112
ways_decoded: 112
ways_drawn: 112
segments_drawn: 2991
polygons_collected: 90
visible_pixels: 18583
node_index: yes
way_index: yes
```

This proves relation member geometry reaches the renderer without a linear way scan. It is still not full multipolygon assembly: closed member ways are filled individually and open member chains are drawn as outlines until relation ring stitching is implemented.

## 4. City-Name Rendering

`osmrender --city NAME` resolves an administrative boundary relation by exact `name`, computes a padded bbox from indexed boundary member ways, overlays that boundary, and renders the city. It requires both node and way indexes for the same extract.

Potsdam can be rendered from the Brandenburg extract:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/brandenburg-260524.osm.pbf build/brandenburg.osmnidx build/brandenburg.osmwidx
./build/freestanding-linux-x86_64/osmrelindex --progress data/brandenburg-260524.osm.pbf build/brandenburg.osmridx
./build/freestanding-linux-x86_64/osmspindex --progress build/brandenburg.osmnidx build/brandenburg.osmwidx build/brandenburg.osmspidx
./build/freestanding-linux-x86_64/osmrender data/brandenburg-260524.osm.pbf build/potsdam-city.png --city Potsdam --width 1600 --height 1200 --style styles/osmrender-default.conf --node-index build/brandenburg.osmnidx --way-index build/brandenburg.osmwidx --relation-index build/brandenburg.osmridx --spatial-index build/brandenburg.osmspidx --green-only --major-roads
```

Do not use `--stop-after-drawn` for final maps; it intentionally produces sparse smoke-test output. `--no-relation-scan` is also a benchmark shortcut: it keeps boundary lookup from the relation index, but it omits many green multipolygon relations, so parks, forests, woods, and water areas can be visibly incomplete.

Observed Potsdam counters:

```text
nodes_in_bbox: 1033950
ways_decoded: 468088
ways_drawn: 11601
segments_drawn: 165375
relations_seen: 10083
relation_members_collected: 45263
relation_ways_matched: 43119
polygons_collected: 265143
visible_pixels: 1222807
green_only: yes
major_roads: yes
```

Germany-wide indexing is practical with the combined buffered builder:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/germany-260524.osm.pbf build/germany.osmnidx build/germany.osmwidx
```

Measured result: 433,974,413 nodes, 70,233,055 ways, and 590,335,612 refs in 81.40 seconds, producing a 9.8 GB node index and a 6.0 GB way index. Full-Germany city rendering is a separate bottleneck: `osmrender` still streams the complete PBF and falls back to on-disk node lookups for indexes this large, so state extracts remain better for interactive map iteration until spatial way filtering or tiled indexes are added.

The first spatial way index can be built from the Germany node and way indexes:

```sh
./build/freestanding-linux-x86_64/osmrelindex --progress data/germany-260524.osm.pbf build/germany.osmridx
./build/freestanding-linux-x86_64/osmspindex --progress build/germany.osmnidx build/germany.osmwidx build/germany.osmspidx
```

Measured on `data/germany-260524.osm.pbf`, `osmrelindex` indexed 32,045 administrative relations in 39.37 seconds and `osmspindex` indexed 70,233,055 way bboxes in 193.96 seconds, producing a 2.7 GB spatial index. A capped Potsdam smoke test with `--stop-after-drawn 200 --no-relation-scan` decodes only 200 ways by design; the uncapped Germany render without relation scanning drew 11,093 ways in 103.96 seconds, while the full Brandenburg render with relation scanning drew 11,568 ways in 6.70 seconds.
