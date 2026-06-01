# pbf-parser

This project contains small freestanding C tools for reading and processing OpenStreetMap PBF files. The current focus is on inspecting PBF metadata, streaming OSM entities, running lookups, building render packs for map output, and building route packs for routing.

The tools are written for static Linux x86_64 builds without the standard C library. They use a small local runtime, direct platform syscall wrappers, and project-local parsing and compression code instead of external libraries.

## Current Status

The active render-pack path is the `OSMRPK02` version 2 pipeline:

```sh
make
./build/freestanding-linux-x86_64/pbf-to-rpack data/brandenburg-260524.osm.pbf data/brandenburg.rpack
./build/freestanding-linux-x86_64/rpack-info data/brandenburg.rpack
./build/freestanding-linux-x86_64/rpack-render data/brandenburg.rpack build/potsdam.png --city Potsdam
```

The authoritative implemented render-pack spec is `docs/osmrpack_format.md`. The active builder is `src/tools/pbf_to_rpack.c`, the active renderer is `src/tools/rpack_render.c`, and `rpack-info` understands the same v2 header.

Directory status:

- `src/` and `docs/` contain the active tools and implemented format documentation.
- `actual/` contains future stable-format design notes, not the format currently emitted by the tools.
- `browser/` is a browser prototype that consumes packs produced by the active top-level tools.

## Scope

The repository currently provides:

- `pbf-info`, which reads OSM PBF fileblocks and reports summary counts and metadata.
- `osm-lookup`, which streams nodes, ways, and relations and filters them by type, tags, IDs, names, and bounding boxes.
- `osm-addresses`, which extracts address tags into TSV rows containing state, city, suburb, street, house number, and postcode fields.
- `osm-buildings`, which extracts building/address candidates into TSV rows with address fields, building tags, levels/flats hints, and centroid/bbox coordinates.
- `pbf-to-rpack`, which builds `OSMRPK02` render-pack files from `.osm.pbf` extracts for faster repeated city/bbox rendering.
- `rpack-info`, which prints render-pack header and directory metadata.
- `rpack-render`, which renders PNGs from `.rpack` files using `--city` or `--bbox` without scanning the source PBF.
- `pbf-to-rte`, an `OSMRTE01` route-pack converter that writes the binary container, source counts, metric tile metadata, bounds, sorted tile directory records, address records, and tiled walking graph payloads.
- `rte-info`, which inspects `OSMRTE01` route-pack files, prints header and section metadata, checks whether address and GTFS sections are present, and can look up the route tile for a latitude/longitude pair.
- `rte-route`, which resolves two walking-route endpoint addresses from an `OSMRTE01` route pack, loads the endpoint tile neighborhood, snaps to walking graph nodes, and runs Dijkstra from the `.rte` file.
- `test-thread`, a small validation tool for the Linux nolibc threading layer.
- `test-font`, a small smoke tool for the vendored TrueType renderer used by future map labels.

The project is not intended to be a complete GIS engine. It is a compact parser and tool collection for exploring OSM PBF data with minimal runtime dependencies.

## Architecture

The code is split into platform, runtime, shared parser, and tool layers:

- `src/arch` contains architecture-specific process startup and syscall entry code.
- `src/platform` contains operating-system wrappers for files, I/O, memory mapping, threads, time, identity, and process operations.
- `src/shared/runtime` contains the small runtime functions used instead of libc.
- `src/shared/compression` contains the local zlib/deflate implementation used for PBF blobs.
- `src/shared/pbf.c` and `src/shared/pbf.h` contain the OSM PBF and protobuf streaming parser.
- `src/shared/osm_index.c` and `src/shared/osm_index.h` contain legacy index readers used by older lookup/extraction helpers.
- `src/shared/fontrender` contains the vendored freestanding TrueType parser/rasterizer core from `~/fontrender`.
- `src/shared/fontrender_runtime.c` installs this project's memory, file, and logging hooks for the font renderer.
- `src/tools` contains the command-line tools built on top of the shared layers.

The parser reads PBF fileblocks, decodes `OSMHeader` and `OSMData` blobs, inflates zlib-compressed payloads, parses protobuf fields, and exposes decoded OSM entities through streaming callbacks. Consumers can skip node tags or prefilter way and relation tags when they only need coordinates, IDs, renderable ways, or tag-only records.

## Build

The default build uses `gcc-16` and writes static binaries to `build/freestanding-linux-x86_64/`:

```sh
make all
```

The build output and local PBF data files are intentionally ignored by git.

`make clean` removes the whole build directory. Recreate the binaries with `make`, then recreate any needed `.rpack` or `.rte` files.

## Data

Place local `.osm.pbf` files under `data/` when running the tools. Example:

```sh
./build/freestanding-linux-x86_64/pbf-info data/example.osm.pbf
```

Residential building/address candidates can be exported as TSV for a city-sized bbox. Bbox arguments use the same `MINLON,MINLAT,MAXLON,MAXLAT` order as `osm-lookup`.

```sh
./build/freestanding-linux-x86_64/osm-buildings data/germany-260524.osm.pbf potsdam_buildings.tsv \
	--bbox 12.85,52.30,13.25,52.55
```

For map rendering, build a render pack once and render from it:

```sh
./build/freestanding-linux-x86_64/pbf-to-rpack --tile-zoom 10 --threads 8 data/germany-260524.osm.pbf build/germany.rpack
./build/freestanding-linux-x86_64/rpack-render build/germany.rpack city.png --city Berlin --width 1600 --height 1200 --profile
```

`rpack-render` renders directly from the pack-contained place directory, tile payloads, and embedded per-place boundary payloads. It loads `styles/osmrender-default.conf` when present, and `--style FILE` can override the map colors and stroke widths for one render. For `--city`, it draws the matching administrative boundary and fades pixels outside it from the `.rpack` data; distant exclave components are excluded from the default viewport and can be shown with `--exclave-insets`. A route overlay can be drawn with `--route-polyline FILE`, where the file contains one `lon,lat` point per line. The pack format is documented in `docs/osmrpack_format.md`.

## Routing

The route-pack converter builds the query-time routing cache from a PBF extract:

```sh
make -B build/freestanding-macos-arm64/pbf-to-rte
build/freestanding-macos-arm64/pbf-to-rte --tile-size-m 4000 --threads 2 \
	data/brandenburg-260525.osm.pbf build/brandenburg.rte
```

Add `--gtfs DIR` to embed public-transport stops, routes, service calendars, trips, and stop-time events into the same `OSMRTE01` file:

```sh
build/freestanding-macos-arm64/pbf-to-rte --tile-size-m 4000 --threads 2 \
	--gtfs data/GTFS \
	data/brandenburg-260525.osm.pbf build/brandenburg-gtfs.rte
```

This milestone writes an `OSMRTE01` file with real metric route tiles, tiled walking nodes, edge-offset arrays, directed walking edges, minimal snap-grid records, a first global address section for exact street/house lookup, and an optional embedded GTFS single-leg transit payload. Full route-pattern transfer routing and optimized address dictionaries are still future work. The target format is documented in `docs/OSMRTE01.md`.

Inspect a generated route pack:

```sh
make -B build/freestanding-macos-arm64/rte-info
build/freestanding-macos-arm64/rte-info build/brandenburg-tiles.rte --sections
build/freestanding-macos-arm64/rte-info build/brandenburg-tiles.rte \
	--address "Friedrich-Engels-Strasse 22"
build/freestanding-macos-arm64/rte-info build/brandenburg-tiles.rte \
	--address "Hermann-Mattern-Promenade 25, Potsdam"
build/freestanding-macos-arm64/rte-info build/brandenburg-tiles.rte \
	--tile 52.3906,13.0645
```

Packs built with `--gtfs` should report `addresses_present: yes` and `gtfs_present: yes`. Address lookup does not require a tile coordinate; `--tile` is only a low-level tile-directory probe. Address queries use `street house` and may add `, city/suburb/postcode` to disambiguate. The inspector folds `Straße` and `Strasse` together for lookup.

The first route-pack walking CLI resolves endpoints from `.rte` data:

```sh
make -B build/freestanding-macos-arm64/rte-route
build/freestanding-macos-arm64/rte-route data/brandenburg.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam"
```

When the pack was built with GTFS, `rte-route` also asks for a single public-transport leg with walking access at both ends. If neither `--depart` nor `--arrive` is provided, the query uses the current time as `--depart now`:

```sh
build/freestanding-macos-arm64/rte-route build/brandenburg-gtfs.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam" \
	--depart 2026-05-27T11:00
```

The transit suggestion reports the access walk, boarding stop, vehicle mode and line, alighting stop, final walk, and total time. The first embedded planner intentionally evaluates one GTFS vehicle leg plus walking at both ends; multi-transfer routing and stop-to-walk edge snapping are next milestones.

For the Potsdam sample above, the current route pack resolves node and building-way addresses, loads a small tile neighborhood, and reports `route_status: found`. `rte-route` keeps the machine-readable key/value fields and also prints a human-readable walking summary with ANSI colors by default. Use `--no-color` when capturing output for scripts or logs.

Use `--json` to emit newline-delimited JSON events instead of the human/key-value text output. JSON mode disables terminal color and follows the `newos.tool.v1` envelope documented in `docs/json-output.md`. Successful walking routes stream `metadata`, `address`, `tile_context`, `graph_loaded`, `route_status`, `route`, `route_point`, and `route_step` events; GTFS-enabled packs also emit a `transit_plan` event, using the current departure time unless `--depart` or `--arrive` is supplied. Each route point includes latitude/longitude and cumulative distance, while each step includes action, distance, direction where available, and start/end coordinates. Diagnostics are written as JSON events on stderr.

```sh
build/freestanding-macos-arm64/rte-route data/brandenburg.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam" \
	--json
```

The macOS route CLI can also render a PNG map for a successful route:

```sh
build/freestanding-macos-arm64/rte-route data/brandenburg.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam" \
	--map build/route.png
```

`--map` writes the route path to a temporary `lon,lat` polyline and invokes the sibling `rpack-render` binary with a render pack. Pass `--rpack FILE.rpack` to choose the map layer explicitly; otherwise the tool first looks for a matching `.rpack` next to the `.rte`, then for known local packs such as `build/brandenburg-260525.rpack` and `data/germany.rpack`. Use `--width N` and/or `--height N` to control map dimensions; if only one dimension is provided, the renderer derives the other from the selected viewport. Cross-city bbox maps default to `--width 1600` and derive height from the route bbox. When both endpoints are in the same city, the map uses the renderer's `--city` viewport and still prints the computed route bbox as `map_bbox`.

The human-readable directions are currently coarse graph directions such as “continue generally north-west for 450 m”; edge street names and fuller snap-grid edge snapping remain future work. The builder currently duplicates cross-tile walking segments into both endpoint tiles and writes minimal snap-grid headers; endpoint snapping in `rte-route` uses nearest graph nodes.

## Fonts

The project vendors the freestanding TrueType backend from `~/fontrender` under `src/shared/fontrender`. The core remains dependency-free and is connected to this runtime by `fontrender_runtime_install()`. Full map labels are still future work.

A smoke test can load a `.ttf` and rasterize one glyph:

```sh
./build/freestanding-linux-x86_64/test-font /path/to/font.ttf A 32
```

It prints glyph metrics and the number of non-empty bitmap pixels.

## Documentation

Additional notes are in:

- `docs/threading.md`
- `docs/osm_streaming.md`
- `docs/osmrpack_format.md`
- `docs/OSMRTE01.md`

## Generation And License

The source code of this project is LLM-generated using GPT 5.5.

The project license is CC-0.