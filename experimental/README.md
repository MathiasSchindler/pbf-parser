# Experimental OSM Render Pack Tools

This directory is a sandbox for render-pack experiments. It is deliberately separate from the stable top-level tools and build rules so the current working parser, indexers, and renderer stay usable while the pack format changes.

## Tools

- `osmrenderpack`: builds an experimental `.osmrpack` file from a `.osm.pbf` source. The current v1 builder writes way-derived render geometry for roads, rail, water, forest, and park features. Buildings are skipped by default and can be included with `--buildings`.
- `osmrpackinfo`: prints the header fields from a pack.
- `osmrenderpackrender`: renderer entry point. It reads the pack, collects visible geometry for the requested bbox/city, and writes a PNG without reading the source PBF. For `--city`, it can also use matching `.osmnidx`, `.osmwidx`, and `.osmridx` files to draw the city boundary and fade areas outside it.

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
./build/freestanding-linux-x86_64/osmrpackinfo ../build/brandenburg.experimental.osmrpack
./build/freestanding-linux-x86_64/osmrenderpackrender ../build/brandenburg.experimental.osmrpack ../build/potsdam-pack.png --city Potsdam --width 1600 --height 1200
```

When the pack name matches existing indexes, `osmrenderpackrender --city` infers the index paths by replacing `.osmrpack` with `.osmnidx`, `.osmwidx`, and `.osmridx`. With those indexes available, the city bbox is derived from the administrative boundary before the pack is scanned, and the same boundary is used for the outside fade. Otherwise pass the index paths explicitly:

```sh
./build/freestanding-linux-x86_64/osmrenderpackrender ../build/germany.osmrpack ../build/potsdam-pack-boundary-fade.png --city Potsdam --width 1600 --height 1200 --node-index ../build/germany.osmnidx --way-index ../build/germany.osmwidx --relation-index ../build/germany.osmridx
```

PNG output uses adaptive scanline filters, fixed-Huffman deflate, and an indexed palette by default. Pass `--png-rgb` to keep exact RGB pixels while still using PNG filtering and compression.

Measured on this machine with `data/brandenburg-260524.osm.pbf`, the no-buildings pack build took `47.17s` wall time and produced a `254 MB` pack with `1,530,306` features and `12,002,860` points. Rendering Potsdam from that pack at `1600x1200` took `1.62s` wall time.

## Format Documentation

See `docs/osmrpack_format.md` for the current file layout and planned tile/geometry payload design.

## Intended Direction

The pack should become a render-friendly derived cache for Germany-scale extracts:

1. Keep `data/germany-*.osm.pbf` as canonical input.
2. Spend a bounded preparation step building `build/germany.osmrpack`.
3. Let experimental renders jump directly to the tiles intersecting the requested city/bbox.
4. Avoid full PBF scans during repeated renders.

The current pack uses one flat payload record rather than real spatial tiles. The next milestone is to partition the feature payload by tile so large Germany-scale packs do not need a full feature scan for every city render.
