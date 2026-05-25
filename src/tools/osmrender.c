#include "pbf.h"
#include "osm_index.h"
#include "platform.h"
#include "runtime.h"
#include "simple_config.h"

typedef struct OsmRenderStyleSheet OsmRenderStyleSheet;

typedef struct {
    long long id;
    long long lat_nano;
    long long lon_nano;
    int used;
} OsmRenderNodeEntry;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    unsigned int style_id;
} OsmRenderSegment;

typedef struct {
    unsigned int point_offset;
    unsigned int point_count;
    unsigned int style_id;
} OsmRenderPolygon;

typedef struct {
    long long id;
    unsigned int style_id;
    int used;
} OsmRenderRelationWayEntry;

typedef struct {
    long long id;
    unsigned int style_id;
} OsmRenderRelationWayRef;

typedef struct {
    OsmWayIndexRecord record;
    unsigned int style_id;
} OsmRenderIndexedWayCandidate;

typedef struct {
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    unsigned int width;
    unsigned int height;
    unsigned char *pixels;
    OsmRenderNodeEntry *nodes;
    OsmNodeIndex node_index;
    OsmWayIndex way_index;
    OsmNodeIndexRecord *node_index_records;
    OsmRenderSegment *segments;
    OsmRenderPolygon *polygons;
    OsmRenderRelationWayEntry *relation_ways;
    OsmRenderRelationWayRef *relation_way_order;
    int *polygon_points;
    unsigned int node_capacity;
    unsigned int node_count;
    unsigned long long node_index_record_count;
    unsigned int segment_capacity;
    unsigned int segment_count;
    unsigned int polygon_capacity;
    unsigned int polygon_count;
    unsigned int relation_way_capacity;
    unsigned int relation_way_count;
    unsigned int polygon_point_capacity;
    unsigned int polygon_point_count;
    unsigned long long ways_seen;
    unsigned long long ways_decoded;
    unsigned long long ways_drawn;
    unsigned long long segments_drawn;
    unsigned long long tree_nodes_drawn;
    unsigned long long relations_seen;
    unsigned long long relation_members_collected;
    unsigned long long relation_ways_matched;
    unsigned long long way_refs_skipped;
    unsigned long long visible_pixels;
    unsigned int stop_after_nodes;
    unsigned int stop_after_trees;
    unsigned int stop_after_ways;
    unsigned int stop_after_drawn;
    unsigned int max_way_refs;
    long long relation_filter_id;
    const char *node_index_path;
    const char *way_index_path;
    const OsmRenderStyleSheet *style_sheet;
    unsigned int render_step;
    int node_points;
    int tree_points;
    int green_only;
    int no_fills;
    int relation_filter_enabled;
    int node_index_open;
    int way_index_open;
    int stopped_after_nodes;
    int stopped_after_trees;
    int stopped_after_ways;
    int stopped_after_drawn;
    int failed;
} OsmRenderContext;

typedef struct {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char alpha;
    unsigned char fill_r;
    unsigned char fill_g;
    unsigned char fill_b;
    unsigned char fill_alpha;
    unsigned char casing_r;
    unsigned char casing_g;
    unsigned char casing_b;
    unsigned char casing_alpha;
    unsigned int width;
    unsigned int casing_width;
    unsigned int flags;
} OsmRenderStyle;

#define OSM_RENDER_STYLE_LINE   (1U << 0)
#define OSM_RENDER_STYLE_FILL   (1U << 1)
#define OSM_RENDER_STYLE_CASING (1U << 2)

#define OSM_RENDER_STEP_AREA   0U
#define OSM_RENDER_STEP_CASING 1U
#define OSM_RENDER_STEP_LINE   2U

typedef enum {
    OSM_RENDER_STYLE_WATER = 0,
    OSM_RENDER_STYLE_WATERWAY,
    OSM_RENDER_STYLE_FOREST,
    OSM_RENDER_STYLE_PARK,
    OSM_RENDER_STYLE_BUILDING,
    OSM_RENDER_STYLE_MOTORWAY,
    OSM_RENDER_STYLE_PRIMARY,
    OSM_RENDER_STYLE_SECONDARY,
    OSM_RENDER_STYLE_MINOR_ROAD,
    OSM_RENDER_STYLE_PATH,
    OSM_RENDER_STYLE_RAIL,
    OSM_RENDER_STYLE_COUNT
} OsmRenderStyleId;

struct OsmRenderStyleSheet {
    unsigned char background_r;
    unsigned char background_g;
    unsigned char background_b;
    OsmRenderStyle styles[OSM_RENDER_STYLE_COUNT];
};

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf OUT.bmp --bbox MINLON,MINLAT,MAXLON,MAXLAT [--width N] [--height N] [--style FILE] [--node-index FILE] [--way-index FILE] [--green-only] [--no-fills] [--max-way-refs N] [--relation-id ID] [--node-points] [--tree-points] [--stop-after-nodes N] [--stop-after-trees N] [--stop-after-ways N] [--stop-after-drawn N]\n");
}

static int parse_uint_arg(const char *text, unsigned int *value_out) {
    unsigned long long value = 0ULL;
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') return -1;
    while (text[index] != '\0') {
        unsigned long long previous;
        if (text[index] < '0' || text[index] > '9') return -1;
        previous = value;
        value = value * 10ULL + (unsigned long long)(text[index] - '0');
        if (value < previous || value > 8192ULL) return -1;
        index += 1U;
    }
    if (value == 0ULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_id_arg(const char *text, long long *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > 9223372036854775807ULL || value == 0ULL) return -1;
    *value_out = (long long)value;
    return 0;
}

static int parse_coord_part(const char *text, size_t size, long long *value_out) {
    size_t index = 0U;
    unsigned long long whole = 0ULL;
    unsigned long long fraction = 0ULL;
    unsigned int fraction_digits = 0U;
    int negative = 0;

    if (size == 0U) return -1;
    if (text[index] == '-') {
        negative = 1;
        index += 1U;
    }
    if (index >= size || text[index] < '0' || text[index] > '9') return -1;
    while (index < size && text[index] >= '0' && text[index] <= '9') {
        whole = whole * 10ULL + (unsigned long long)(text[index] - '0');
        if (whole > 180ULL) return -1;
        index += 1U;
    }
    if (index < size && text[index] == '.') {
        index += 1U;
        while (index < size && text[index] >= '0' && text[index] <= '9') {
            if (fraction_digits >= 9U) return -1;
            fraction = fraction * 10ULL + (unsigned long long)(text[index] - '0');
            fraction_digits += 1U;
            index += 1U;
        }
    }
    if (index != size) return -1;
    while (fraction_digits < 9U) {
        fraction *= 10ULL;
        fraction_digits += 1U;
    }
    *value_out = (long long)(whole * 1000000000ULL + fraction);
    if (negative) *value_out = -*value_out;
    return 0;
}

static int parse_bbox_arg(const char *text, OsmRenderContext *context) {
    const char *parts[4];
    size_t sizes[4];
    size_t start = 0U;
    size_t index = 0U;
    unsigned int part = 0U;

    for (;;) {
        if (text[index] == ',' || text[index] == '\0') {
            if (part >= 4U) return -1;
            parts[part] = text + start;
            sizes[part] = index - start;
            part += 1U;
            if (text[index] == '\0') break;
            start = index + 1U;
        }
        index += 1U;
    }
    if (part != 4U) return -1;
    if (parse_coord_part(parts[0], sizes[0], &context->min_lon_nano) != 0 ||
        parse_coord_part(parts[1], sizes[1], &context->min_lat_nano) != 0 ||
        parse_coord_part(parts[2], sizes[2], &context->max_lon_nano) != 0 ||
        parse_coord_part(parts[3], sizes[3], &context->max_lat_nano) != 0) {
        return -1;
    }
    return context->min_lon_nano < context->max_lon_nano && context->min_lat_nano < context->max_lat_nano ? 0 : -1;
}

static int text_equals(PbfText text, const char *value) {
    size_t value_size = rt_strlen(value);
    return text.size == value_size && memcmp(text.data, value, value_size) == 0;
}

static const PbfText *find_tag_value_in_tags(const PbfTag *tags, unsigned int tag_count, const char *key) {
    unsigned int index;

    for (index = 0U; index < tag_count; ++index) {
        if (text_equals(tags[index].key, key)) return &tags[index].value;
    }
    return 0;
}

static const PbfText *find_tag_value(const PbfWay *way, const char *key) {
    return find_tag_value_in_tags(way->tags, way->tag_count, key);
}

static int tag_value_equals(const PbfText *value, const char *expected) {
    return value != 0 && text_equals(*value, expected);
}

static void set_line_style(OsmRenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned int width) {
    style->r = red;
    style->g = green;
    style->b = blue;
    style->alpha = 255U;
    style->width = width;
    style->flags |= OSM_RENDER_STYLE_LINE;
}

static void set_fill_style(OsmRenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
    style->fill_r = red;
    style->fill_g = green;
    style->fill_b = blue;
    style->fill_alpha = alpha;
    style->flags |= OSM_RENDER_STYLE_FILL;
}

static void set_casing_style(OsmRenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned int width) {
    style->casing_r = red;
    style->casing_g = green;
    style->casing_b = blue;
    style->casing_alpha = 220U;
    style->casing_width = width;
    style->flags |= OSM_RENDER_STYLE_CASING;
}

static void style_sheet_init(OsmRenderStyleSheet *style_sheet) {
    rt_memset(style_sheet, 0, sizeof(*style_sheet));
    style_sheet->background_r = 242U;
    style_sheet->background_g = 239U;
    style_sheet->background_b = 232U;
    set_fill_style(&style_sheet->styles[OSM_RENDER_STYLE_WATER], 154U, 196U, 214U, 230U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_WATER], 94U, 151U, 183U, 2U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_WATERWAY], 92U, 158U, 194U, 2U);
    set_fill_style(&style_sheet->styles[OSM_RENDER_STYLE_FOREST], 176U, 205U, 165U, 190U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_FOREST], 134U, 168U, 121U, 1U);
    set_fill_style(&style_sheet->styles[OSM_RENDER_STYLE_PARK], 190U, 218U, 170U, 185U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_PARK], 142U, 181U, 119U, 1U);
    set_fill_style(&style_sheet->styles[OSM_RENDER_STYLE_BUILDING], 217U, 203U, 184U, 175U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_BUILDING], 157U, 138U, 117U, 1U);
    set_casing_style(&style_sheet->styles[OSM_RENDER_STYLE_MOTORWAY], 196U, 126U, 92U, 7U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_MOTORWAY], 238U, 171U, 105U, 5U);
    set_casing_style(&style_sheet->styles[OSM_RENDER_STYLE_PRIMARY], 202U, 145U, 98U, 6U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_PRIMARY], 246U, 194U, 118U, 4U);
    set_casing_style(&style_sheet->styles[OSM_RENDER_STYLE_SECONDARY], 198U, 176U, 121U, 5U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_SECONDARY], 246U, 226U, 148U, 3U);
    set_casing_style(&style_sheet->styles[OSM_RENDER_STYLE_MINOR_ROAD], 190U, 182U, 169U, 4U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_MINOR_ROAD], 250U, 248U, 240U, 2U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_PATH], 147U, 139U, 122U, 1U);
    set_casing_style(&style_sheet->styles[OSM_RENDER_STYLE_RAIL], 222U, 218U, 210U, 3U);
    set_line_style(&style_sheet->styles[OSM_RENDER_STYLE_RAIL], 92U, 91U, 101U, 1U);
}

static void copy_style(const OsmRenderStyleSheet *style_sheet, OsmRenderStyleId style_id, OsmRenderStyle *style_out) {
    *style_out = style_sheet->styles[style_id];
}

static int classify_tags_id(const PbfTag *tags, unsigned int tag_count, OsmRenderStyleId *style_id_out) {
    const PbfText *highway;
    const PbfText *railway;
    const PbfText *waterway;
    const PbfText *natural;
    const PbfText *landuse;
    const PbfText *leisure;
    const PbfText *building;

    highway = find_tag_value_in_tags(tags, tag_count, "highway");
    railway = find_tag_value_in_tags(tags, tag_count, "railway");
    waterway = find_tag_value_in_tags(tags, tag_count, "waterway");
    natural = find_tag_value_in_tags(tags, tag_count, "natural");
    landuse = find_tag_value_in_tags(tags, tag_count, "landuse");
    leisure = find_tag_value_in_tags(tags, tag_count, "leisure");
    building = find_tag_value_in_tags(tags, tag_count, "building");

    if (tag_value_equals(natural, "water") || tag_value_equals(waterway, "riverbank")) {
        *style_id_out = OSM_RENDER_STYLE_WATER;
        return 1;
    }
    if (waterway != 0) {
        *style_id_out = OSM_RENDER_STYLE_WATERWAY;
        return 1;
    }
    if (tag_value_equals(landuse, "forest") || tag_value_equals(landuse, "orchard") || tag_value_equals(natural, "wood") ||
        tag_value_equals(natural, "scrub") || tag_value_equals(natural, "tree_row")) {
        *style_id_out = OSM_RENDER_STYLE_FOREST;
        return 1;
    }
    if (tag_value_equals(leisure, "park") || tag_value_equals(leisure, "garden") || tag_value_equals(leisure, "nature_reserve") ||
        tag_value_equals(landuse, "grass") || tag_value_equals(landuse, "meadow") || tag_value_equals(landuse, "recreation_ground") ||
        tag_value_equals(landuse, "village_green") || tag_value_equals(natural, "grassland") || tag_value_equals(natural, "heath")) {
        *style_id_out = OSM_RENDER_STYLE_PARK;
        return 1;
    }
    if (building != 0) {
        *style_id_out = OSM_RENDER_STYLE_BUILDING;
        return 1;
    }
    if (highway != 0) {
        if (tag_value_equals(highway, "motorway") || tag_value_equals(highway, "trunk")) {
            *style_id_out = OSM_RENDER_STYLE_MOTORWAY;
        } else if (tag_value_equals(highway, "primary")) {
            *style_id_out = OSM_RENDER_STYLE_PRIMARY;
        } else if (tag_value_equals(highway, "secondary") || tag_value_equals(highway, "tertiary")) {
            *style_id_out = OSM_RENDER_STYLE_SECONDARY;
        } else if (tag_value_equals(highway, "footway") || tag_value_equals(highway, "path") || tag_value_equals(highway, "cycleway") || tag_value_equals(highway, "track")) {
            *style_id_out = OSM_RENDER_STYLE_PATH;
        } else {
            *style_id_out = OSM_RENDER_STYLE_MINOR_ROAD;
        }
        return 1;
    }
    if (railway != 0) {
        *style_id_out = OSM_RENDER_STYLE_RAIL;
        return 1;
    }
    return 0;
}

static int classify_way_id(const PbfWay *way, OsmRenderStyleId *style_id_out) {
    return classify_tags_id(way->tags, way->tag_count, style_id_out);
}

static int classify_way(const OsmRenderStyleSheet *style_sheet, const PbfWay *way, OsmRenderStyle *style_out, OsmRenderStyleId *style_id_out) {
    if (!classify_way_id(way, style_id_out)) {
        rt_memset(style_out, 0, sizeof(*style_out));
        return 0;
    }
    copy_style(style_sheet, *style_id_out, style_out);
    return 1;
}

static int style_id_is_green_context(OsmRenderStyleId style_id) {
    return style_id == OSM_RENDER_STYLE_WATER || style_id == OSM_RENDER_STYLE_WATERWAY ||
           style_id == OSM_RENDER_STYLE_FOREST || style_id == OSM_RENDER_STYLE_PARK;
}

static int node_in_bbox(const OsmRenderContext *context, long long lat_nano, long long lon_nano) {
    return lon_nano >= context->min_lon_nano && lon_nano <= context->max_lon_nano &&
           lat_nano >= context->min_lat_nano && lat_nano <= context->max_lat_nano;
}

static unsigned long long hash_id(long long id) {
    unsigned long long value = (unsigned long long)id;

    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    return value;
}

static int node_map_insert_raw(OsmRenderNodeEntry *nodes, unsigned int capacity, long long id, long long lat_nano, long long lon_nano) {
    unsigned int slot = (unsigned int)(hash_id(id) & (unsigned long long)(capacity - 1U));

    for (;;) {
        if (!nodes[slot].used || nodes[slot].id == id) {
            nodes[slot].used = 1;
            nodes[slot].id = id;
            nodes[slot].lat_nano = lat_nano;
            nodes[slot].lon_nano = lon_nano;
            return 0;
        }
        slot = (slot + 1U) & (capacity - 1U);
    }
}

static int node_map_grow(OsmRenderContext *context, unsigned int needed) {
    unsigned int capacity = context->node_capacity == 0U ? 4096U : context->node_capacity;
    OsmRenderNodeEntry *nodes;
    unsigned int index;

    while (capacity < needed * 2U) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->node_capacity) return 0;
    nodes = (OsmRenderNodeEntry *)rt_malloc(sizeof(OsmRenderNodeEntry) * (size_t)capacity);
    if (nodes == 0) return -1;
    rt_memset(nodes, 0, sizeof(OsmRenderNodeEntry) * (size_t)capacity);
    for (index = 0U; index < context->node_capacity; ++index) {
        if (context->nodes[index].used) {
            node_map_insert_raw(nodes, capacity, context->nodes[index].id, context->nodes[index].lat_nano, context->nodes[index].lon_nano);
        }
    }
    rt_free(context->nodes);
    context->nodes = nodes;
    context->node_capacity = capacity;
    return 0;
}

static int node_map_insert(OsmRenderContext *context, long long id, long long lat_nano, long long lon_nano) {
    if (node_map_grow(context, context->node_count + 1U) != 0) return -1;
    if (node_map_insert_raw(context->nodes, context->node_capacity, id, lat_nano, lon_nano) != 0) return -1;
    context->node_count += 1U;
    return 0;
}

static int node_map_find(const OsmRenderContext *context, long long id, OsmRenderNodeEntry *node_out) {
    unsigned int slot;

    if (context->node_capacity == 0U) return 0;
    slot = (unsigned int)(hash_id(id) & (unsigned long long)(context->node_capacity - 1U));
    for (;;) {
        if (!context->nodes[slot].used) return 0;
        if (context->nodes[slot].id == id) {
            *node_out = context->nodes[slot];
            return 1;
        }
        slot = (slot + 1U) & (context->node_capacity - 1U);
    }
}

static int relation_way_insert_raw(OsmRenderRelationWayEntry *entries, unsigned int capacity, long long id, unsigned int style_id) {
    unsigned int slot = (unsigned int)(hash_id(id) & (unsigned long long)(capacity - 1U));

    for (;;) {
        if (!entries[slot].used || entries[slot].id == id) {
            entries[slot].used = 1;
            entries[slot].id = id;
            entries[slot].style_id = style_id;
            return 0;
        }
        slot = (slot + 1U) & (capacity - 1U);
    }
}

static int relation_way_grow(OsmRenderContext *context, unsigned int needed) {
    unsigned int capacity = context->relation_way_capacity == 0U ? 1024U : context->relation_way_capacity;
    OsmRenderRelationWayEntry *entries;
    OsmRenderRelationWayRef *order;
    unsigned int index;

    while (capacity < needed * 2U) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->relation_way_capacity) return 0;
    entries = (OsmRenderRelationWayEntry *)rt_malloc(sizeof(OsmRenderRelationWayEntry) * (size_t)capacity);
    if (entries == 0) return -1;
    order = (OsmRenderRelationWayRef *)rt_realloc(context->relation_way_order, sizeof(OsmRenderRelationWayRef) * (size_t)capacity);
    if (order == 0) {
        rt_free(entries);
        return -1;
    }
    rt_memset(entries, 0, sizeof(OsmRenderRelationWayEntry) * (size_t)capacity);
    for (index = 0U; index < context->relation_way_capacity; ++index) {
        if (context->relation_ways[index].used) {
            relation_way_insert_raw(entries, capacity, context->relation_ways[index].id, context->relation_ways[index].style_id);
        }
    }
    rt_free(context->relation_ways);
    context->relation_ways = entries;
    context->relation_way_order = order;
    context->relation_way_capacity = capacity;
    return 0;
}

static int relation_way_insert(OsmRenderContext *context, long long id, unsigned int style_id) {
    unsigned int slot;

    if (context->relation_way_capacity != 0U) {
        slot = (unsigned int)(hash_id(id) & (unsigned long long)(context->relation_way_capacity - 1U));
        for (;;) {
            if (!context->relation_ways[slot].used) break;
            if (context->relation_ways[slot].id == id) {
                context->relation_ways[slot].style_id = style_id;
                return 0;
            }
            slot = (slot + 1U) & (context->relation_way_capacity - 1U);
        }
    }
    if (relation_way_grow(context, context->relation_way_count + 1U) != 0) return -1;
    if (relation_way_insert_raw(context->relation_ways, context->relation_way_capacity, id, style_id) != 0) return -1;
    context->relation_way_order[context->relation_way_count].id = id;
    context->relation_way_order[context->relation_way_count].style_id = style_id;
    context->relation_way_count += 1U;
    context->relation_members_collected += 1ULL;
    return 0;
}

static int relation_way_find(const OsmRenderContext *context, long long id, unsigned int *style_id_out) {
    unsigned int slot;

    if (context->relation_way_capacity == 0U) return 0;
    slot = (unsigned int)(hash_id(id) & (unsigned long long)(context->relation_way_capacity - 1U));
    for (;;) {
        if (!context->relation_ways[slot].used) return 0;
        if (context->relation_ways[slot].id == id) {
            *style_id_out = context->relation_ways[slot].style_id;
            return 1;
        }
        slot = (slot + 1U) & (context->relation_way_capacity - 1U);
    }
}

static int resolve_node(OsmRenderContext *context, long long id, OsmRenderNodeEntry *node_out) {
    if (context->node_index_records != 0) {
        unsigned long long left = 0ULL;
        unsigned long long right = context->node_index_record_count;

        while (left < right) {
            unsigned long long mid = left + (right - left) / 2ULL;
            const OsmNodeIndexRecord *record = &context->node_index_records[mid];
            if (record->id == id) {
                node_out->id = record->id;
                node_out->lat_nano = record->lat_nano;
                node_out->lon_nano = record->lon_nano;
                node_out->used = 1;
                return 1;
            }
            if (record->id < id) left = mid + 1ULL;
            else right = mid;
        }
        return 0;
    }
    if (context->node_index_open) {
        OsmNodeIndexRecord record;
        char error[OSM_INDEX_ERROR_CAPACITY];
        int found;

        error[0] = '\0';
        found = osm_node_index_find(&context->node_index, id, &record, error, sizeof(error));
        if (found < 0) {
            context->failed = 1;
            return 0;
        }
        if (found == 0) return 0;
        node_out->id = record.id;
        node_out->lat_nano = record.lat_nano;
        node_out->lon_nano = record.lon_nano;
        node_out->used = 1;
        return 1;
    }
    return node_map_find(context, id, node_out);
}

static int project_point_ll(const OsmRenderContext *context, const OsmRenderNodeEntry *node, long long *x_out, long long *y_out);
static int append_segment(OsmRenderContext *context, unsigned int style_id, int x0, int y0, int x1, int y1);

static int project_point(const OsmRenderContext *context, const OsmRenderNodeEntry *node, int *x_out, int *y_out) {
    long long x;
    long long y;

    if (project_point_ll(context, node, &x, &y) != 0) return -1;
    if (x < -2147483647LL || x > 2147483647LL || y < -2147483647LL || y > 2147483647LL) return -1;
    *x_out = (int)x;
    *y_out = (int)y;
    return 0;
}

static int project_point_ll(const OsmRenderContext *context, const OsmRenderNodeEntry *node, long long *x_out, long long *y_out) {
    long long lon_span = context->max_lon_nano - context->min_lon_nano;
    long long lat_span = context->max_lat_nano - context->min_lat_nano;
    long long x_num = node->lon_nano - context->min_lon_nano;
    long long y_num = context->max_lat_nano - node->lat_nano;

    if (lon_span <= 0 || lat_span <= 0) return -1;
    *x_out = (x_num * (long long)(context->width - 1U)) / lon_span;
    *y_out = (y_num * (long long)(context->height - 1U)) / lat_span;
    return 0;
}

#define OSM_RENDER_CLIP_LEFT   1U
#define OSM_RENDER_CLIP_RIGHT  2U
#define OSM_RENDER_CLIP_TOP    4U
#define OSM_RENDER_CLIP_BOTTOM 8U

static unsigned int clip_code(long long x, long long y, long long max_x, long long max_y) {
    unsigned int code = 0U;

    if (x < 0) code |= OSM_RENDER_CLIP_LEFT;
    else if (x > max_x) code |= OSM_RENDER_CLIP_RIGHT;
    if (y < 0) code |= OSM_RENDER_CLIP_TOP;
    else if (y > max_y) code |= OSM_RENDER_CLIP_BOTTOM;
    return code;
}

static int clip_segment_to_viewport(const OsmRenderContext *context, long long *x0, long long *y0, long long *x1, long long *y1) {
    long long max_x = (long long)context->width - 1LL;
    long long max_y = (long long)context->height - 1LL;
    unsigned int code0;
    unsigned int code1;
    unsigned int iterations = 0U;

    if (context->width == 0U || context->height == 0U) return 0;
    for (;;) {
        unsigned int outside;
        long long x;
        long long y;

        if (iterations++ > 16U) return 0;

        code0 = clip_code(*x0, *y0, max_x, max_y);
        code1 = clip_code(*x1, *y1, max_x, max_y);
        if ((code0 | code1) == 0U) return 1;
        if ((code0 & code1) != 0U) return 0;
        outside = code0 != 0U ? code0 : code1;
        if ((outside & OSM_RENDER_CLIP_TOP) != 0U) {
            if (*y1 == *y0) return 0;
            y = 0;
            x = *x0 + (*x1 - *x0) * (y - *y0) / (*y1 - *y0);
        } else if ((outside & OSM_RENDER_CLIP_BOTTOM) != 0U) {
            if (*y1 == *y0) return 0;
            y = max_y;
            x = *x0 + (*x1 - *x0) * (y - *y0) / (*y1 - *y0);
        } else if ((outside & OSM_RENDER_CLIP_RIGHT) != 0U) {
            if (*x1 == *x0) return 0;
            x = max_x;
            y = *y0 + (*y1 - *y0) * (x - *x0) / (*x1 - *x0);
        } else {
            if (*x1 == *x0) return 0;
            x = 0;
            y = *y0 + (*y1 - *y0) * (x - *x0) / (*x1 - *x0);
        }
        if (outside == code0) {
            if (x == *x0 && y == *y0) return 0;
            *x0 = x;
            *y0 = y;
        } else {
            if (x == *x1 && y == *y1) return 0;
            *x1 = x;
            *y1 = y;
        }
    }
}

static int append_clipped_segment(OsmRenderContext *context, unsigned int style_id, long long x0, long long y0, long long x1, long long y1) {
    if (!clip_segment_to_viewport(context, &x0, &y0, &x1, &y1)) return 0;
    return append_segment(context, style_id, (int)x0, (int)y0, (int)x1, (int)y1);
}

static int grow_segment_list(OsmRenderContext *context, unsigned int needed) {
    unsigned int capacity = context->segment_capacity == 0U ? 4096U : context->segment_capacity;
    OsmRenderSegment *segments;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->segment_capacity) return 0;
    segments = (OsmRenderSegment *)rt_realloc(context->segments, sizeof(OsmRenderSegment) * (size_t)capacity);
    if (segments == 0) return -1;
    context->segments = segments;
    context->segment_capacity = capacity;
    return 0;
}

static int append_segment(OsmRenderContext *context, unsigned int style_id, int x0, int y0, int x1, int y1) {
    OsmRenderSegment *segment;

    if (grow_segment_list(context, context->segment_count + 1U) != 0) return -1;
    segment = &context->segments[context->segment_count];
    segment->x0 = x0;
    segment->y0 = y0;
    segment->x1 = x1;
    segment->y1 = y1;
    segment->style_id = style_id;
    context->segment_count += 1U;
    context->segments_drawn += 1ULL;
    return 0;
}

static int grow_polygon_list(OsmRenderContext *context, unsigned int needed) {
    unsigned int capacity = context->polygon_capacity == 0U ? 1024U : context->polygon_capacity;
    OsmRenderPolygon *polygons;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->polygon_capacity) return 0;
    polygons = (OsmRenderPolygon *)rt_realloc(context->polygons, sizeof(OsmRenderPolygon) * (size_t)capacity);
    if (polygons == 0) return -1;
    context->polygons = polygons;
    context->polygon_capacity = capacity;
    return 0;
}

static int grow_polygon_points(OsmRenderContext *context, unsigned int needed) {
    unsigned int capacity = context->polygon_point_capacity == 0U ? 8192U : context->polygon_point_capacity;
    int *points;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->polygon_point_capacity) return 0;
    points = (int *)rt_realloc(context->polygon_points, sizeof(int) * (size_t)capacity);
    if (points == 0) return -1;
    context->polygon_points = points;
    context->polygon_point_capacity = capacity;
    return 0;
}

static int append_polygon(OsmRenderContext *context, unsigned int style_id, const int *points, unsigned int point_count) {
    OsmRenderPolygon *polygon;
    unsigned int value_count;

    if (point_count > 0x7fffffffU) return -1;
    value_count = point_count * 2U;
    if (context->polygon_point_count > 0xffffffffU - value_count) return -1;
    if (grow_polygon_list(context, context->polygon_count + 1U) != 0) return -1;
    if (grow_polygon_points(context, context->polygon_point_count + value_count) != 0) return -1;
    memcpy(context->polygon_points + context->polygon_point_count, points, sizeof(int) * (size_t)value_count);
    polygon = &context->polygons[context->polygon_count];
    polygon->point_offset = context->polygon_point_count;
    polygon->point_count = point_count;
    polygon->style_id = style_id;
    context->polygon_point_count += value_count;
    context->polygon_count += 1U;
    return 0;
}

static unsigned char blend_channel(unsigned char dst, unsigned char src, unsigned char alpha) {
    unsigned int inverse_alpha = 255U - (unsigned int)alpha;
    unsigned int value = (unsigned int)src * (unsigned int)alpha + (unsigned int)dst * inverse_alpha + 127U;

    return (unsigned char)(value / 255U);
}

static void put_pixel_rgb(OsmRenderContext *context, int x, int y, unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
    unsigned char *pixel;

    if (x < 0 || y < 0 || x >= (int)context->width || y >= (int)context->height) return;
    pixel = context->pixels + ((size_t)y * (size_t)context->width + (size_t)x) * 3U;
    if (alpha == 255U) {
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
    } else if (alpha != 0U) {
        pixel[0] = blend_channel(pixel[0], red, alpha);
        pixel[1] = blend_channel(pixel[1], green, alpha);
        pixel[2] = blend_channel(pixel[2], blue, alpha);
    }
}

static void put_pixel(OsmRenderContext *context, int x, int y, const OsmRenderStyle *style) {
    put_pixel_rgb(context, x, y, style->r, style->g, style->b, style->alpha);
}

static void put_brush(OsmRenderContext *context, int x, int y, const OsmRenderStyle *style) {
    int radius = style->width > 1U ? (int)((style->width + 1U) / 2U) : 0;
    int inner_radius = radius > 0 ? radius - 1 : 0;
    int inner_limit = inner_radius * inner_radius;
    int outer_limit = radius * radius;
    int dy;

    if (radius == 0) {
        put_pixel(context, x, y, style);
        return;
    }
    for (dy = -radius; dy <= radius; ++dy) {
        int dx;
        for (dx = -radius; dx <= radius; ++dx) {
            int distance = dx * dx + dy * dy;
            if (distance <= inner_limit) {
                put_pixel(context, x + dx, y + dy, style);
            } else if (distance <= outer_limit) {
                put_pixel_rgb(context, x + dx, y + dy, style->r, style->g, style->b, (unsigned char)((unsigned int)style->alpha / 2U));
            }
        }
    }
}

static int abs_int(int value) {
    return value < 0 ? -value : value;
}

static void draw_line(OsmRenderContext *context, int x0, int y0, int x1, int y1, const OsmRenderStyle *style) {
    int dx = abs_int(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs_int(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        int twice_err;

        put_brush(context, x0, y0, style);
        if (x0 == x1 && y0 == y1) break;
        twice_err = 2 * err;
        if (twice_err >= dy) {
            err += dy;
            x0 += sx;
        }
        if (twice_err <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_styled_line_step(OsmRenderContext *context, int x0, int y0, int x1, int y1, const OsmRenderStyle *style) {
    OsmRenderStyle stroke;

    if (context->render_step == OSM_RENDER_STEP_CASING && (style->flags & OSM_RENDER_STYLE_CASING) != 0U && style->casing_width > style->width) {
        rt_memset(&stroke, 0, sizeof(stroke));
        stroke.r = style->casing_r;
        stroke.g = style->casing_g;
        stroke.b = style->casing_b;
        stroke.alpha = style->casing_alpha;
        stroke.width = style->casing_width;
        draw_line(context, x0, y0, x1, y1, &stroke);
    }
    if (context->render_step == OSM_RENDER_STEP_LINE && (style->flags & OSM_RENDER_STYLE_LINE) != 0U) {
        draw_line(context, x0, y0, x1, y1, style);
    }
}

static void sort_intersections(int *values, unsigned int count) {
    unsigned int index;

    for (index = 1U; index < count; ++index) {
        int value = values[index];
        unsigned int scan = index;
        while (scan > 0U && values[scan - 1U] > value) {
            values[scan] = values[scan - 1U];
            scan -= 1U;
        }
        values[scan] = value;
    }
}

static void draw_filled_polygon(OsmRenderContext *context, const int *points, unsigned int point_count, const OsmRenderStyle *style) {
    int min_y = (int)context->height;
    int max_y = -1;
    int *intersections;
    unsigned int point_index;
    int scan_y;

    if ((style->flags & OSM_RENDER_STYLE_FILL) == 0U || point_count < 4U) return;
    for (point_index = 0U; point_index < point_count; ++point_index) {
        int point_y = points[point_index * 2U + 1U];
        if (point_y < min_y) min_y = point_y;
        if (point_y > max_y) max_y = point_y;
    }
    if (max_y < 0 || min_y >= (int)context->height) return;
    if (min_y < 0) min_y = 0;
    if (max_y >= (int)context->height) max_y = (int)context->height - 1;
    intersections = (int *)rt_malloc(sizeof(int) * (size_t)point_count);
    if (intersections == 0) return;
    for (scan_y = min_y; scan_y <= max_y; ++scan_y) {
        unsigned int intersection_count = 0U;
        for (point_index = 0U; point_index + 1U < point_count; ++point_index) {
            int x0 = points[point_index * 2U + 0U];
            int y0 = points[point_index * 2U + 1U];
            int x1 = points[(point_index + 1U) * 2U + 0U];
            int y1 = points[(point_index + 1U) * 2U + 1U];
            if ((y0 <= scan_y && y1 > scan_y) || (y1 <= scan_y && y0 > scan_y)) {
                long long numerator = (long long)(scan_y - y0) * (long long)(x1 - x0);
                int crossing = x0 + (int)(numerator / (long long)(y1 - y0));
                intersections[intersection_count] = crossing;
                intersection_count += 1U;
            }
        }
        sort_intersections(intersections, intersection_count);
        for (point_index = 0U; point_index + 1U < intersection_count; point_index += 2U) {
            int start_x = intersections[point_index];
            int end_x = intersections[point_index + 1U];
            int fill_x;
            if (start_x < 0) start_x = 0;
            if (end_x >= (int)context->width) end_x = (int)context->width - 1;
            for (fill_x = start_x; fill_x <= end_x; ++fill_x) {
                put_pixel_rgb(context, fill_x, scan_y, style->fill_r, style->fill_g, style->fill_b, style->fill_alpha);
            }
        }
    }
    rt_free(intersections);
}

static int parse_style_uint_value(const char *text, unsigned int max_value, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > (unsigned long long)max_value) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static void skip_style_spaces(const char **text_io) {
    while (rt_is_space(**text_io)) *text_io += 1;
}

static int parse_style_color_component(const char **text_io, unsigned char *component_out) {
    unsigned int value = 0U;
    unsigned int digits = 0U;

    skip_style_spaces(text_io);
    while (**text_io >= '0' && **text_io <= '9') {
        value = value * 10U + (unsigned int)(**text_io - '0');
        if (value > 255U) return -1;
        digits += 1U;
        *text_io += 1;
    }
    if (digits == 0U) return -1;
    skip_style_spaces(text_io);
    *component_out = (unsigned char)value;
    return 0;
}

static int parse_style_color(const char *text, unsigned char default_alpha, unsigned char *red, unsigned char *green, unsigned char *blue, unsigned char *alpha) {
    const char *cursor = text;

    if (parse_style_color_component(&cursor, red) != 0 || *cursor != ',') return -1;
    cursor += 1;
    if (parse_style_color_component(&cursor, green) != 0 || *cursor != ',') return -1;
    cursor += 1;
    if (parse_style_color_component(&cursor, blue) != 0) return -1;
    *alpha = default_alpha;
    if (*cursor == ',') {
        cursor += 1;
        if (parse_style_color_component(&cursor, alpha) != 0) return -1;
    }
    skip_style_spaces(&cursor);
    return *cursor == '\0' ? 0 : -1;
}

static int style_name_equals(const char *key, size_t key_size, const char *name) {
    size_t name_size = rt_strlen(name);

    return key_size == name_size && memcmp(key, name, name_size) == 0;
}

static int style_id_from_name(const char *name, size_t name_size, OsmRenderStyleId *style_id_out) {
    if (style_name_equals(name, name_size, "water")) *style_id_out = OSM_RENDER_STYLE_WATER;
    else if (style_name_equals(name, name_size, "waterway")) *style_id_out = OSM_RENDER_STYLE_WATERWAY;
    else if (style_name_equals(name, name_size, "forest")) *style_id_out = OSM_RENDER_STYLE_FOREST;
    else if (style_name_equals(name, name_size, "park")) *style_id_out = OSM_RENDER_STYLE_PARK;
    else if (style_name_equals(name, name_size, "building")) *style_id_out = OSM_RENDER_STYLE_BUILDING;
    else if (style_name_equals(name, name_size, "motorway")) *style_id_out = OSM_RENDER_STYLE_MOTORWAY;
    else if (style_name_equals(name, name_size, "primary")) *style_id_out = OSM_RENDER_STYLE_PRIMARY;
    else if (style_name_equals(name, name_size, "secondary")) *style_id_out = OSM_RENDER_STYLE_SECONDARY;
    else if (style_name_equals(name, name_size, "minor_road")) *style_id_out = OSM_RENDER_STYLE_MINOR_ROAD;
    else if (style_name_equals(name, name_size, "path")) *style_id_out = OSM_RENDER_STYLE_PATH;
    else if (style_name_equals(name, name_size, "rail")) *style_id_out = OSM_RENDER_STYLE_RAIL;
    else return -1;
    return 0;
}

static int style_config_visit(const char *key, const char *value, void *user) {
    OsmRenderStyleSheet *style_sheet = (OsmRenderStyleSheet *)user;
    const char *dot = key;
    OsmRenderStyleId style_id;
    OsmRenderStyle *style;

    if (rt_strcmp(key, "background") == 0) {
        unsigned char alpha;
        return parse_style_color(value, 255U, &style_sheet->background_r, &style_sheet->background_g, &style_sheet->background_b, &alpha);
    }
    while (*dot != '\0' && *dot != '.') dot += 1;
    if (*dot != '.' || dot == key || dot[1] == '\0') return -1;
    if (style_id_from_name(key, (size_t)(dot - key), &style_id) != 0) return -1;
    style = &style_sheet->styles[style_id];
    dot += 1;
    if (rt_strcmp(dot, "line") == 0) {
        if (parse_style_color(value, 255U, &style->r, &style->g, &style->b, &style->alpha) != 0) return -1;
        if (style->width == 0U) style->width = 1U;
        style->flags |= OSM_RENDER_STYLE_LINE;
        return 0;
    }
    if (rt_strcmp(dot, "fill") == 0) {
        if (parse_style_color(value, 200U, &style->fill_r, &style->fill_g, &style->fill_b, &style->fill_alpha) != 0) return -1;
        style->flags |= OSM_RENDER_STYLE_FILL;
        return 0;
    }
    if (rt_strcmp(dot, "casing") == 0) {
        if (parse_style_color(value, 220U, &style->casing_r, &style->casing_g, &style->casing_b, &style->casing_alpha) != 0) return -1;
        if (style->casing_width == 0U) style->casing_width = style->width + 2U;
        style->flags |= OSM_RENDER_STYLE_CASING;
        return 0;
    }
    if (rt_strcmp(dot, "width") == 0) return parse_style_uint_value(value, 64U, &style->width);
    if (rt_strcmp(dot, "casing_width") == 0) return parse_style_uint_value(value, 64U, &style->casing_width);
    return -1;
}

static int load_style_sheet(const char *path, OsmRenderStyleSheet *style_sheet) {
    return simple_config_parse_file(path, style_config_visit, style_sheet);
}

static int on_node(void *user, const PbfNode *node) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    OsmRenderNodeEntry point;
    OsmRenderStyle point_style;
    int draw_point = context->node_points;
    int x;
    int y;

    if (!node_in_bbox(context, node->lat_nano, node->lon_nano)) return 0;
    if (node_map_insert(context, node->id, node->lat_nano, node->lon_nano) != 0) {
        context->failed = 1;
        return 1;
    }
    if (context->tree_points) {
        draw_point = tag_value_equals(find_tag_value_in_tags(node->tags, node->tag_count, "natural"), "tree");
    }
    if (draw_point) {
        point.id = node->id;
        point.lat_nano = node->lat_nano;
        point.lon_nano = node->lon_nano;
        point.used = 1;
        point_style.r = context->tree_points ? 63U : 82U;
        point_style.g = context->tree_points ? 128U : 116U;
        point_style.b = context->tree_points ? 70U : 92U;
        point_style.alpha = context->tree_points ? 170U : 150U;
        point_style.width = context->tree_points ? 2U : 1U;
        point_style.casing_width = 0U;
        point_style.flags = OSM_RENDER_STYLE_LINE;
        if (project_point(context, &point, &x, &y) == 0) {
            put_brush(context, x, y, &point_style);
            if (context->tree_points) {
                context->tree_nodes_drawn += 1ULL;
                if (context->stop_after_trees != 0U && context->tree_nodes_drawn >= (unsigned long long)context->stop_after_trees) {
                    context->stopped_after_trees = 1;
                    return 1;
                }
            }
        }
    }
    if (context->stop_after_nodes != 0U && context->node_count >= context->stop_after_nodes) {
        context->stopped_after_nodes = 1;
        return 1;
    }
    return 0;
}

static int on_way(void *user, const PbfWay *way) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    OsmRenderStyle style;
    OsmRenderStyleId style_id;
    long long previous_x = 0;
    long long previous_y = 0;
    int has_previous = 0;
    unsigned int index;
    unsigned int segment_start;
    int is_closed = way->ref_count > 3U && way->refs[0] == way->refs[way->ref_count - 1U];
    int is_relation_member = 0;

    if (context->relation_filter_enabled) {
        unsigned int relation_style_id;
        if (!relation_way_find(context, way->id, &relation_style_id)) return 0;
        style_id = (OsmRenderStyleId)relation_style_id;
        copy_style(context->style_sheet, style_id, &style);
        is_relation_member = 1;
    } else if (!classify_way(context->style_sheet, way, &style, &style_id)) {
        unsigned int relation_style_id;
        if (!relation_way_find(context, way->id, &relation_style_id)) return 0;
        style_id = (OsmRenderStyleId)relation_style_id;
        copy_style(context->style_sheet, style_id, &style);
        is_relation_member = 1;
    } else {
        unsigned int relation_style_id;
        if (relation_way_find(context, way->id, &relation_style_id) && style_id_is_green_context((OsmRenderStyleId)relation_style_id)) {
            style_id = (OsmRenderStyleId)relation_style_id;
            copy_style(context->style_sheet, style_id, &style);
            is_relation_member = 1;
        }
    }
    if (context->green_only && !style_id_is_green_context(style_id)) return 0;
    if (context->max_way_refs != 0U && way->ref_count > context->max_way_refs) {
        context->way_refs_skipped += 1ULL;
        return 0;
    }
    context->ways_decoded += 1ULL;
    if (is_relation_member) context->relation_ways_matched += 1ULL;
    if ((style.flags & (OSM_RENDER_STYLE_LINE | OSM_RENDER_STYLE_CASING)) != 0U) context->ways_seen += 1ULL;
    if (!context->no_fills && is_closed && (style.flags & OSM_RENDER_STYLE_FILL) != 0U) {
        int *points = (int *)rt_malloc(sizeof(int) * (size_t)way->ref_count * 2U);
        unsigned int point_count = 0U;

        if (points == 0) {
            context->failed = 1;
            return 1;
        }
        for (index = 0U; index < way->ref_count; ++index) {
            OsmRenderNodeEntry node;
            long long x;
            long long y;
            if (!resolve_node(context, way->refs[index], &node)) break;
            if (project_point_ll(context, &node, &x, &y) != 0) break;
            if (x < -1000000LL || x > (long long)context->width + 1000000LL || y < -1000000LL || y > (long long)context->height + 1000000LL) break;
            points[point_count * 2U] = (int)x;
            points[point_count * 2U + 1U] = (int)y;
            point_count += 1U;
        }
        if (point_count == way->ref_count && append_polygon(context, (unsigned int)style_id, points, point_count) != 0) {
            rt_free(points);
            context->failed = 1;
            return 1;
        }
        rt_free(points);
    }
    if ((style.flags & (OSM_RENDER_STYLE_LINE | OSM_RENDER_STYLE_CASING)) == 0U && !is_relation_member) return 0;
    segment_start = context->segment_count;
    for (index = 0U; index < way->ref_count; ++index) {
        OsmRenderNodeEntry node;
        long long x;
        long long y;

        if (!resolve_node(context, way->refs[index], &node)) {
            has_previous = 0;
            continue;
        }
        if (project_point_ll(context, &node, &x, &y) != 0) {
            has_previous = 0;
            continue;
        }
        if (has_previous) {
            if (append_clipped_segment(context, (unsigned int)style_id, previous_x, previous_y, x, y) != 0) {
                context->failed = 1;
                return 1;
            }
        }
        previous_x = x;
        previous_y = y;
        has_previous = 1;
    }
    if (context->segment_count != segment_start) {
        context->ways_drawn += 1ULL;
        if (context->stop_after_drawn != 0U && context->ways_drawn >= (unsigned long long)context->stop_after_drawn) {
            context->stopped_after_drawn = 1;
            return 1;
        }
    }
    if (context->relation_filter_enabled && context->relation_way_count != 0U && context->relation_ways_matched >= (unsigned long long)context->relation_way_count) {
        context->stopped_after_ways = 1;
        return 1;
    }
    if (context->stop_after_ways != 0U && context->ways_decoded >= (unsigned long long)context->stop_after_ways) {
        context->stopped_after_ways = 1;
        return 1;
    }
    return 0;
}

static int on_way_tags(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    PbfWay way;
    OsmRenderStyle style;
    OsmRenderStyleId style_id;

    rt_memset(&way, 0, sizeof(way));
    way.id = id;
    way.tags = tags;
    way.tag_count = tag_count;
    if (context->relation_filter_enabled) return relation_way_find(context, id, &style_id);
    if (relation_way_find(context, id, &style_id)) return 1;
    if (!classify_way(context->style_sheet, &way, &style, &style_id)) return 0;
    if (context->green_only && !style_id_is_green_context(style_id)) return 0;
    return (style.flags & (OSM_RENDER_STYLE_FILL | OSM_RENDER_STYLE_LINE | OSM_RENDER_STYLE_CASING)) != 0U;
}

static int on_relation_tags(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    OsmRenderStyleId style_id;

    if (context->relation_filter_enabled && id != context->relation_filter_id) return 0;
    if (!classify_tags_id(tags, tag_count, &style_id)) return 0;
    if (context->green_only && !style_id_is_green_context(style_id)) return 0;
    return style_id_is_green_context(style_id);
}

static int on_relation(void *user, const PbfRelation *relation) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    OsmRenderStyleId style_id;
    unsigned int index;

    if (context->relation_filter_enabled && relation->id != context->relation_filter_id) return 0;
    if (!classify_tags_id(relation->tags, relation->tag_count, &style_id)) return 0;
    if (!style_id_is_green_context(style_id)) return 0;
    context->relations_seen += 1ULL;
    for (index = 0U; index < relation->member_count; ++index) {
        const PbfRelationMember *member = &relation->members[index];
        if (member->type == PBF_RELATION_MEMBER_WAY) {
            if (relation_way_insert(context, member->id, (unsigned int)style_id) != 0) {
                context->failed = 1;
                return 1;
            }
        }
    }
    return 0;
}

static int compare_indexed_way_candidates(const void *left, const void *right) {
    const OsmRenderIndexedWayCandidate *left_candidate = (const OsmRenderIndexedWayCandidate *)left;
    const OsmRenderIndexedWayCandidate *right_candidate = (const OsmRenderIndexedWayCandidate *)right;

    if (left_candidate->record.ref_count < right_candidate->record.ref_count) return -1;
    if (left_candidate->record.ref_count > right_candidate->record.ref_count) return 1;
    if (left_candidate->record.id < right_candidate->record.id) return -1;
    if (left_candidate->record.id > right_candidate->record.id) return 1;
    return 0;
}

static int render_indexed_relation_ways(OsmRenderContext *context) {
    OsmRenderIndexedWayCandidate *candidates;
    unsigned int candidate_count = 0U;
    unsigned int index;

    if (context->relation_way_count == 0U) return 0;
    candidates = (OsmRenderIndexedWayCandidate *)rt_malloc(sizeof(OsmRenderIndexedWayCandidate) * (size_t)context->relation_way_count);
    if (candidates == 0) {
        context->failed = 1;
        return 1;
    }

    for (index = 0U; index < context->relation_way_count; ++index) {
        OsmWayIndexRecord record;
        char error[OSM_INDEX_ERROR_CAPACITY];
        int found;

        error[0] = '\0';
        found = osm_way_index_find(&context->way_index, context->relation_way_order[index].id, &record, error, sizeof(error));
        if (found < 0) {
            rt_free(candidates);
            context->failed = 1;
            return 1;
        }
        if (found == 0) continue;
        candidates[candidate_count].record = record;
        candidates[candidate_count].style_id = context->relation_way_order[index].style_id;
        candidate_count += 1U;
    }
    if (context->stop_after_ways != 0U || context->stop_after_drawn != 0U) {
        rt_sort(candidates, candidate_count, sizeof(*candidates), compare_indexed_way_candidates);
    }
    for (index = 0U; index < candidate_count; ++index) {
        long long *refs = 0;
        PbfWay way;
        char error[OSM_INDEX_ERROR_CAPACITY];

        error[0] = '\0';
        if (context->max_way_refs != 0U && candidates[index].record.ref_count > context->max_way_refs) {
            context->way_refs_skipped += 1ULL;
            continue;
        }
        refs = 0;
        if (osm_way_index_read_refs(&context->way_index, &candidates[index].record, &refs, error, sizeof(error)) != 0) {
            rt_free(candidates);
            context->failed = 1;
            return 1;
        }
        rt_memset(&way, 0, sizeof(way));
        way.id = candidates[index].record.id;
        way.refs = refs;
        way.ref_count = candidates[index].record.ref_count;
        if (on_way(context, &way) != 0) {
            rt_free(refs);
            rt_free(candidates);
            return 1;
        }
        rt_free(refs);
        if (context->stopped_after_ways || context->stopped_after_drawn || context->failed) {
            rt_free(candidates);
            return 1;
        }
    }
    rt_free(candidates);
    return 0;
}

static void write_u16_le(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
}

static void write_u32_le(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
    out[2] = (unsigned char)((value >> 16U) & 0xffU);
    out[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static int write_bmp(const char *path, const OsmRenderContext *context) {
    int fd;
    unsigned int row_stride = ((context->width * 3U + 3U) / 4U) * 4U;
    unsigned int pixel_bytes = row_stride * context->height;
    unsigned int file_size = 54U + pixel_bytes;
    unsigned char header[54];
    unsigned char *row;
    unsigned int y;

    row = (unsigned char *)rt_malloc(row_stride);
    if (row == 0) return -1;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        rt_free(row);
        return -1;
    }
    rt_memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    write_u32_le(header + 2U, file_size);
    write_u32_le(header + 10U, 54U);
    write_u32_le(header + 14U, 40U);
    write_u32_le(header + 18U, context->width);
    write_u32_le(header + 22U, context->height);
    write_u16_le(header + 26U, 1U);
    write_u16_le(header + 28U, 24U);
    write_u32_le(header + 34U, pixel_bytes);
    if (rt_write_all(fd, header, sizeof(header)) != 0) {
        (void)platform_close(fd);
        rt_free(row);
        return -1;
    }
    for (y = 0U; y < context->height; ++y) {
        unsigned int src_y = context->height - 1U - y;
        unsigned int x;
        rt_memset(row, 0, row_stride);
        for (x = 0U; x < context->width; ++x) {
            const unsigned char *pixel = context->pixels + ((size_t)src_y * (size_t)context->width + (size_t)x) * 3U;
            row[x * 3U + 0U] = pixel[2];
            row[x * 3U + 1U] = pixel[1];
            row[x * 3U + 2U] = pixel[0];
        }
        if (rt_write_all(fd, row, row_stride) != 0) {
            (void)platform_close(fd);
            rt_free(row);
            return -1;
        }
    }
    rt_free(row);
    return platform_close(fd);
}

static void fill_background(OsmRenderContext *context) {
    size_t pixel_count = (size_t)context->width * (size_t)context->height;
    size_t index;

    for (index = 0U; index < pixel_count; ++index) {
        context->pixels[index * 3U + 0U] = context->style_sheet->background_r;
        context->pixels[index * 3U + 1U] = context->style_sheet->background_g;
        context->pixels[index * 3U + 2U] = context->style_sheet->background_b;
    }
}

static unsigned long long count_visible_pixels(const OsmRenderContext *context) {
    size_t pixel_count = (size_t)context->width * (size_t)context->height;
    size_t index;
    unsigned long long visible = 0ULL;

    for (index = 0U; index < pixel_count; ++index) {
        const unsigned char *pixel = context->pixels + index * 3U;
        if (pixel[0] != context->style_sheet->background_r || pixel[1] != context->style_sheet->background_g || pixel[2] != context->style_sheet->background_b) {
            visible += 1ULL;
        }
    }
    return visible;
}

static void collect_loaded_index_nodes(OsmRenderContext *context) {
    unsigned long long index;

    for (index = 0ULL; index < context->node_index_record_count; ++index) {
        const OsmNodeIndexRecord *record = &context->node_index_records[index];
        OsmRenderNodeEntry node;

        if (!node_in_bbox(context, record->lat_nano, record->lon_nano)) continue;
        if (context->node_count < 0xffffffffU) context->node_count += 1U;
        if (context->node_points) {
            OsmRenderStyle point_style;
            int x;
            int y;

            node.id = record->id;
            node.lat_nano = record->lat_nano;
            node.lon_nano = record->lon_nano;
            node.used = 1;
            point_style.r = 82U;
            point_style.g = 116U;
            point_style.b = 92U;
            point_style.alpha = 150U;
            point_style.width = 1U;
            point_style.casing_width = 0U;
            point_style.flags = OSM_RENDER_STYLE_LINE;
            if (project_point(context, &node, &x, &y) == 0) put_pixel(context, x, y, &point_style);
        }
        if (context->stop_after_nodes != 0U && context->node_count >= context->stop_after_nodes) {
            context->stopped_after_nodes = 1;
            return;
        }
    }
}

typedef struct {
    unsigned int step;
    OsmRenderStyleId style_id;
} OsmRenderLayer;

static const OsmRenderLayer render_layers[] = {
    { OSM_RENDER_STEP_AREA, OSM_RENDER_STYLE_WATER },
    { OSM_RENDER_STEP_AREA, OSM_RENDER_STYLE_FOREST },
    { OSM_RENDER_STEP_AREA, OSM_RENDER_STYLE_PARK },
    { OSM_RENDER_STEP_AREA, OSM_RENDER_STYLE_BUILDING },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_WATER },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_FOREST },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_PARK },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_BUILDING },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_WATERWAY },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_PATH },
    { OSM_RENDER_STEP_CASING, OSM_RENDER_STYLE_MINOR_ROAD },
    { OSM_RENDER_STEP_CASING, OSM_RENDER_STYLE_SECONDARY },
    { OSM_RENDER_STEP_CASING, OSM_RENDER_STYLE_PRIMARY },
    { OSM_RENDER_STEP_CASING, OSM_RENDER_STYLE_MOTORWAY },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_MINOR_ROAD },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_SECONDARY },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_PRIMARY },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_MOTORWAY },
    { OSM_RENDER_STEP_CASING, OSM_RENDER_STYLE_RAIL },
    { OSM_RENDER_STEP_LINE, OSM_RENDER_STYLE_RAIL }
};

static void draw_render_layers(OsmRenderContext *context) {
    unsigned int layer_index;

    for (layer_index = 0U; layer_index < (unsigned int)(sizeof(render_layers) / sizeof(render_layers[0])); ++layer_index) {
        OsmRenderStyle style;
        unsigned int style_id = (unsigned int)render_layers[layer_index].style_id;
        unsigned int index;

        context->render_step = render_layers[layer_index].step;
        copy_style(context->style_sheet, render_layers[layer_index].style_id, &style);
        if (context->render_step == OSM_RENDER_STEP_AREA) {
            for (index = 0U; index < context->polygon_count; ++index) {
                const OsmRenderPolygon *polygon = &context->polygons[index];
                if (polygon->style_id == style_id) {
                    draw_filled_polygon(context, context->polygon_points + polygon->point_offset, polygon->point_count, &style);
                }
            }
        } else {
            for (index = 0U; index < context->segment_count; ++index) {
                const OsmRenderSegment *segment = &context->segments[index];
                if (segment->style_id == style_id) {
                    draw_styled_line_step(context, segment->x0, segment->y0, segment->x1, segment->y1, &style);
                }
            }
        }
    }
}

static void cleanup_context(OsmRenderContext *context) {
    if (context->node_index_open) {
        osm_node_index_close(&context->node_index);
        context->node_index_open = 0;
    }
    if (context->way_index_open) {
        osm_way_index_close(&context->way_index);
        context->way_index_open = 0;
    }
    rt_free(context->nodes);
    rt_free(context->node_index_records);
    rt_free(context->segments);
    rt_free(context->polygons);
    rt_free(context->relation_ways);
    rt_free(context->relation_way_order);
    rt_free(context->polygon_points);
    rt_free(context->pixels);
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmrender";
    const char *pbf_path;
    const char *out_path;
    const char *style_path = 0;
    OsmRenderContext context;
    OsmRenderStyleSheet style_sheet;
    PbfStreamCallbacks callbacks;
    char error[PBF_ERROR_CAPACITY];
    int argi;

    if (argc < 4) {
        write_usage(program);
        return 1;
    }
    pbf_path = argv[1];
    if (argc >= 5 && argv[3][0] != '-' && argv[4][0] == '-') {
        out_path = argv[3];
        argi = 4;
    } else {
        out_path = argv[2];
        argi = 3;
    }
    rt_memset(&context, 0, sizeof(context));
    style_sheet_init(&style_sheet);
    context.style_sheet = &style_sheet;
    context.width = 1024U;
    context.height = 1024U;
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--bbox") == 0) {
            argi += 1;
            if (argi >= argc || parse_bbox_arg(argv[argi], &context) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--width") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.width) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--height") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.height) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--style") == 0) {
            argi += 1;
            if (argi >= argc) {
                write_usage(program);
                return 1;
            }
            style_path = argv[argi];
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--node-index") == 0) {
            argi += 1;
            if (argi >= argc) {
                write_usage(program);
                return 1;
            }
            context.node_index_path = argv[argi];
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--way-index") == 0) {
            argi += 1;
            if (argi >= argc) {
                write_usage(program);
                return 1;
            }
            context.way_index_path = argv[argi];
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--node-points") == 0) {
            context.node_points = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--tree-points") == 0) {
            context.tree_points = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--green-only") == 0) {
            context.green_only = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-fills") == 0) {
            context.no_fills = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--max-way-refs") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.max_way_refs) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--relation-id") == 0) {
            argi += 1;
            if (argi >= argc || parse_id_arg(argv[argi], &context.relation_filter_id) != 0) {
                write_usage(program);
                return 1;
            }
            context.relation_filter_enabled = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stop-after-nodes") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.stop_after_nodes) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stop-after-trees") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.stop_after_trees) != 0) {
                write_usage(program);
                return 1;
            }
            context.tree_points = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stop-after-ways") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.stop_after_ways) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stop-after-drawn") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.stop_after_drawn) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            write_usage(program);
            return 0;
        } else {
            write_usage(program);
            return 1;
        }
    }
    if (context.min_lon_nano == 0 && context.max_lon_nano == 0 && context.min_lat_nano == 0 && context.max_lat_nano == 0) {
        write_usage(program);
        return 1;
    }
    if (style_path != 0 && load_style_sheet(style_path, &style_sheet) != 0) {
        rt_write_cstr(2, "osmrender: could not parse style file: ");
        rt_write_cstr(2, style_path);
        rt_write_char(2, '\n');
        return 1;
    }
    if (context.node_index_path != 0) {
        char index_error[OSM_INDEX_ERROR_CAPACITY];

        index_error[0] = '\0';
        if (osm_node_index_open(&context.node_index, context.node_index_path, index_error, sizeof(index_error)) != 0) {
            rt_write_cstr(2, "osmrender: ");
            rt_write_cstr(2, index_error[0] == '\0' ? "could not open node index" : index_error);
            rt_write_char(2, '\n');
            return 1;
        }
        context.node_index_open = 1;
        if (osm_node_index_load_records(&context.node_index, &context.node_index_records, &context.node_index_record_count, index_error, sizeof(index_error)) != 0) {
            rt_write_cstr(2, "osmrender: ");
            rt_write_cstr(2, index_error[0] == '\0' ? "could not load node index" : index_error);
            rt_write_char(2, '\n');
            cleanup_context(&context);
            return 1;
        }
    }
    if (context.way_index_path != 0) {
        char index_error[OSM_INDEX_ERROR_CAPACITY];

        index_error[0] = '\0';
        if (osm_way_index_open(&context.way_index, context.way_index_path, index_error, sizeof(index_error)) != 0) {
            rt_write_cstr(2, "osmrender: ");
            rt_write_cstr(2, index_error[0] == '\0' ? "could not open way index" : index_error);
            rt_write_char(2, '\n');
            cleanup_context(&context);
            return 1;
        }
        context.way_index_open = 1;
    }
    context.pixels = (unsigned char *)rt_malloc((size_t)context.width * (size_t)context.height * 3U);
    if (context.pixels == 0) {
        rt_write_cstr(2, "osmrender: out of memory\n");
        cleanup_context(&context);
        return 1;
    }
    fill_background(&context);

    if (!context.node_index_open || context.tree_points) {
        rt_memset(&callbacks, 0, sizeof(callbacks));
        callbacks.flags = context.tree_points ? 0U : PBF_STREAM_SKIP_NODE_TAGS;
        callbacks.node = on_node;
        error[0] = '\0';
        if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
            rt_write_cstr(2, "osmrender: ");
            rt_write_cstr(2, context.failed ? "out of memory while collecting bbox nodes" : (error[0] == '\0' ? "failed to collect bbox nodes" : error));
            rt_write_char(2, '\n');
            cleanup_context(&context);
            return 1;
        }
    } else {
        collect_loaded_index_nodes(&context);
    }
    if (!context.stopped_after_nodes && !context.stopped_after_trees && !context.stopped_after_ways && !context.stopped_after_drawn) {
        rt_memset(&callbacks, 0, sizeof(callbacks));
        callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS;
        callbacks.relation_tags = on_relation_tags;
        callbacks.relation = on_relation;
        error[0] = '\0';
        if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
            rt_write_cstr(2, "osmrender: ");
            rt_write_cstr(2, context.failed ? "out of memory while collecting relations" : (error[0] == '\0' ? "failed to collect relations" : error));
            rt_write_char(2, '\n');
            cleanup_context(&context);
            return 1;
        }
        if (context.relation_filter_enabled && context.way_index_open) {
            if (render_indexed_relation_ways(&context) != 0 && context.failed) {
                rt_write_cstr(2, "osmrender: failed to collect indexed relation ways\n");
                cleanup_context(&context);
                return 1;
            }
        } else {
            rt_memset(&callbacks, 0, sizeof(callbacks));
            callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS;
            if (context.relation_filter_enabled) callbacks.flags |= PBF_STREAM_SKIP_WAY_TAGS;
            callbacks.way_tags = on_way_tags;
            callbacks.way = on_way;
            error[0] = '\0';
            if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
                rt_write_cstr(2, "osmrender: ");
                rt_write_cstr(2, context.failed ? "out of memory while collecting ways" : (error[0] == '\0' ? "failed to collect ways" : error));
                rt_write_char(2, '\n');
                cleanup_context(&context);
                return 1;
            }
        }
        draw_render_layers(&context);
    }
    context.visible_pixels = count_visible_pixels(&context);
    if (write_bmp(out_path, &context) != 0) {
        rt_write_cstr(2, "osmrender: could not write BMP\n");
        cleanup_context(&context);
        return 1;
    }
    rt_write_cstr(1, "nodes_in_bbox: ");
    rt_write_uint(1, context.node_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_seen: ");
    rt_write_uint(1, context.ways_seen);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_decoded: ");
    rt_write_uint(1, context.ways_decoded);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_drawn: ");
    rt_write_uint(1, context.ways_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "segments_drawn: ");
    rt_write_uint(1, context.segments_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tree_nodes_drawn: ");
    rt_write_uint(1, context.tree_nodes_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "relations_seen: ");
    rt_write_uint(1, context.relations_seen);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "relation_members_collected: ");
    rt_write_uint(1, context.relation_members_collected);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "relation_ways_matched: ");
    rt_write_uint(1, context.relation_ways_matched);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "way_refs_skipped: ");
    rt_write_uint(1, context.way_refs_skipped);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "polygons_collected: ");
    rt_write_uint(1, context.polygon_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "segments_collected: ");
    rt_write_uint(1, context.segment_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "visible_pixels: ");
    rt_write_uint(1, context.visible_pixels);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "node_index: ");
    rt_write_cstr(1, context.node_index_open ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "way_index: ");
    rt_write_cstr(1, context.way_index_open ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "bounded: ");
    rt_write_cstr(1, (context.stopped_after_nodes || context.stopped_after_trees || context.stopped_after_ways || context.stopped_after_drawn) ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "node_points: ");
    rt_write_cstr(1, context.node_points ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tree_points: ");
    rt_write_cstr(1, context.tree_points ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "green_only: ");
    rt_write_cstr(1, context.green_only ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "no_fills: ");
    rt_write_cstr(1, context.no_fills ? "yes" : "no");
    rt_write_char(1, '\n');
    cleanup_context(&context);
    return 0;
}
