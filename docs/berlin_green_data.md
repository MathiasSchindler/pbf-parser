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

## Toward Generic City Rendering

A generic Germany-wide command should resolve the city boundary before rendering. The intended workflow is:

1. Build Germany-wide node and way indexes once.
2. Find the administrative boundary relation by `name=<city>`, `boundary=administrative`, and a city-level `admin_level`.
3. Use the boundary relation's member ways and the indexes to compute a padded bbox automatically.
4. Render the bbox with `--green-only`, `--boundary-relation-id <id>`, and optional `--major-roads`.

A future CLI can hide those steps behind a city name, for example:

```sh
./build/freestanding-linux-x86_64/osmrender data/germany-260524.osm.pbf city.png --city Berlin --node-index build/germany.osmnidx --way-index build/germany.osmwidx --green-only --major-roads
```

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
