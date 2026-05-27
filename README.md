# pbf-parser

This project contains small freestanding C tools for reading and processing OpenStreetMap PBF files. The current focus is on inspecting PBF metadata, streaming OSM entities, building simple indexes, running lookups, and experimenting with bitmap rendering.

The tools are written for static Linux x86_64 builds without the standard C library. They use a small local runtime, direct platform syscall wrappers, and project-local parsing and compression code instead of external libraries.

## Scope

The repository currently provides:

- `pbfinfo`, which reads OSM PBF fileblocks and reports summary counts and metadata.
- `osmlookup`, which streams nodes, ways, and relations and filters them by type, tags, IDs, names, and bounding boxes.
- `osmaddresses`, which extracts address tags into TSV rows containing state, city, suburb, street, house number, and postcode fields.
- `osmnodeindex`, which builds a compact node coordinate index for way geometry lookup.
- `osmwayindex`, which builds a compact way-reference index for targeted relation rendering.
- `osmindex`, which builds node and way indexes together in one buffered pass through a PBF file.
- `osmrelindex`, which builds a compact administrative relation index for fast city-boundary lookup.
- `osmspindex`, which builds a way bounding-box index from node and way indexes for faster viewport filtering.
- `osmrender`, an experimental BMP/PNG renderer with bbox, relation-boundary, city-name, green-area, major-road rendering modes, and a font-rendered status footer.
- `osmrenderpackv2`, which builds `OSMRPK02` render-pack files from `.osm.pbf` extracts for faster repeated city/bbox rendering.
- `osmrpackinfo`, which prints render-pack header and directory metadata.
- `osmrender-rpack`, which renders PNGs from `.rpack` files using `--city` or `--bbox` without scanning the source PBF.
- `osmwalkroute`, which finds a walking route between two street addresses and can compare that result against a first-pass single-leg GTFS transit option for a departure time.
- `osmroutepack`, an `OSMRTE01` route-pack converter that writes the binary container, source counts, metric tile metadata, bounds, sorted tile directory records, address records, and tiled walking graph payloads.
- `osmrteinfo`, which inspects `OSMRTE01` route-pack files, prints header and section metadata, checks whether address and GTFS sections are present, and can look up the route tile for a latitude/longitude pair.
- `rtewalkroute`, which resolves two walking-route endpoint addresses from an `OSMRTE01` route pack, loads the endpoint tile neighborhood, snaps to walking graph nodes, and runs Dijkstra from the `.rte` file.
- `threadtest`, a small validation tool for the Linux nolibc threading layer.
- `fonttest`, a small smoke tool for the vendored TrueType renderer used by future map labels.

The project is not intended to be a complete GIS engine. It is a compact parser and tool collection for exploring OSM PBF data with minimal runtime dependencies.

## Architecture

The code is split into platform, runtime, shared parser, and tool layers:

- `src/arch` contains architecture-specific process startup and syscall entry code.
- `src/platform` contains operating-system wrappers for files, I/O, memory mapping, threads, time, identity, and process operations.
- `src/shared/runtime` contains the small runtime functions used instead of libc.
- `src/shared/compression` contains the local zlib/deflate implementation used for PBF blobs.
- `src/shared/pbf.c` and `src/shared/pbf.h` contain the OSM PBF and protobuf streaming parser.
- `src/shared/osm_index.c` and `src/shared/osm_index.h` contain the reusable node, way, relation, and spatial index formats and lookup code.
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

For the freestanding macOS arm64 router build used during Potsdam routing work:

```sh
make -B build/freestanding-macos-arm64/osmwalkroute
```

`make clean` removes the whole build directory, including generated OSM indexes. Recreate the binaries with `make`, then recreate any needed `.osm*idx` files before city rendering from a large extract.

## Data

Place local `.osm.pbf` files under `data/` when running the tools. Example:

```sh
./build/freestanding-linux-x86_64/pbfinfo data/example.osm.pbf
```

For city rendering, build the indexes once for the matching extract:

```sh
./build/freestanding-linux-x86_64/osmindex --progress data/germany-260524.osm.pbf build/germany.osmnidx build/germany.osmwidx
./build/freestanding-linux-x86_64/osmrelindex --progress data/germany-260524.osm.pbf build/germany.osmridx
./build/freestanding-linux-x86_64/osmspindex --progress build/germany.osmnidx build/germany.osmwidx build/germany.osmspidx
```

Then a short city render command uses sane defaults:

```sh
./build/freestanding-linux-x86_64/osmrender data/germany-260524.osm.pbf city.png --city Berlin
```

With `--city`, `osmrender` infers matching `build/<extract>.osmnidx`, `.osmwidx`, optional `.osmridx`, optional `.osmspidx`, the default style file, green-area rendering, major roads, aspect-aware output dimensions, and a brighter outside-boundary mask. If the inferred indexes are missing, it prints the commands needed to rebuild them.

By default, `osmrender` appends a small status footer below the map when the configured TrueType font is available. Footer font, colors, size, and text are configured with `footer.*` keys in `styles/osmrender-default.conf`; `--font FILE.ttf` overrides the configured font for one run, and `--no-status-footer` disables the footer for clean exports. Available footer variables are documented in `docs/osmrender_footer.md`.

For repeated renders from a large extract, build a render pack once and render from it:

```sh
./build/freestanding-linux-x86_64/osmrenderpackv2 --tile-zoom 10 --threads 8 data/germany-260524.osm.pbf build/germany.rpack
./build/freestanding-linux-x86_64/osmrender-rpack build/germany.rpack city.png --city Berlin --width 1600 --height 1200 --profile
```

`osmrender-rpack` can render directly from the pack-contained place directory, tile payloads, and embedded per-place boundary payloads. For `--city`, it draws the matching administrative boundary and fades pixels outside it without sidecar indexes; distant exclave components are excluded from the default viewport and can be shown with `--exclave-insets`. A route overlay can be drawn with `--route-polyline FILE`, where the file contains one `lon,lat` point per line. `.osmnidx`, `.osmwidx`, and `.osmridx` remain as a fallback for older packs. The pack format is documented in `docs/osmrpack_format.md`.

## Routing

`osmwalkroute` resolves the source and destination addresses from the OSM extract, builds a walkable graph from OSM ways, runs Dijkstra on that graph, and prints route statistics plus plain-language directions. With GTFS input and a faster transit option, the directions become multimodal: walk to the stop, take the selected tram/bus/train leg, then walk from the alighting stop to the destination.

Human-readable output uses ANSI colors by default for successful statuses, stops, lines, times, and direction step numbers. Use `--no-color` when capturing output for scripts or logs; `--color` can be used to re-enable color explicitly.

Walking-only example on macOS arm64:

```sh
build/freestanding-macos-arm64/osmwalkroute data/brandenburg-260525.osm.pbf \
	"Friedrich-Engels-Straße 22" "Hermann-Mattern-Promenade 25" \
	--city Potsdam
```

GTFS-aware departure-time comparison:

```sh
build/freestanding-macos-arm64/osmwalkroute data/brandenburg-260525.osm.pbf \
	"Friedrich-Engels-Straße 22" "Hermann-Mattern-Promenade 25" \
	--city Potsdam --gtfs data/GTFS --depart 2026-05-27T11:00
```

Current transit support is intentionally narrow:

- `--depart` is implemented.
- `--arrive` is parsed but not planned yet.
- The transit search currently evaluates a single transit leg plus walking at both ends.

The router also accepts `--threads N`. The node pass stays serial because it builds the shared coordinate index. When `N > 1`, the way pass is split into two phases: workers scan OSM ways in parallel and collect compact walkable segments, then the main thread materializes those segments into the route graph. This keeps graph mutation deterministic while moving way decoding and reference lookup off the single-threaded path.

Measured on macOS arm64 with:

```sh
build/freestanding-macos-arm64/osmwalkroute data/brandenburg-260525.osm.pbf \
	"Friedrich-Engels-Straße 22" "Hermann-Mattern-Promenade 25" \
	--city Potsdam [--gtfs data/GTFS --depart 2026-05-27T11:00] --threads N
```

Walking-only results after the way/graph restructuring:

- `--threads 1`: real 13.71s, user 13.10s, sys 0.28s
- `--threads 2`: real 13.41s, user 27.16s, sys 5.20s
- `--threads 4`: real 18.46s, user 68.72s, sys 7.89s
- `--threads 8`: real 24.25s, user 174.80s, sys 11.31s

GTFS comparison results:

- `--threads 1`: real 72.51s, user 71.64s, sys 0.66s
- `--threads 2`: real 72.41s, user 85.45s, sys 5.80s

The best observed walking-only run is currently `--threads 2`, but the improvement is small. GTFS queries remain dominated by the full `stop_times.txt` scan, so threading the way pass does not improve the end-to-end transit comparison yet.

The route-pack converter starts the move away from per-query source scans:

```sh
make -B build/freestanding-macos-arm64/osmroutepack
build/freestanding-macos-arm64/osmroutepack --tile-size-m 4000 --threads 2 \
	data/brandenburg-260525.osm.pbf build/brandenburg.rte
```

This milestone writes an `OSMRTE01` file with real metric route tiles, tiled walking nodes, edge-offset arrays, directed walking edges, minimal snap-grid records, and a first global address section for exact street/house lookup. GTFS route-pattern arrays, service-day bitsets, and optimized address dictionaries are still future work. The target format is documented in `docs/OSMRTE01.md`.

Inspect a generated route pack:

```sh
make -B build/freestanding-macos-arm64/osmrteinfo
build/freestanding-macos-arm64/osmrteinfo build/brandenburg-tiles.rte --sections
build/freestanding-macos-arm64/osmrteinfo build/brandenburg-tiles.rte \
	--address "Friedrich-Engels-Strasse 22"
build/freestanding-macos-arm64/osmrteinfo build/brandenburg-tiles.rte \
	--address "Hermann-Mattern-Promenade 25, Potsdam"
build/freestanding-macos-arm64/osmrteinfo build/brandenburg-tiles.rte \
	--tile 52.3906,13.0645
```

The current tiled milestone should report `addresses_present: yes` and `gtfs_present: no`. Address lookup does not require a tile coordinate; `--tile` is only a low-level tile-directory probe. Address queries use `street house` and may add `, city/suburb/postcode` to disambiguate. The inspector folds `Straße` and `Strasse` together for lookup. GTFS data still comes from the separate `data/GTFS` directory in the experimental `osmwalkroute` path; it is not embedded in `build/brandenburg-tiles.rte` yet.

The first route-pack walking CLI resolves endpoints from `.rte` data:

```sh
make -B build/freestanding-macos-arm64/rtewalkroute
build/freestanding-macos-arm64/rtewalkroute data/brandenburg.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam"
```

For the Potsdam sample above, the current route pack resolves node and building-way addresses, loads a small tile neighborhood, and reports `route_status: found`. `rtewalkroute` keeps the machine-readable key/value fields and also prints a human-readable walking summary with ANSI colors by default. Use `--no-color` when capturing output for scripts or logs.

The macOS route CLI can also render a PNG map for a successful route:

```sh
build/freestanding-macos-arm64/rtewalkroute data/brandenburg.rte \
	"Friedrich-Engels-Straße 22, Potsdam" \
	"Hermann-Mattern-Promenade 25, Potsdam" \
	--map build/route.png
```

`--map` writes the route path to a temporary `lon,lat` polyline and invokes the sibling `osmrender-rpack` binary with a render pack. Pass `--rpack FILE.rpack` to choose the map layer explicitly; otherwise the tool first looks for a matching `.rpack` next to the `.rte`, then for known local packs such as `build/brandenburg-260525.rpack` and `data/germany.rpack`. When both endpoints are in the same city, the map uses the renderer's `--city` viewport and still prints the computed route bbox as `map_bbox`.

The human-readable directions are currently coarse graph directions such as “continue generally north-west for 450 m”; edge street names and fuller snap-grid edge snapping remain future work. The builder currently duplicates cross-tile walking segments into both endpoint tiles and writes minimal snap-grid headers; endpoint snapping in `rtewalkroute` uses nearest graph nodes.

## Fonts

The project vendors the freestanding TrueType backend from `~/fontrender` under `src/shared/fontrender`. The core remains dependency-free and is connected to this runtime by `fontrender_runtime_install()`. `osmrender` uses it for the diagnostic footer; full map labels are still future work.

A smoke test can load a `.ttf` and rasterize one glyph:

```sh
./build/freestanding-linux-x86_64/fonttest /path/to/font.ttf A 32
```

It prints glyph metrics and the number of non-empty bitmap pixels.

## Documentation

Additional notes are in:

- `docs/threading.md`
- `docs/osm_streaming.md`
- `docs/osm_rendering.md`
- `docs/osmrender_footer.md`
- `docs/osmrpack_format.md`
- `docs/OSMRTE01.md`
- `docs/berlin_green_data.md`

## Generation And License

The source code of this project is LLM-generated using GPT 5.5.

The project license is CC-0.