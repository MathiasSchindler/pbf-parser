# osmrender Footer Configuration

`osmrender` can append a small status footer below the map image. The footer is configured in the same style file as map colors, normally `styles/osmrender-default.conf`.

The footer is added after rendering, so `{map_width}` and `{map_height}` describe the actual map area while `{image_width}` and `{image_height}` describe the final file including the footer.

## Settings

```conf
footer.enabled = yes
footer.font = data/fonts/Roboto-Regular.ttf
footer.font_size = 12
footer.height = 32
footer.background = 255,255,255
footer.text_color = 0,0,0
footer.rule = 210,210,210
footer.text = map={map_width}x{map_height} img={image_width}x{image_height} t={render_time_ms}ms n={nodes_in_bbox} w={ways_drawn}/{ways_decoded} r={relations_seen} p={polygons_collected} s={segments_drawn}
```

- `footer.enabled`: `yes`/`no`, `true`/`false`, `on`/`off`, or `1`/`0`.
- `footer.font`: path to a TrueType font. `--font FILE.ttf` still overrides this for one run.
- `footer.font_size`: glyph pixel size.
- `footer.height`: footer band height in pixels.
- `footer.background`: RGB footer background color.
- `footer.text_color`: RGB text color.
- `footer.rule`: RGB one-pixel separator line color at the top of the footer.
- `footer.text`: literal text with `{variable}` placeholders.

Use `{{` and `}}` to emit literal braces in `footer.text`. Unknown placeholders are kept as `{name}` in the rendered footer so mistakes remain visible.

The command-line flag `--no-status-footer` disables the footer regardless of the style file.

## Variables

Dimensions:

- `{map_width}`: width of the rendered map area before the footer is appended.
- `{map_height}`: height of the rendered map area before the footer is appended.
- `{image_width}`: final output image width.
- `{image_height}`: final output image height including the footer.
- `{width}`: alias for `{image_width}`.
- `{height}`: alias for `{image_height}`.

Render identity and time:

- `{city}`: city name passed with `--city`, or empty for bbox renders.
- `{render_time_ms}`: elapsed renderer time in milliseconds before writing the image.

Object counters:

- `{nodes_in_bbox}`: nodes kept for the render bbox. In lazy large-index renders, this counts indexed nodes resolved for selected render geometry, not every node physically inside the bbox.
- `{ways_seen}`: ways encountered or selected for decoding.
- `{ways_decoded}`: ways decoded far enough for rendering.
- `{ways_drawn}`: ways that produced visible render geometry.
- `{segments_drawn}`: line segments drawn from ways and relation members.
- `{segments_collected}`: line segments retained in the render layer buffers.
- `{tree_nodes_drawn}`: tree point nodes drawn.
- `{relations_seen}`: relations scanned or selected.
- `{relation_members_collected}`: relation member references collected.
- `{relation_ways_matched}`: relation member ways matched through the way index.
- `{way_refs_skipped}`: way references skipped because they exceeded configured limits or missing data.
- `{polygons_collected}`: polygon records retained in the render layer buffers.
- `{visible_pixels}`: non-background pixels counted before the footer is appended.

Mode and index flags:

- `{node_index}`: `yes` when a node index is open.
- `{lazy_node_index}`: `yes` when a large node index is used lazily instead of streaming all bbox nodes from the PBF.
- `{way_index}`: `yes` when a way index is open.
- `{relation_index}`: `yes` when a relation index is open.
- `{spatial_index}`: `yes` when a spatial way index is open.
- `{bounded}`: `yes` for explicit `--bbox` renders.
- `{green_only}`: `yes` when non-green styles are suppressed.
- `{major_roads}`: `yes` when major roads are included with green rendering.
- `{no_fills}`: `yes` when polygon fills are disabled.
- `{relation_scan}`: `yes` unless `--no-relation-scan` was used.
- `{boundary_fade}`: `yes` when the outside-city fade was applied.

## Performance Note

On Germany-wide city renders with all current indexes, the slow part is no longer drawing the map geometry. The spatial index cuts way decoding down to the city viewport, relation-member spatial filtering keeps only local relation ways, and lazy node-index lookup avoids streaming every Germany node for large indexes. The measured Potsdam command dropped from about 140 seconds to about 90 seconds. Getting from there toward 5-10 seconds likely needs one of these larger changes:

- a compact way-style/tag index so roads, water, forests, parks, and buildings can be selected without streaming the full PBF;
- a relation/member geometry index for renderable multipolygons, not only administrative city lookup;
- tiled or bbox-partitioned node/way lookup so Germany-scale random reads are localized;
- parallel PBF blob decompression and parsing if full-file streaming remains necessary.

The first two are the most direct path to interactive city renders because they remove most of the full Germany PBF scan instead of making that scan faster.