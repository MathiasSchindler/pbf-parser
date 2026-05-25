# Experimental OSM Render Pack Tools

This directory is a sandbox for render-pack experiments. It is deliberately separate from the stable top-level tools and build rules so the current working parser, indexers, and renderer stay usable while the pack format changes.

## Tools

- `osmrenderpack`: builds an experimental `.osmrpack` file from a `.osm.pbf` source. The current v1 builder writes way-derived render geometry for roads, rail, water, forest, and park features. Buildings are skipped by default and can be included with `--buildings`.
- `osmrenderpackv2`: builds an `OSMRPK02` `.rpack` file with a bbox-only place directory and duplicated per-tile feature payloads. It reads the source PBF directly and accepts `--tile-zoom`, `--tile-halo`, and `--buildings`.
- `osmrpackinfo`: prints the header fields from a pack.
- `osmrenderpackrender`: renderer entry point. It reads the pack, collects visible geometry for the requested bbox/city, and writes a PNG without reading the source PBF. For `--city`, v2 packs can resolve arbitrary UTF-8 city names from the pack place directory; matching `.osmnidx`, `.osmwidx`, and `.osmridx` files add exact city boundaries and outside-boundary fade.

## Build

```sh
cd experimental
make
```

Binaries are written to:

```text
experimental/build/freestanding-linux-x86_64/
```

## Smoke Test

```sh
cd experimental
./build/freestanding-linux-x86_64/osmrenderpack ../data/brandenburg-260524.osm.pbf ../build/brandenburg.experimental.osmrpack
./build/freestanding-linux-x86_64/osmrenderpackv2 --tile-zoom 10 ../data/brandenburg-260524.osm.pbf ../build/brandenburg-v2.rpack
./build/freestanding-linux-x86_64/osmrpackinfo ../build/brandenburg.experimental.osmrpack
./build/freestanding-linux-x86_64/osmrenderpackrender ../build/brandenburg.experimental.osmrpack ../build/potsdam-pack.png --city Potsdam --width 1600 --height 1200
```

A typical Germany v2 build is:

```sh
./build/freestanding-linux-x86_64/osmrenderpackv2 --tile-zoom 10 ../data/germany-260524.osm.pbf ../build/germany.rpack
```

The v2 renderer reads only the tile payloads intersecting the requested bbox or resolved city bbox. City names are compared as UTF-8 byte strings, so names such as `Lübeck` work when they exist in the pack or relation index:

```sh
./build/freestanding-linux-x86_64/osmrenderpackrender ../build/germany.rpack ../build/luebeck-germany-rpack-v2.png --city Lübeck --width 4000 --height 3000 --node-index ../build/germany.osmnidx --way-index ../build/germany.osmwidx --relation-index ../build/germany.osmridx --profile
```

When the pack name matches existing indexes, `osmrenderpackrender --city` infers the index paths by replacing `.osmrpack` with `.osmnidx`, `.osmwidx`, and `.osmridx`. With those indexes available, the city bbox is derived from the administrative boundary before the pack is scanned, and the same boundary is used for the outside fade. Otherwise pass the index paths explicitly:

```sh
./build/freestanding-linux-x86_64/osmrenderpackrender ../build/germany.osmrpack ../build/potsdam-pack-boundary-fade.png --city Potsdam --width 1600 --height 1200 --node-index ../build/germany.osmnidx --way-index ../build/germany.osmwidx --relation-index ../build/germany.osmridx
```

PNG output uses adaptive scanline filters, fixed-Huffman deflate, and an indexed palette by default. Pass `--png-rgb` to keep exact RGB pixels while still using PNG filtering and compression.

Pass `--profile` to print phase timings for city bbox resolution, feature collection, boundary loading, layer drawing, and PNG writing. The scan counters distinguish features/points inspected in the selected pack payloads from those retained for drawing, and report how many bytes/refills the buffered pack scanner consumed.

Measured on this machine with `data/brandenburg-260524.osm.pbf`, the no-buildings pack build took `47.17s` wall time and produced a `254 MB` pack with `1,530,306` features and `12,002,860` points. Rendering Potsdam from that pack at `1600x1200` took `1.62s` wall time.

## Format Documentation

See `docs/osmrpack_format.md` for the current file layout and planned tile/geometry payload design.

## Intended Direction

The pack should become a render-friendly derived cache for Germany-scale extracts:

1. Keep `data/germany-*.osm.pbf` as canonical input.
2. Spend a bounded preparation step building `build/germany.osmrpack` or `build/germany.rpack`.
3. Let experimental renders jump directly to the tiles intersecting the requested city/bbox.
4. Avoid full PBF scans during repeated renders.

The v1 pack uses one flat payload record rather than real spatial tiles. The v2 builder writes a bbox/name place directory and true non-empty tile records, with each feature copied into every tile touched by its bbox. The v2 renderer reads only the tile payloads intersecting the requested city or bbox and de-duplicates overlapping feature copies during collection.
