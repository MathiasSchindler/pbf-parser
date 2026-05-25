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
- `osmrender`, an experimental BMP/PNG renderer with bbox, relation-boundary, city-name, green-area, and major-road rendering modes.
- `threadtest`, a small validation tool for the Linux nolibc threading layer.

The project is not intended to be a complete GIS engine. It is a compact parser and tool collection for exploring OSM PBF data with minimal runtime dependencies.

## Architecture

The code is split into platform, runtime, shared parser, and tool layers:

- `src/arch` contains architecture-specific process startup and syscall entry code.
- `src/platform` contains operating-system wrappers for files, I/O, memory mapping, threads, time, identity, and process operations.
- `src/shared/runtime` contains the small runtime functions used instead of libc.
- `src/shared/compression` contains the local zlib/deflate implementation used for PBF blobs.
- `src/shared/pbf.c` and `src/shared/pbf.h` contain the OSM PBF and protobuf streaming parser.
- `src/shared/osm_index.c` and `src/shared/osm_index.h` contain the reusable node, way, relation, and spatial index formats and lookup code.
- `src/tools` contains the command-line tools built on top of the shared layers.

The parser reads PBF fileblocks, decodes `OSMHeader` and `OSMData` blobs, inflates zlib-compressed payloads, parses protobuf fields, and exposes decoded OSM entities through streaming callbacks. Consumers can skip node tags or prefilter way and relation tags when they only need coordinates, IDs, renderable ways, or tag-only records.

## Build

The default build uses `gcc-16` and writes static binaries to `build/freestanding-linux-x86_64/`:

```sh
make all
```

The build output and local PBF data files are intentionally ignored by git.

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

## Documentation

Additional notes are in:

- `docs/threading.md`
- `docs/osm_streaming.md`
- `docs/osm_rendering.md`
- `docs/berlin_green_data.md`

## Generation And License

The source code of this project is LLM-generated using GPT 5.5.

The project license is CC-0.