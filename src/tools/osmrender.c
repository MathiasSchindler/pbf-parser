#include "pbf.h"
#include "platform.h"
#include "runtime.h"

typedef struct {
    long long id;
    long long lat_nano;
    long long lon_nano;
    int used;
} OsmRenderNodeEntry;

typedef struct {
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    unsigned int width;
    unsigned int height;
    unsigned char *pixels;
    OsmRenderNodeEntry *nodes;
    unsigned int node_capacity;
    unsigned int node_count;
    unsigned long long ways_seen;
    unsigned long long ways_drawn;
    unsigned long long segments_drawn;
    unsigned int stop_after_nodes;
    unsigned int stop_after_drawn;
    int node_points;
    int stopped_after_nodes;
    int stopped_after_drawn;
    int failed;
} OsmRenderContext;

typedef struct {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned int width;
} OsmRenderStyle;

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf OUT.bmp --bbox MINLON,MINLAT,MAXLON,MAXLAT [--width N] [--height N] [--node-points] [--stop-after-nodes N] [--stop-after-drawn N]\n");
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

static int classify_way(const PbfWay *way, OsmRenderStyle *style_out) {
    unsigned int index;
    int has_building = 0;

    for (index = 0U; index < way->tag_count; ++index) {
        const PbfTag *tag = &way->tags[index];
        if (text_equals(tag->key, "highway")) {
            style_out->r = 215U;
            style_out->g = 96U;
            style_out->b = 72U;
            style_out->width = 3U;
            return 1;
        }
        if (text_equals(tag->key, "railway")) {
            style_out->r = 95U;
            style_out->g = 95U;
            style_out->b = 105U;
            style_out->width = 2U;
            return 1;
        }
        if (text_equals(tag->key, "waterway") || (text_equals(tag->key, "natural") && text_equals(tag->value, "water"))) {
            style_out->r = 70U;
            style_out->g = 130U;
            style_out->b = 190U;
            style_out->width = 2U;
            return 1;
        }
        if (text_equals(tag->key, "building")) {
            has_building = 1;
        }
    }
    if (has_building) {
        style_out->r = 150U;
        style_out->g = 125U;
        style_out->b = 95U;
        style_out->width = 1U;
        return 1;
    }
    return 0;
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

static int project_point(const OsmRenderContext *context, const OsmRenderNodeEntry *node, int *x_out, int *y_out) {
    long long lon_span = context->max_lon_nano - context->min_lon_nano;
    long long lat_span = context->max_lat_nano - context->min_lat_nano;
    long long x_num = node->lon_nano - context->min_lon_nano;
    long long y_num = context->max_lat_nano - node->lat_nano;

    if (lon_span <= 0 || lat_span <= 0) return -1;
    *x_out = (int)((x_num * (long long)(context->width - 1U)) / lon_span);
    *y_out = (int)((y_num * (long long)(context->height - 1U)) / lat_span);
    return 0;
}

static void put_pixel(OsmRenderContext *context, int x, int y, const OsmRenderStyle *style) {
    unsigned char *pixel;

    if (x < 0 || y < 0 || x >= (int)context->width || y >= (int)context->height) return;
    pixel = context->pixels + ((size_t)y * (size_t)context->width + (size_t)x) * 3U;
    pixel[0] = style->r;
    pixel[1] = style->g;
    pixel[2] = style->b;
}

static void put_brush(OsmRenderContext *context, int x, int y, const OsmRenderStyle *style) {
    int radius = style->width > 1U ? (int)(style->width / 2U) : 0;
    int dy;

    for (dy = -radius; dy <= radius; ++dy) {
        int dx;
        for (dx = -radius; dx <= radius; ++dx) {
            put_pixel(context, x + dx, y + dy, style);
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
        put_brush(context, x0, y0, style);
        if (x0 == x1 && y0 == y1) break;
        if (2 * err >= dy) {
            err += dy;
            x0 += sx;
        }
        if (2 * err <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    context->segments_drawn += 1ULL;
}

static int on_node(void *user, const PbfNode *node) {
    OsmRenderContext *context = (OsmRenderContext *)user;
    OsmRenderNodeEntry point;
    OsmRenderStyle point_style;
    int x;
    int y;

    if (!node_in_bbox(context, node->lat_nano, node->lon_nano)) return 0;
    if (node_map_insert(context, node->id, node->lat_nano, node->lon_nano) != 0) {
        context->failed = 1;
        return 1;
    }
    if (context->node_points) {
        point.id = node->id;
        point.lat_nano = node->lat_nano;
        point.lon_nano = node->lon_nano;
        point.used = 1;
        point_style.r = 82U;
        point_style.g = 116U;
        point_style.b = 92U;
        point_style.width = 1U;
        if (project_point(context, &point, &x, &y) == 0) put_pixel(context, x, y, &point_style);
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
    OsmRenderNodeEntry previous;
    int has_previous = 0;
    unsigned int index;
    int drew = 0;

    context->ways_seen += 1ULL;
    if (!classify_way(way, &style)) return 0;
    for (index = 0U; index < way->ref_count; ++index) {
        OsmRenderNodeEntry node;
        if (!node_map_find(context, way->refs[index], &node)) {
            has_previous = 0;
            continue;
        }
        if (has_previous) {
            int x0;
            int y0;
            int x1;
            int y1;
            if (project_point(context, &previous, &x0, &y0) == 0 && project_point(context, &node, &x1, &y1) == 0) {
                draw_line(context, x0, y0, x1, y1, &style);
                drew = 1;
            }
        }
        previous = node;
        has_previous = 1;
    }
    if (drew) {
        context->ways_drawn += 1ULL;
        if (context->stop_after_drawn != 0U && context->ways_drawn >= (unsigned long long)context->stop_after_drawn) {
            context->stopped_after_drawn = 1;
            return 1;
        }
    }
    return 0;
}

static int on_way_tags(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    PbfWay way;
    OsmRenderStyle style;

    (void)user;
    rt_memset(&way, 0, sizeof(way));
    way.id = id;
    way.tags = tags;
    way.tag_count = tag_count;
    return classify_way(&way, &style);
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
        context->pixels[index * 3U + 0U] = 242U;
        context->pixels[index * 3U + 1U] = 239U;
        context->pixels[index * 3U + 2U] = 232U;
    }
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmrender";
    const char *pbf_path;
    const char *out_path;
    OsmRenderContext context;
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
        } else if (rt_strcmp(argv[argi], "--node-points") == 0) {
            context.node_points = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--stop-after-nodes") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.stop_after_nodes) != 0) {
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
    context.pixels = (unsigned char *)rt_malloc((size_t)context.width * (size_t)context.height * 3U);
    if (context.pixels == 0) {
        rt_write_cstr(2, "osmrender: out of memory\n");
        return 1;
    }
    fill_background(&context);

    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS;
    callbacks.node = on_node;
    callbacks.way_tags = on_way_tags;
    callbacks.way = on_way;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
        rt_write_cstr(2, "osmrender: ");
        rt_write_cstr(2, context.failed ? "out of memory while collecting bbox nodes" : (error[0] == '\0' ? "failed to render PBF" : error));
        rt_write_char(2, '\n');
        rt_free(context.nodes);
        rt_free(context.pixels);
        return 1;
    }
    if (write_bmp(out_path, &context) != 0) {
        rt_write_cstr(2, "osmrender: could not write BMP\n");
        rt_free(context.nodes);
        rt_free(context.pixels);
        return 1;
    }
    rt_write_cstr(1, "nodes_in_bbox: ");
    rt_write_uint(1, context.node_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_seen: ");
    rt_write_uint(1, context.ways_seen);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_drawn: ");
    rt_write_uint(1, context.ways_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "segments_drawn: ");
    rt_write_uint(1, context.segments_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "bounded: ");
    rt_write_cstr(1, (context.stopped_after_nodes || context.stopped_after_drawn) ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "node_points: ");
    rt_write_cstr(1, context.node_points ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_free(context.nodes);
    rt_free(context.pixels);
    return 0;
}
