# Berlin Green OSM Data Notes

Data file: `data/berlin-260524.osm.pbf`

The Berlin extract contains enough green data to make recognizable green structure, but it is split across nodes, closed ways, and multipolygon relations. The renderer handles nodes only when explicitly requested and can now collect green relation member ways, but it does not yet stitch open multipolygon rings.

## Observed Counts

Counts from `osmlookup` with `--limit 0`:

| Entity | Tag | Count |
| --- | --- | ---: |
| node | `natural=tree` | 286895 |
| node | `natural=tree_row` | 0 |
| way | `natural=wood` | 1888 |
| way | `landuse=forest` | 990 |
| way | `leisure=park` | 2573 |
| way | `landuse=grass` | 18713 |
| way | `landuse=meadow` | 829 |
| way | `leisure=garden` | 3320 |
| way | `natural=scrub` | 7280 |
| relation | `natural=wood` | 86 |
| relation | `landuse=forest` | 119 |
| relation | `leisure=park` | 175 |

## Representative Records

Tree nodes include leaf metadata:

```text
node 21487172 lat=52.514311500 lon=13.351769700 leaf_cycle=deciduous leaf_type=broadleaved natural=tree
node 26908663 lat=52.471730900 lon=13.346266800 leaf_cycle=deciduous leaf_type=broadleaved natural=tree
node 27306554 lat=52.522519100 lon=13.300860000 leaf_cycle=deciduous leaf_type=broadleaved natural=tree
```

Important way-based green areas include parks, grass, meadows, gardens, woods, forests, and scrub:

```text
way 4413796 refs=33 leisure=park name=Preußenpark protection_title=Geschützte Grünanlage
way 4900146 refs=56 landuse=forest
way 4449364 refs=10 landcover=grass landuse=grass
way 17405866 refs=9 landuse=meadow
way 4805430 refs=24 natural=scrub
```

Major recognizable green areas also appear as multipolygon relations:

```text
relation 3410 members=112 landuse=forest name=Grunewald type=multipolygon
relation 13666 members=74 landuse=forest name=Spandauer Forst type=multipolygon
relation 36841 members=8 leisure=park name=Marienhöhe type=multipolygon
```

Berlin's city boundary is present as relation `62422`:

```text
relation 62422 members=177 boundary=administrative admin_level=4 ISO3166-2=DE-BE name=Berlin type=boundary
```

## Renderer Implications

- `natural=tree` is a node tag, so it requires decoding node tags. `osmrender --tree-points` enables this path.
- `landuse=meadow`, `leisure=garden`, `natural=scrub`, `natural=grassland`, `natural=heath`, `natural=tree_row`, `landuse=orchard`, and `landuse=village_green` should be considered green renderable features. The renderer now maps these to park or forest styles.
- Large landmarks such as Grunewald and Spandauer Forst are relation multipolygons. `osmrender --relation-id ID` can target one relation, collect its member way IDs, and render closed member ways or member outlines with the relation's green style.
- `osmwayindex` builds a way-reference index so targeted relation renders can look up member ways directly instead of scanning all PBF ways. On `berlin-260524.osm.pbf`, `build/berlin.osmwidx` contains 1322684 ways and 10329914 refs and is about 110 MB.
- Berlin's administrative boundary can be overlaid with `--boundary-relation-id 62422`.
- Complete forest shapes remain incomplete because open relation members are not stitched into rings yet. The next structural improvement should be relation ring stitching, not color tuning.

## Current Checkpoints

Full Berlin green-shape map with Berlin boundary:

```sh
./build/freestanding-linux-x86_64/osmnodeindex data/berlin-260524.osm.pbf build/berlin.osmnidx
./build/freestanding-linux-x86_64/osmwayindex data/berlin-260524.osm.pbf build/berlin.osmwidx
timeout 300 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-green-shapes-2048x1664.png --bbox 13.05,52.33,13.80,52.68 --width 2048 --height 1664 --style styles/osmrender-default.conf --node-index build/berlin.osmnidx --way-index build/berlin.osmwidx --green-only --boundary-relation-id 62422
```

Variant with large roads overlaid:

```sh
timeout 300 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-green-shapes-roads-2048x1664.png --bbox 13.05,52.33,13.80,52.68 --width 2048 --height 1664 --style styles/osmrender-default.conf --node-index build/berlin.osmnidx --way-index build/berlin.osmwidx --green-only --major-roads --boundary-relation-id 62422
```

The aspect ratio uses a latitude-aware height for the lon/lat bbox around Berlin. The east edge is padded to `13.80` so Berlin's eastern boundary does not clip against the image edge. `osmrender` writes PNG directly when the output path ends in `.png`; use the same command with a `.bmp` output path to write BMP instead. PNG output is dependency-free and uses an indexed-color palette when possible to keep map-style images smaller than BMP.
`--major-roads` keeps motorway/trunk, primary, and secondary roads in `--green-only` renders and leaves tertiary/minor roads out.

Observed current counters:

```text
nodes_in_bbox: 7832535
ways_seen: 48224
ways_decoded: 48224
ways_drawn: 48224
segments_drawn: 722706
relations_seen: 896
relation_members_collected: 4509
relation_ways_matched: 3558
polygons_collected: 43219
visible_pixels: 871668
node_index: yes
way_index: yes
green_only: yes
```

Observed output: `build/berlin-green-shapes-2048x1664.png` is a 2048x1664 indexed-color PNG and is about 3.3 MB.

Observed road-overlay counters:

```text
ways_decoded: 70400
ways_drawn: 70400
segments_drawn: 792005
visible_pixels: 987325
major_roads: yes
```

## Generic City Rendering

`osmrender --city NAME` resolves the city boundary before rendering. The workflow is:

1. Build node and way indexes for the extract once.
2. Find the administrative boundary relation by `name=<city>`, `boundary=administrative`, and a city-level `admin_level`.
3. Use the boundary relation's member ways and the indexes to compute a padded bbox automatically.
4. Render the bbox with `--green-only`, `--boundary-relation-id <id>`, and optional `--major-roads`.

Potsdam from the Brandenburg extract:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/brandenburg-260524.osm.pbf build/brandenburg.osmnidx build/brandenburg.osmwidx
./build/freestanding-linux-x86_64/osmrelindex --progress data/brandenburg-260524.osm.pbf build/brandenburg.osmridx
./build/freestanding-linux-x86_64/osmspindex --progress build/brandenburg.osmnidx build/brandenburg.osmwidx build/brandenburg.osmspidx
./build/freestanding-linux-x86_64/osmrender data/brandenburg-260524.osm.pbf build/potsdam-city.png --city Potsdam --width 1600 --height 1200 --style styles/osmrender-default.conf --node-index build/brandenburg.osmnidx --way-index build/brandenburg.osmwidx --relation-index build/brandenburg.osmridx --spatial-index build/brandenburg.osmspidx --green-only --major-roads
```

Once those indexes exist, the short form is equivalent to the green city-map defaults:

```sh
./build/freestanding-linux-x86_64/osmrender data/brandenburg-260524.osm.pbf build/potsdam-short-default.png --city Potsdam
```

This defaults to the project style, green areas plus major roads, relation scanning, optional spatial filtering, aspect-aware output dimensions, and a brighter outside-boundary mask.

The sparse `germany-potsdam-spatial*.png` benchmark images were produced with `--stop-after-drawn 200` and/or `--no-relation-scan`. Those flags are useful for timing parser/index changes, but they are not full green-map commands: `--stop-after-drawn` stops after a tiny sample, and `--no-relation-scan` skips many multipolygon green areas.

Germany-wide indexes can now be built directly with the combined buffered indexer:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/germany-260524.osm.pbf build/germany.osmnidx build/germany.osmwidx
```

Measured on `data/germany-260524.osm.pbf`: 433,974,413 nodes, 70,233,055 ways, and 590,335,612 refs were indexed in 81.40 seconds. The outputs were 9.8 GB and 6.0 GB. Rendering a city directly from the full Germany PBF is still slower than rendering from a state extract because `osmrender` must stream the whole PBF and perform many on-disk node-index lookups; the next renderer-scale improvement is spatial way filtering or tiled indexes.

Additional Germany indexes are now available for boundary lookup and spatial way filtering:

```sh
./build/freestanding-linux-x86_64/osmrelindex --progress data/germany-260524.osm.pbf build/germany.osmridx
./build/freestanding-linux-x86_64/osmspindex --progress build/germany.osmnidx build/germany.osmwidx build/germany.osmspidx
```

Measured results: `osmrelindex` built `build/germany.osmridx` in 39.37 seconds, and `osmspindex` built `build/germany.osmspidx` in 193.96 seconds. With the drawn-way cap removed, a Germany/Potsdam render using `--no-relation-scan` drew 11,093 ways, but the richer Brandenburg/Potsdam render with relation scanning drew 11,568 ways and includes many more relation-based green shapes.

If `make clean` has removed the generated indexes, rebuild Germany city-rendering indexes in this order: `osmindex`, `osmrelindex`, then `osmspindex`. On the current machine the measured total was about 5.2 minutes, dominated by the spatial index build.

Tree-point checkpoint:

```sh
timeout 60 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-tree-points-5000.bmp --bbox 13.05,52.33,13.75,52.68 --width 1024 --height 1024 --style styles/osmrender-default.conf --green-only --stop-after-trees 5000
```

Expected current counters:

```text
nodes_in_bbox: 814238
tree_nodes_drawn: 5000
visible_pixels: 3590
bounded: yes
tree_points: yes
green_only: yes
```

Grunewald relation checkpoint:

```sh
./build/freestanding-linux-x86_64/osmwayindex data/berlin-260524.osm.pbf build/berlin.osmwidx
timeout 120 ./build/freestanding-linux-x86_64/osmrender data/berlin-260524.osm.pbf build/berlin-grunewald-relation-3410-wayidx.bmp --bbox 13.17,52.44,13.30,52.54 --width 1024 --height 768 --style styles/osmrender-default.conf --node-index build/berlin.osmnidx --way-index build/berlin.osmwidx --green-only --relation-id 3410
```

Expected current counters:

```text
relations_seen: 1
relation_members_collected: 112
relation_ways_matched: 112
ways_decoded: 112
ways_drawn: 112
segments_drawn: 2991
polygons_collected: 90
visible_pixels: 18583
way_index: yes
```

Earlier 8+ member checkpoints timed out because of a line-drawing bug and the linear way scan. The renderer now uses the way index for targeted relations and fixes the Bresenham line step so shallow lines cannot run forever.
