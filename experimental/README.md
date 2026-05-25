# Experimental OSM Render Pack Tools

The active render-pack tools have moved to the normal top-level build:

```text
build/freestanding-linux-x86_64/osmrenderpackv2
build/freestanding-linux-x86_64/osmrpackinfo
build/freestanding-linux-x86_64/osmrender-rpack
```

Use `make` from the repository root to build them. The old experimental v1 `osmrenderpack` path is superseded by the `OSMRPK02` builder and should not be used for new packs.

See `../docs/osmrpack_format.md` for the current format and usage notes.
