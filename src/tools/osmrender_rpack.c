#include "osmrpack.h"

#include "compression/crc32.h"
#include "compression/zlib.h"
#include "osm_index.h"
#include "platform.h"
#include "runtime.h"

#define PACK_STYLE_WATER 0U
#define PACK_STYLE_WATERWAY 1U
#define PACK_STYLE_FOREST 2U
#define PACK_STYLE_PARK 3U
#define PACK_STYLE_BUILDING 4U
#define PACK_STYLE_MOTORWAY 5U
#define PACK_STYLE_PRIMARY 6U
#define PACK_STYLE_SECONDARY 7U
#define PACK_STYLE_MINOR_ROAD 8U
#define PACK_STYLE_PATH 9U
#define PACK_STYLE_RAIL 10U
#define PACK_STYLE_BOUNDARY 11U
#define PACK_STYLE_COUNT 12U

#define RENDER_STEP_AREA 0U
#define RENDER_STEP_CASING 1U
#define RENDER_STEP_LINE 2U

#define STYLE_FLAG_LINE (1U << 0)
#define STYLE_FLAG_FILL (1U << 1)
#define STYLE_FLAG_CASING (1U << 2)

#define OSMRPACK_V2_HEADER_SIZE 256U
#define OSMRPACK_V2_PLACE_RECORD_SIZE 112U
#define OSMRPACK_V2_TILE_RECORD_SIZE 96U

typedef struct {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
    unsigned char alpha;
    unsigned char fill_red;
    unsigned char fill_green;
    unsigned char fill_blue;
    unsigned char fill_alpha;
    unsigned char casing_red;
    unsigned char casing_green;
    unsigned char casing_blue;
    unsigned char casing_alpha;
    unsigned int width;
    unsigned int casing_width;
    unsigned int flags;
} RenderStyle;

typedef struct {
    unsigned int step;
    unsigned int style_id;
} RenderLayer;

typedef struct {
    unsigned int style_id;
    unsigned int flags;
    unsigned int point_offset;
    unsigned int point_count;
} RenderFeature;

typedef struct {
    int x;
    int y;
} BoundaryEndpoint;

typedef struct {
    unsigned int count;
    unsigned int colors[256];
    short table[1024];
} PngPalette;

typedef struct {
    unsigned int count;
    unsigned long long red_sum;
    unsigned long long green_sum;
    unsigned long long blue_sum;
} PngColorBucket;

typedef struct {
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    unsigned int width;
    unsigned int height;
    unsigned char *pixels;
    RenderFeature *features;
    int *points;
    unsigned int feature_count;
    unsigned int feature_capacity;
    unsigned int point_count;
    unsigned int point_capacity;
    RenderStyle styles[PACK_STYLE_COUNT];
    const char *city_name;
    const char *node_index_path;
    const char *way_index_path;
    const char *relation_index_path;
    unsigned long long v2_boundary_payload_offset;
    unsigned long long v2_boundary_payload_size;
    unsigned int v2_boundary_feature_count;
    unsigned int boundary_feature_count;
    int city_enabled;
    int boundary_fade;
    int boundary_fade_applied;
    int png_palette;
    unsigned long long features_seen;
    unsigned long long features_skipped;
    unsigned long long features_collected;
    unsigned long long points_skipped;
    unsigned long long points_collected;
    unsigned long long collect_skip_count;
    unsigned long long collect_header_bytes;
    unsigned long long collect_skipped_bytes;
    unsigned long long collect_point_bytes;
    unsigned long long collect_refills;
    unsigned long long collect_bytes_read;
    unsigned long long features_drawn;
    unsigned long long segments_drawn;
    unsigned long long polygons_drawn;
} RenderContext;

typedef struct {
    unsigned int style_id;
    unsigned int flags;
    unsigned int point_count;
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
} PackFeatureHeader;

typedef struct {
    int fd;
    unsigned char *buffer;
    size_t capacity;
    size_t position;
    size_t used;
    unsigned long long refill_count;
    unsigned long long bytes_read;
} PackReader;

typedef struct {
    unsigned int version;
    unsigned int header_size;
    unsigned int flags;
    unsigned int tile_zoom;
    unsigned int tile_halo;
    unsigned int layer_count;
    unsigned int place_record_size;
    unsigned int tile_record_size;
    unsigned int feature_record_size;
    unsigned long long place_count;
    unsigned long long tile_count;
    unsigned long long place_directory_offset;
    unsigned long long place_directory_size;
    unsigned long long tile_directory_offset;
    unsigned long long tile_directory_size;
    unsigned long long tile_range_index_offset;
    unsigned long long tile_range_index_size;
    unsigned long long tile_payload_offset;
    unsigned long long tile_payload_size;
    unsigned long long string_table_offset;
    unsigned long long string_table_size;
    unsigned long long source_nodes;
    unsigned long long source_ways;
    unsigned long long source_relations;
} OsmrPackV2Header;

typedef struct {
    unsigned long long tile_id;
    unsigned int z;
    unsigned int x;
    unsigned int y;
    unsigned int feature_count;
    unsigned int layer_mask;
    unsigned long long payload_offset;
    unsigned long long payload_size;
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
} OsmrPackV2TileRecord;

typedef struct {
    unsigned long long *entries;
    unsigned int count;
    unsigned int capacity;
} FeatureHashSet;

static const unsigned char osmrpack_v2_magic[8] = { 'O', 'S', 'M', 'R', 'P', 'K', '0', '2' };

static const RenderLayer render_layers[] = {
    { RENDER_STEP_AREA, PACK_STYLE_WATER },
    { RENDER_STEP_AREA, PACK_STYLE_FOREST },
    { RENDER_STEP_AREA, PACK_STYLE_PARK },
    { RENDER_STEP_AREA, PACK_STYLE_BUILDING },
    { RENDER_STEP_LINE, PACK_STYLE_WATER },
    { RENDER_STEP_LINE, PACK_STYLE_FOREST },
    { RENDER_STEP_LINE, PACK_STYLE_PARK },
    { RENDER_STEP_LINE, PACK_STYLE_BUILDING },
    { RENDER_STEP_LINE, PACK_STYLE_WATERWAY },
    { RENDER_STEP_LINE, PACK_STYLE_PATH },
    { RENDER_STEP_CASING, PACK_STYLE_MINOR_ROAD },
    { RENDER_STEP_CASING, PACK_STYLE_SECONDARY },
    { RENDER_STEP_CASING, PACK_STYLE_PRIMARY },
    { RENDER_STEP_CASING, PACK_STYLE_MOTORWAY },
    { RENDER_STEP_LINE, PACK_STYLE_MINOR_ROAD },
    { RENDER_STEP_LINE, PACK_STYLE_SECONDARY },
    { RENDER_STEP_LINE, PACK_STYLE_PRIMARY },
    { RENDER_STEP_LINE, PACK_STYLE_MOTORWAY },
    { RENDER_STEP_CASING, PACK_STYLE_RAIL },
    { RENDER_STEP_LINE, PACK_STYLE_RAIL },
    { RENDER_STEP_CASING, PACK_STYLE_BOUNDARY },
    { RENDER_STEP_LINE, PACK_STYLE_BOUNDARY }
};

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " PACK.rpack OUT.png (--bbox MINLON,MINLAT,MAXLON,MAXLAT | --city NAME) [--width N] [--height N] [--node-index FILE] [--way-index FILE] [--relation-index FILE] [--no-boundary-fade] [--png-rgb] [--profile]\n");
}

static void write_profile_ms(const char *label, unsigned long long elapsed_ms) {
    rt_write_cstr(1, label);
    rt_write_uint(1, elapsed_ms);
    rt_write_char(1, '\n');
}

static int path_exists(const char *path) {
    int fd;

    if (path == 0) return 0;
    fd = platform_open_read(path);
    if (fd < 0) return 0;
    (void)platform_close(fd);
    return 1;
}

static int infer_index_path_from_pack(const char *pack_path, const char *extension, char *buffer, size_t buffer_size) {
    size_t length = rt_strlen(pack_path);
    size_t dot = length;
    size_t prefix_size;

    while (dot > 0U && pack_path[dot - 1U] != '.') dot -= 1U;
    if (dot == 0U) return -1;
    prefix_size = dot - 1U;
    if (prefix_size + rt_strlen(extension) + 1U > buffer_size) return -1;
    memcpy(buffer, pack_path, prefix_size);
    memcpy(buffer + prefix_size, extension, rt_strlen(extension) + 1U);
    return 0;
}

static int parse_uint_arg(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 10000ULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static unsigned int clamp_dimension(unsigned long long value) {
    if (value < 1ULL) return 1U;
    if (value > 10000ULL) return 10000U;
    return (unsigned int)value;
}

static void choose_missing_dimension(RenderContext *context, int width_explicit, int height_explicit) {
    unsigned long long lon_span;
    unsigned long long lat_span;
    unsigned long long scaled_lon_span;

    if (width_explicit == height_explicit) return;
    if (context->max_lon_nano <= context->min_lon_nano || context->max_lat_nano <= context->min_lat_nano) return;
    lon_span = (unsigned long long)(context->max_lon_nano - context->min_lon_nano);
    lat_span = (unsigned long long)(context->max_lat_nano - context->min_lat_nano);
    scaled_lon_span = (lon_span * 3ULL + 2ULL) / 5ULL;
    if (scaled_lon_span == 0ULL) scaled_lon_span = 1ULL;
    if (width_explicit) {
        unsigned long long height = (lat_span * (unsigned long long)context->width + scaled_lon_span / 2ULL) / scaled_lon_span;
        context->height = clamp_dimension(height);
    } else {
        unsigned long long width = (scaled_lon_span * (unsigned long long)context->height + lat_span / 2ULL) / lat_span;
        context->width = clamp_dimension(width);
    }
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

static int parse_bbox_arg(const char *text, RenderContext *context) {
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

static int set_city_bbox(const char *city, RenderContext *context) {
    if (rt_strcmp(city, "Potsdam") == 0) return parse_bbox_arg("12.88,52.32,13.18,52.50", context);
    if (rt_strcmp(city, "Berlin") == 0) return parse_bbox_arg("13.05,52.33,13.80,52.68", context);
    return -1;
}

static int render_bbox_is_valid(const RenderContext *context) {
    return context->min_lon_nano < context->max_lon_nano && context->min_lat_nano < context->max_lat_nano;
}

static void set_line_style(RenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned int width) {
    style->red = red;
    style->green = green;
    style->blue = blue;
    style->alpha = 255U;
    style->width = width;
    style->flags |= STYLE_FLAG_LINE;
}

static void set_fill_style(RenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
    style->fill_red = red;
    style->fill_green = green;
    style->fill_blue = blue;
    style->fill_alpha = alpha;
    style->flags |= STYLE_FLAG_FILL;
}

static void set_casing_style(RenderStyle *style, unsigned char red, unsigned char green, unsigned char blue, unsigned int width) {
    style->casing_red = red;
    style->casing_green = green;
    style->casing_blue = blue;
    style->casing_alpha = 220U;
    style->casing_width = width;
    style->flags |= STYLE_FLAG_CASING;
}

static void styles_init(RenderContext *context) {
    rt_memset(context->styles, 0, sizeof(context->styles));
    set_fill_style(&context->styles[PACK_STYLE_WATER], 154U, 196U, 214U, 230U);
    set_line_style(&context->styles[PACK_STYLE_WATER], 94U, 151U, 183U, 2U);
    set_line_style(&context->styles[PACK_STYLE_WATERWAY], 92U, 158U, 194U, 2U);
    set_fill_style(&context->styles[PACK_STYLE_FOREST], 176U, 205U, 165U, 190U);
    set_line_style(&context->styles[PACK_STYLE_FOREST], 134U, 168U, 121U, 1U);
    set_fill_style(&context->styles[PACK_STYLE_PARK], 190U, 218U, 170U, 185U);
    set_line_style(&context->styles[PACK_STYLE_PARK], 142U, 181U, 119U, 1U);
    set_fill_style(&context->styles[PACK_STYLE_BUILDING], 217U, 203U, 184U, 175U);
    set_line_style(&context->styles[PACK_STYLE_BUILDING], 157U, 138U, 117U, 1U);
    set_casing_style(&context->styles[PACK_STYLE_MOTORWAY], 238U, 236U, 228U, 5U);
    set_line_style(&context->styles[PACK_STYLE_MOTORWAY], 82U, 82U, 78U, 3U);
    set_casing_style(&context->styles[PACK_STYLE_PRIMARY], 238U, 236U, 228U, 4U);
    set_line_style(&context->styles[PACK_STYLE_PRIMARY], 92U, 92U, 88U, 2U);
    set_casing_style(&context->styles[PACK_STYLE_SECONDARY], 238U, 236U, 228U, 3U);
    set_line_style(&context->styles[PACK_STYLE_SECONDARY], 112U, 112U, 106U, 1U);
    set_casing_style(&context->styles[PACK_STYLE_MINOR_ROAD], 190U, 182U, 169U, 4U);
    set_line_style(&context->styles[PACK_STYLE_MINOR_ROAD], 250U, 248U, 240U, 2U);
    set_line_style(&context->styles[PACK_STYLE_PATH], 147U, 139U, 122U, 1U);
    set_casing_style(&context->styles[PACK_STYLE_RAIL], 222U, 218U, 210U, 3U);
    set_line_style(&context->styles[PACK_STYLE_RAIL], 92U, 91U, 101U, 1U);
    set_casing_style(&context->styles[PACK_STYLE_BOUNDARY], 244U, 239U, 225U, 5U);
    set_line_style(&context->styles[PACK_STYLE_BOUNDARY], 119U, 88U, 121U, 3U);
}

static unsigned int read_u32_le(const unsigned char *data) {
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8U) | ((unsigned int)data[2] << 16U) | ((unsigned int)data[3] << 24U);
}

static unsigned long long read_u64_le(const unsigned char *data) {
    unsigned long long value = 0ULL;
    unsigned int byte_index;

    for (byte_index = 0U; byte_index < 8U; ++byte_index) value |= ((unsigned long long)data[byte_index]) << (byte_index * 8U);
    return value;
}

static long long read_i64_le(const unsigned char *data) {
    return (long long)read_u64_le(data);
}

static int read_v2_header_bytes(const unsigned char data[OSMRPACK_V2_HEADER_SIZE], OsmrPackV2Header *header) {
    if (memcmp(data, osmrpack_v2_magic, sizeof(osmrpack_v2_magic)) != 0) return 0;
    rt_memset(header, 0, sizeof(*header));
    header->version = read_u32_le(data + 8U);
    header->header_size = read_u32_le(data + 12U);
    header->flags = read_u32_le(data + 16U);
    header->tile_zoom = read_u32_le(data + 20U);
    header->tile_halo = read_u32_le(data + 24U);
    header->layer_count = read_u32_le(data + 28U);
    header->place_record_size = read_u32_le(data + 32U);
    header->tile_record_size = read_u32_le(data + 36U);
    header->feature_record_size = read_u32_le(data + 40U);
    header->place_count = read_u64_le(data + 48U);
    header->tile_count = read_u64_le(data + 56U);
    header->place_directory_offset = read_u64_le(data + 64U);
    header->place_directory_size = read_u64_le(data + 72U);
    header->tile_directory_offset = read_u64_le(data + 80U);
    header->tile_directory_size = read_u64_le(data + 88U);
    header->tile_range_index_offset = read_u64_le(data + 96U);
    header->tile_range_index_size = read_u64_le(data + 104U);
    header->tile_payload_offset = read_u64_le(data + 112U);
    header->tile_payload_size = read_u64_le(data + 120U);
    header->string_table_offset = read_u64_le(data + 128U);
    header->string_table_size = read_u64_le(data + 136U);
    header->source_nodes = read_u64_le(data + 144U);
    header->source_ways = read_u64_le(data + 152U);
    header->source_relations = read_u64_le(data + 160U);
    if (header->version != 2U || header->header_size != OSMRPACK_V2_HEADER_SIZE) return -1;
    if (header->place_record_size != OSMRPACK_V2_PLACE_RECORD_SIZE || header->tile_record_size != OSMRPACK_V2_TILE_RECORD_SIZE) return -1;
    if (header->feature_record_size != OSMRPACK_FEATURE_HEADER_SIZE || header->tile_zoom > 29U) return -1;
    return 1;
}

static void write_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)((value >> 24U) & 0xffU);
    out[1] = (unsigned char)((value >> 16U) & 0xffU);
    out[2] = (unsigned char)((value >> 8U) & 0xffU);
    out[3] = (unsigned char)(value & 0xffU);
}

static int read_exact(int fd, void *buffer, size_t size) {
    unsigned char *cursor = (unsigned char *)buffer;
    size_t offset = 0U;

    while (offset < size) {
        long amount = platform_read(fd, cursor + offset, size - offset);
        if (amount <= 0) return -1;
        offset += (size_t)amount;
    }
    return 0;
}

static int read_v2_header_fd(int fd, OsmrPackV2Header *header) {
    unsigned char data[OSMRPACK_V2_HEADER_SIZE];
    int result;

    if (platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) return -1;
    if (read_exact(fd, data, sizeof(data)) != 0) return -1;
    result = read_v2_header_bytes(data, header);
    return result < 0 ? -1 : result;
}

static int read_v2_header_path(const char *path, OsmrPackV2Header *header) {
    int fd = platform_open_read(path);
    int result;

    if (fd < 0) return -1;
    result = read_v2_header_fd(fd, header);
    if (platform_close(fd) != 0 && result >= 0) result = -1;
    return result;
}

static void read_v2_tile_record_bytes(const unsigned char data[OSMRPACK_V2_TILE_RECORD_SIZE], OsmrPackV2TileRecord *record) {
    rt_memset(record, 0, sizeof(*record));
    record->tile_id = read_u64_le(data + 0U);
    record->z = read_u32_le(data + 8U);
    record->x = read_u32_le(data + 12U);
    record->y = read_u32_le(data + 16U);
    record->feature_count = read_u32_le(data + 20U);
    record->layer_mask = read_u32_le(data + 24U);
    record->payload_offset = read_u64_le(data + 32U);
    record->payload_size = read_u64_le(data + 40U);
    record->min_lon_nano = read_i64_le(data + 48U);
    record->min_lat_nano = read_i64_le(data + 56U);
    record->max_lon_nano = read_i64_le(data + 64U);
    record->max_lat_nano = read_i64_le(data + 72U);
}

static int read_v2_tile_record_fd(int fd, OsmrPackV2TileRecord *record) {
    unsigned char data[OSMRPACK_V2_TILE_RECORD_SIZE];
    if (read_exact(fd, data, sizeof(data)) != 0) return -1;
    read_v2_tile_record_bytes(data, record);
    return 0;
}

static unsigned int v2_tile_axis(unsigned int zoom) {
    return 1U << zoom;
}

static unsigned int v2_lon_to_tile_x(long long lon_nano, unsigned int zoom) {
    long long shifted = lon_nano + 180000000000LL;
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    unsigned long long value;
    if (shifted < 0) shifted = 0;
    if (shifted > 360000000000LL) shifted = 360000000000LL;
    value = ((unsigned long long)shifted * axis) / 360000000000ULL;
    if (value >= axis) value = axis - 1ULL;
    return (unsigned int)value;
}

static unsigned int v2_lat_to_tile_y(long long lat_nano, unsigned int zoom) {
    long long shifted = 90000000000LL - lat_nano;
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    unsigned long long value;
    if (shifted < 0) shifted = 0;
    if (shifted > 180000000000LL) shifted = 180000000000LL;
    value = ((unsigned long long)shifted * axis) / 180000000000ULL;
    if (value >= axis) value = axis - 1ULL;
    return (unsigned int)value;
}

static unsigned long long feature_hash_mix(unsigned long long hash, unsigned long long value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    return hash == 0ULL ? 1ULL : hash;
}

static unsigned long long feature_hash_init(const PackFeatureHeader *feature) {
    unsigned long long hash = 1469598103934665603ULL;
    hash = feature_hash_mix(hash, feature->style_id);
    hash = feature_hash_mix(hash, feature->flags);
    hash = feature_hash_mix(hash, feature->point_count);
    hash = feature_hash_mix(hash, (unsigned long long)feature->min_lon_nano);
    hash = feature_hash_mix(hash, (unsigned long long)feature->min_lat_nano);
    hash = feature_hash_mix(hash, (unsigned long long)feature->max_lon_nano);
    hash = feature_hash_mix(hash, (unsigned long long)feature->max_lat_nano);
    return hash;
}

static void feature_hash_set_destroy(FeatureHashSet *set) {
    rt_free(set->entries);
    set->entries = 0;
    set->count = 0U;
    set->capacity = 0U;
}

static int feature_hash_set_rehash(FeatureHashSet *set, unsigned int new_capacity) {
    unsigned long long *old_entries = set->entries;
    unsigned int old_capacity = set->capacity;
    unsigned int index;

    set->entries = (unsigned long long *)rt_malloc(sizeof(*set->entries) * (size_t)new_capacity);
    if (set->entries == 0) {
        set->entries = old_entries;
        return -1;
    }
    rt_memset(set->entries, 0, sizeof(*set->entries) * (size_t)new_capacity);
    set->capacity = new_capacity;
    set->count = 0U;
    for (index = 0U; index < old_capacity; ++index) {
        unsigned long long hash = old_entries[index];
        if (hash != 0ULL) {
            unsigned int slot = (unsigned int)hash & (set->capacity - 1U);
            while (set->entries[slot] != 0ULL) slot = (slot + 1U) & (set->capacity - 1U);
            set->entries[slot] = hash;
            set->count += 1U;
        }
    }
    rt_free(old_entries);
    return 0;
}

static int feature_hash_set_insert(FeatureHashSet *set, unsigned long long hash) {
    unsigned int slot;

    if (hash == 0ULL) hash = 1ULL;
    if (set->capacity == 0U) {
        if (feature_hash_set_rehash(set, 65536U) != 0) return -1;
    } else if ((set->count + 1U) * 2U >= set->capacity) {
        if (set->capacity > 0x40000000U || feature_hash_set_rehash(set, set->capacity * 2U) != 0) return -1;
    }
    slot = (unsigned int)hash & (set->capacity - 1U);
    while (set->entries[slot] != 0ULL) {
        if (set->entries[slot] == hash) return 0;
        slot = (slot + 1U) & (set->capacity - 1U);
    }
    set->entries[slot] = hash;
    set->count += 1U;
    return 1;
}

static int read_feature_header(int fd, PackFeatureHeader *header) {
    unsigned char data[OSMRPACK_FEATURE_HEADER_SIZE];

    if (read_exact(fd, data, sizeof(data)) != 0) return -1;
    header->style_id = read_u32_le(data + 0U);
    header->flags = read_u32_le(data + 4U);
    header->point_count = read_u32_le(data + 8U);
    header->min_lon_nano = read_i64_le(data + 16U);
    header->min_lat_nano = read_i64_le(data + 24U);
    header->max_lon_nano = read_i64_le(data + 32U);
    header->max_lat_nano = read_i64_le(data + 40U);
    return 0;
}

static int pack_reader_init(PackReader *reader, int fd, size_t capacity) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
    reader->capacity = capacity;
    reader->buffer = (unsigned char *)rt_malloc(capacity);
    return reader->buffer == 0 ? -1 : 0;
}

static void pack_reader_destroy(PackReader *reader) {
    rt_free(reader->buffer);
    reader->buffer = 0;
    reader->capacity = 0U;
    reader->position = 0U;
    reader->used = 0U;
}

static int pack_reader_refill(PackReader *reader) {
    long amount;

    amount = platform_read(reader->fd, reader->buffer, reader->capacity);
    if (amount <= 0) return -1;
    reader->position = 0U;
    reader->used = (size_t)amount;
    reader->refill_count += 1ULL;
    reader->bytes_read += (unsigned long long)amount;
    return 0;
}

static int pack_reader_read_exact(PackReader *reader, void *buffer, size_t size) {
    unsigned char *out = (unsigned char *)buffer;
    size_t copied = 0U;

    while (copied < size) {
        size_t available;
        size_t chunk;

        if (reader->position >= reader->used && pack_reader_refill(reader) != 0) return -1;
        available = reader->used - reader->position;
        chunk = size - copied < available ? size - copied : available;
        memcpy(out + copied, reader->buffer + reader->position, chunk);
        reader->position += chunk;
        copied += chunk;
    }
    return 0;
}

static int pack_reader_skip(PackReader *reader, unsigned long long size) {
    while (size != 0ULL) {
        size_t available;
        size_t chunk;

        if (reader->position >= reader->used && pack_reader_refill(reader) != 0) return -1;
        available = reader->used - reader->position;
        chunk = size < (unsigned long long)available ? (size_t)size : available;
        reader->position += chunk;
        size -= (unsigned long long)chunk;
    }
    return 0;
}

static int pack_reader_read_feature_header(PackReader *reader, PackFeatureHeader *header) {
    unsigned char data[OSMRPACK_FEATURE_HEADER_SIZE];

    if (pack_reader_read_exact(reader, data, sizeof(data)) != 0) return -1;
    header->style_id = read_u32_le(data + 0U);
    header->flags = read_u32_le(data + 4U);
    header->point_count = read_u32_le(data + 8U);
    header->min_lon_nano = read_i64_le(data + 16U);
    header->min_lat_nano = read_i64_le(data + 24U);
    header->max_lon_nano = read_i64_le(data + 32U);
    header->max_lat_nano = read_i64_le(data + 40U);
    return 0;
}

static int feature_intersects_render_bbox(const RenderContext *context, const PackFeatureHeader *feature) {
    if (feature->max_lon_nano < context->min_lon_nano || feature->min_lon_nano > context->max_lon_nano) return 0;
    if (feature->max_lat_nano < context->min_lat_nano || feature->min_lat_nano > context->max_lat_nano) return 0;
    return 1;
}

static int skip_points(int fd, unsigned int point_count) {
    unsigned long long bytes = (unsigned long long)point_count * 16ULL;

    return platform_seek(fd, (long long)bytes, PLATFORM_SEEK_CUR) < 0 ? -1 : 0;
}

static int read_points(int fd, long long *points, unsigned int point_count) {
    unsigned char data[16];
    unsigned int point_index;

    for (point_index = 0U; point_index < point_count; ++point_index) {
        if (read_exact(fd, data, sizeof(data)) != 0) return -1;
        points[point_index * 2U + 0U] = read_i64_le(data + 0U);
        points[point_index * 2U + 1U] = read_i64_le(data + 8U);
    }
    return 0;
}

static int grow_render_features(RenderContext *context, unsigned int needed) {
    unsigned int capacity = context->feature_capacity == 0U ? 4096U : context->feature_capacity;
    RenderFeature *features;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->feature_capacity) return 0;
    features = (RenderFeature *)rt_realloc(context->features, sizeof(*features) * (size_t)capacity);
    if (features == 0) return -1;
    context->features = features;
    context->feature_capacity = capacity;
    return 0;
}

static int grow_render_points(RenderContext *context, unsigned int needed) {
    unsigned int capacity = context->point_capacity == 0U ? 16384U : context->point_capacity;
    int *points;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->point_capacity) return 0;
    points = (int *)rt_realloc(context->points, sizeof(*points) * (size_t)capacity);
    if (points == 0) return -1;
    context->points = points;
    context->point_capacity = capacity;
    return 0;
}

static int clamp_to_int(long long value) {
    if (value < -2147483647LL) return -2147483647;
    if (value > 2147483647LL) return 2147483647;
    return (int)value;
}

static int project_point(const RenderContext *context, long long lon_nano, long long lat_nano, long long *pixel_x_out, long long *pixel_y_out) {
    long long lon_span = context->max_lon_nano - context->min_lon_nano;
    long long lat_span = context->max_lat_nano - context->min_lat_nano;

    if (lon_span <= 0 || lat_span <= 0) return -1;
    *pixel_x_out = ((lon_nano - context->min_lon_nano) * (long long)(context->width - 1U)) / lon_span;
    *pixel_y_out = ((context->max_lat_nano - lat_nano) * (long long)(context->height - 1U)) / lat_span;
    return 0;
}

static int append_render_feature(RenderContext *context, unsigned int style_id, unsigned int flags, unsigned int point_count, const int *projected_points) {
    unsigned int value_count = point_count * 2U;
    RenderFeature *render_feature;

    if (context->point_count > 0xffffffffU - value_count) return -1;
    if (grow_render_features(context, context->feature_count + 1U) != 0) return -1;
    if (grow_render_points(context, context->point_count + value_count) != 0) return -1;
    memcpy(context->points + context->point_count, projected_points, sizeof(int) * (size_t)value_count);
    render_feature = &context->features[context->feature_count];
    render_feature->style_id = style_id;
    render_feature->flags = flags;
    render_feature->point_offset = context->point_count;
    render_feature->point_count = point_count;
    context->point_count += value_count;
    context->feature_count += 1U;
    return 0;
}

static int append_visible_feature(RenderContext *context, const PackFeatureHeader *feature, const int *projected_points) {
    return append_render_feature(context, feature->style_id, feature->flags, feature->point_count, projected_points);
}

static int read_projected_feature_points(int fd, const PackFeatureHeader *feature, RenderContext *context, int *projected_points) {
    unsigned char data[16];
    unsigned int point_index;

    for (point_index = 0U; point_index < feature->point_count; ++point_index) {
        long long pixel_x;
        long long pixel_y;
        long long lon_nano;
        long long lat_nano;

        if (read_exact(fd, data, sizeof(data)) != 0) return -1;
        lon_nano = read_i64_le(data + 0U);
        lat_nano = read_i64_le(data + 8U);
        if (project_point(context, lon_nano, lat_nano, &pixel_x, &pixel_y) != 0) return -1;
        projected_points[point_index * 2U + 0U] = clamp_to_int(pixel_x);
        projected_points[point_index * 2U + 1U] = clamp_to_int(pixel_y);
    }
    return 0;
}

static int pack_reader_read_projected_feature_points(PackReader *reader, const PackFeatureHeader *feature, RenderContext *context, int *projected_points) {
    unsigned char data[16];
    unsigned int point_index;

    for (point_index = 0U; point_index < feature->point_count; ++point_index) {
        long long pixel_x;
        long long pixel_y;
        long long lon_nano;
        long long lat_nano;

        if (pack_reader_read_exact(reader, data, sizeof(data)) != 0) return -1;
        lon_nano = read_i64_le(data + 0U);
        lat_nano = read_i64_le(data + 8U);
        if (project_point(context, lon_nano, lat_nano, &pixel_x, &pixel_y) != 0) return -1;
        projected_points[point_index * 2U + 0U] = clamp_to_int(pixel_x);
        projected_points[point_index * 2U + 1U] = clamp_to_int(pixel_y);
    }
    return 0;
}

static int pack_reader_read_projected_feature_points_with_hash(PackReader *reader, const PackFeatureHeader *feature, RenderContext *context, int *projected_points, unsigned long long *hash_out) {
    unsigned char data[16];
    unsigned int point_index;
    unsigned long long hash = feature_hash_init(feature);

    for (point_index = 0U; point_index < feature->point_count; ++point_index) {
        long long pixel_x;
        long long pixel_y;
        long long lon_nano;
        long long lat_nano;

        if (pack_reader_read_exact(reader, data, sizeof(data)) != 0) return -1;
        lon_nano = read_i64_le(data + 0U);
        lat_nano = read_i64_le(data + 8U);
        hash = feature_hash_mix(hash, (unsigned long long)lon_nano);
        hash = feature_hash_mix(hash, (unsigned long long)lat_nano);
        if (project_point(context, lon_nano, lat_nano, &pixel_x, &pixel_y) != 0) return -1;
        projected_points[point_index * 2U + 0U] = clamp_to_int(pixel_x);
        projected_points[point_index * 2U + 1U] = clamp_to_int(pixel_y);
    }
    *hash_out = hash;
    return 0;
}

static int collect_visible_features(int fd, const OsmrPackHeader *pack_header, RenderContext *context) {
    unsigned char count_data[8];
    unsigned long long feature_count;
    unsigned long long feature_index;
    PackReader reader;
    int result = -1;

    if (platform_seek(fd, (long long)pack_header->feature_data_offset, PLATFORM_SEEK_SET) < 0) return -1;
    if (pack_reader_init(&reader, fd, 1024U * 1024U) != 0) return -1;
    if (pack_reader_read_exact(&reader, count_data, sizeof(count_data)) != 0) goto cleanup;
    feature_count = read_u64_le(count_data);
    for (feature_index = 0ULL; feature_index < feature_count; ++feature_index) {
        PackFeatureHeader feature;
        int *projected_points;
        unsigned long long point_bytes;

        if (pack_reader_read_feature_header(&reader, &feature) != 0) goto cleanup;
        context->collect_header_bytes += OSMRPACK_FEATURE_HEADER_SIZE;
        context->features_seen += 1ULL;
        if (feature.point_count < 2U || feature.point_count > 10000000U || feature.style_id >= PACK_STYLE_COUNT) goto cleanup;
        point_bytes = (unsigned long long)feature.point_count * 16ULL;
        if (!feature_intersects_render_bbox(context, &feature)) {
            context->features_skipped += 1ULL;
            context->points_skipped += feature.point_count;
            context->collect_skip_count += 1ULL;
            context->collect_skipped_bytes += point_bytes;
            if (pack_reader_skip(&reader, point_bytes) != 0) goto cleanup;
            continue;
        }
        projected_points = (int *)rt_malloc(sizeof(int) * (size_t)feature.point_count * 2U);
        if (projected_points == 0) goto cleanup;
        if (pack_reader_read_projected_feature_points(&reader, &feature, context, projected_points) != 0 || append_visible_feature(context, &feature, projected_points) != 0) {
            rt_free(projected_points);
            goto cleanup;
        }
        context->features_collected += 1ULL;
        context->points_collected += feature.point_count;
        context->collect_point_bytes += point_bytes;
        rt_free(projected_points);
    }
    context->collect_refills = reader.refill_count;
    context->collect_bytes_read = reader.bytes_read;
    result = 0;

cleanup:
    if (result != 0) {
        context->collect_refills = reader.refill_count;
        context->collect_bytes_read = reader.bytes_read;
    }
    pack_reader_destroy(&reader);
    return result;
}

static int grow_v2_tile_records(OsmrPackV2TileRecord **records_io, unsigned int *capacity_io, unsigned int needed) {
    unsigned int capacity = *capacity_io == 0U ? 128U : *capacity_io;
    OsmrPackV2TileRecord *records;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == *capacity_io) return 0;
    records = (OsmrPackV2TileRecord *)rt_realloc(*records_io, sizeof(*records) * (size_t)capacity);
    if (records == 0) return -1;
    *records_io = records;
    *capacity_io = capacity;
    return 0;
}

static int v2_place_name_matches(int fd, unsigned long long name_offset, unsigned int name_size, const char *city_name) {
    char buffer[256];
    size_t city_size = rt_strlen(city_name);

    if (name_size != city_size || name_size >= sizeof(buffer)) return 0;
    if (platform_seek(fd, (long long)name_offset, PLATFORM_SEEK_SET) < 0) return -1;
    if (read_exact(fd, buffer, name_size) != 0) return -1;
    buffer[name_size] = '\0';
    return rt_strcmp(buffer, city_name) == 0 ? 1 : 0;
}

static int apply_v2_place_bbox(int fd, const OsmrPackV2Header *header, RenderContext *context) {
    unsigned long long place_index;
    unsigned int best_rank = 0U;
    long long best_min_lon = 0;
    long long best_min_lat = 0;
    long long best_max_lon = 0;
    long long best_max_lat = 0;
    unsigned long long best_boundary_payload_offset = 0ULL;
    unsigned long long best_boundary_payload_size = 0ULL;
    unsigned int best_boundary_feature_count = 0U;
    int found = 0;

    if (!context->city_enabled || context->city_name == 0 || header->place_count == 0ULL) return 0;
    if (platform_seek(fd, (long long)header->place_directory_offset, PLATFORM_SEEK_SET) < 0) return -1;
    for (place_index = 0ULL; place_index < header->place_count; ++place_index) {
        unsigned char data[OSMRPACK_V2_PLACE_RECORD_SIZE];
        long long min_lon;
        long long min_lat;
        long long max_lon;
        long long max_lat;
        unsigned long long name_offset;
        unsigned long long boundary_payload_offset;
        unsigned long long boundary_payload_size;
        unsigned int name_size;
        unsigned int boundary_feature_count;
        unsigned int rank_score;
        long long next_record_offset = (long long)(header->place_directory_offset + (place_index + 1ULL) * OSMRPACK_V2_PLACE_RECORD_SIZE);
        int match;

        if (read_exact(fd, data, sizeof(data)) != 0) return -1;
        rank_score = read_u32_le(data + 20U);
        min_lon = read_i64_le(data + 24U);
        min_lat = read_i64_le(data + 32U);
        max_lon = read_i64_le(data + 40U);
        max_lat = read_i64_le(data + 48U);
        name_offset = read_u64_le(data + 56U);
        name_size = read_u32_le(data + 64U);
        boundary_payload_offset = read_u64_le(data + 68U);
        boundary_payload_size = read_u64_le(data + 76U);
        boundary_feature_count = read_u32_le(data + 84U);
        match = v2_place_name_matches(fd, name_offset, name_size, context->city_name);
        if (match < 0) return -1;
        if (match && min_lon < max_lon && min_lat < max_lat && (!found || rank_score > best_rank)) {
            best_rank = rank_score;
            best_min_lon = min_lon;
            best_min_lat = min_lat;
            best_max_lon = max_lon;
            best_max_lat = max_lat;
            best_boundary_payload_offset = boundary_payload_offset;
            best_boundary_payload_size = boundary_payload_size;
            best_boundary_feature_count = boundary_feature_count;
            found = 1;
        }
        if (platform_seek(fd, next_record_offset, PLATFORM_SEEK_SET) < 0) return -1;
    }
    if (found) {
        long long lon_padding = (best_max_lon - best_min_lon) / 10LL;
        long long lat_padding = (best_max_lat - best_min_lat) / 10LL;
        if (lon_padding < 5000000LL) lon_padding = 5000000LL;
        if (lat_padding < 5000000LL) lat_padding = 5000000LL;
        context->min_lon_nano = best_min_lon - lon_padding;
        context->max_lon_nano = best_max_lon + lon_padding;
        context->min_lat_nano = best_min_lat - lat_padding;
        context->max_lat_nano = best_max_lat + lat_padding;
        context->v2_boundary_payload_offset = best_boundary_payload_offset;
        context->v2_boundary_payload_size = best_boundary_payload_size;
        context->v2_boundary_feature_count = best_boundary_feature_count;
        return 1;
    }
    return 0;
}

static int collect_v2_place_boundary(RenderContext *context, int fd) {
    unsigned char count_data[8];
    unsigned long long feature_count;
    unsigned long long feature_index;
    unsigned long long payload_end;

    if (!context->city_enabled || context->v2_boundary_payload_offset == 0ULL || context->v2_boundary_payload_size < 8ULL || context->v2_boundary_feature_count == 0U) return 0;
    payload_end = context->v2_boundary_payload_offset + context->v2_boundary_payload_size;
    if (payload_end < context->v2_boundary_payload_offset) return -1;
    if (platform_seek(fd, (long long)context->v2_boundary_payload_offset, PLATFORM_SEEK_SET) < 0) return -1;
    if (read_exact(fd, count_data, sizeof(count_data)) != 0) return -1;
    feature_count = read_u64_le(count_data);
    if (feature_count != (unsigned long long)context->v2_boundary_feature_count || feature_count > 1000000ULL) return -1;
    for (feature_index = 0ULL; feature_index < feature_count; ++feature_index) {
        PackFeatureHeader feature;
        int *projected_points;

        if (read_feature_header(fd, &feature) != 0) return -1;
        if (feature.style_id != PACK_STYLE_BOUNDARY || feature.flags != 0U || feature.point_count < 2U || feature.point_count > 10000000U) return -1;
        projected_points = (int *)rt_malloc(sizeof(int) * (size_t)feature.point_count * 2U);
        if (projected_points == 0) return -1;
        if (read_projected_feature_points(fd, &feature, context, projected_points) != 0 || append_visible_feature(context, &feature, projected_points) != 0) {
            rt_free(projected_points);
            return -1;
        }
        context->boundary_feature_count += 1U;
        rt_free(projected_points);
    }
    if (platform_seek(fd, (long long)payload_end, PLATFORM_SEEK_SET) < 0) return -1;
    return 0;
}

static int v2_tile_in_requested_range(const OsmrPackV2TileRecord *record, unsigned int min_x, unsigned int min_y, unsigned int max_x, unsigned int max_y, unsigned int zoom) {
    if (record->z != zoom) return 0;
    if (record->x < min_x || record->x > max_x) return 0;
    if (record->y < min_y || record->y > max_y) return 0;
    return 1;
}

static int collect_visible_features_v2(int fd, const OsmrPackV2Header *header, RenderContext *context, unsigned long long *selected_tile_count_out, unsigned long long *selected_tile_features_out) {
    OsmrPackV2TileRecord *selected_records = 0;
    unsigned int selected_count = 0U;
    unsigned int selected_capacity = 0U;
    unsigned int axis = v2_tile_axis(header->tile_zoom);
    unsigned int min_x = v2_lon_to_tile_x(context->min_lon_nano, header->tile_zoom);
    unsigned int max_x = v2_lon_to_tile_x(context->max_lon_nano, header->tile_zoom);
    unsigned int min_y = v2_lat_to_tile_y(context->max_lat_nano, header->tile_zoom);
    unsigned int max_y = v2_lat_to_tile_y(context->min_lat_nano, header->tile_zoom);
    unsigned int halo = header->tile_halo;
    unsigned long long tile_index;
    FeatureHashSet seen_hashes;
    int result = -1;

    rt_memset(&seen_hashes, 0, sizeof(seen_hashes));
    if (halo > 16U) halo = 16U;
    min_x = min_x > halo ? min_x - halo : 0U;
    min_y = min_y > halo ? min_y - halo : 0U;
    max_x = max_x + halo < axis ? max_x + halo : axis - 1U;
    max_y = max_y + halo < axis ? max_y + halo : axis - 1U;
    *selected_tile_count_out = 0ULL;
    *selected_tile_features_out = 0ULL;
    if (header->tile_count > 10000000ULL) return -1;
    if (platform_seek(fd, (long long)header->tile_directory_offset, PLATFORM_SEEK_SET) < 0) return -1;
    for (tile_index = 0ULL; tile_index < header->tile_count; ++tile_index) {
        OsmrPackV2TileRecord record;
        if (read_v2_tile_record_fd(fd, &record) != 0) goto cleanup;
        if (v2_tile_in_requested_range(&record, min_x, min_y, max_x, max_y, header->tile_zoom)) {
            if (grow_v2_tile_records(&selected_records, &selected_capacity, selected_count + 1U) != 0) goto cleanup;
            selected_records[selected_count++] = record;
            *selected_tile_features_out += record.feature_count;
        }
    }
    *selected_tile_count_out = selected_count;
    for (tile_index = 0ULL; tile_index < (unsigned long long)selected_count; ++tile_index) {
        OsmrPackV2TileRecord *record = &selected_records[(unsigned int)tile_index];
        unsigned char count_data[8];
        unsigned long long feature_count;
        unsigned long long feature_index;
        PackReader reader;

        if (platform_seek(fd, (long long)record->payload_offset, PLATFORM_SEEK_SET) < 0) goto cleanup;
        if (pack_reader_init(&reader, fd, 1024U * 1024U) != 0) goto cleanup;
        if (pack_reader_read_exact(&reader, count_data, sizeof(count_data)) != 0) {
            pack_reader_destroy(&reader);
            goto cleanup;
        }
        feature_count = read_u64_le(count_data);
        if (feature_count != (unsigned long long)record->feature_count) {
            pack_reader_destroy(&reader);
            goto cleanup;
        }
        for (feature_index = 0ULL; feature_index < feature_count; ++feature_index) {
            PackFeatureHeader feature;
            int *projected_points;
            unsigned long long point_bytes;
            unsigned long long feature_hash;
            int inserted;

            if (pack_reader_read_feature_header(&reader, &feature) != 0) {
                pack_reader_destroy(&reader);
                goto cleanup;
            }
            context->collect_header_bytes += OSMRPACK_FEATURE_HEADER_SIZE;
            context->features_seen += 1ULL;
            if (feature.point_count < 2U || feature.point_count > 10000000U || feature.style_id >= PACK_STYLE_COUNT) {
                pack_reader_destroy(&reader);
                goto cleanup;
            }
            point_bytes = (unsigned long long)feature.point_count * 16ULL;
            if (!feature_intersects_render_bbox(context, &feature)) {
                context->features_skipped += 1ULL;
                context->points_skipped += feature.point_count;
                context->collect_skip_count += 1ULL;
                context->collect_skipped_bytes += point_bytes;
                if (pack_reader_skip(&reader, point_bytes) != 0) {
                    pack_reader_destroy(&reader);
                    goto cleanup;
                }
                continue;
            }
            projected_points = (int *)rt_malloc(sizeof(int) * (size_t)feature.point_count * 2U);
            if (projected_points == 0) {
                pack_reader_destroy(&reader);
                goto cleanup;
            }
            if (pack_reader_read_projected_feature_points_with_hash(&reader, &feature, context, projected_points, &feature_hash) != 0) {
                rt_free(projected_points);
                pack_reader_destroy(&reader);
                goto cleanup;
            }
            inserted = feature_hash_set_insert(&seen_hashes, feature_hash);
            if (inserted < 0 || (inserted > 0 && append_visible_feature(context, &feature, projected_points) != 0)) {
                rt_free(projected_points);
                pack_reader_destroy(&reader);
                goto cleanup;
            }
            if (inserted > 0) {
                context->features_collected += 1ULL;
                context->points_collected += feature.point_count;
                context->collect_point_bytes += point_bytes;
            } else {
                context->features_skipped += 1ULL;
                context->points_skipped += feature.point_count;
            }
            rt_free(projected_points);
        }
        context->collect_refills += reader.refill_count;
        context->collect_bytes_read += reader.bytes_read;
        pack_reader_destroy(&reader);
    }
    result = 0;

cleanup:
    feature_hash_set_destroy(&seen_hashes);
    rt_free(selected_records);
    return result;
}

static int compute_city_boundary_bbox(RenderContext *context) {
    OsmNodeIndex node_index;
    OsmWayIndex way_index;
    OsmRelationIndex relation_index;
    OsmRelationIndexRecord relation_record;
    char error[OSM_INDEX_ERROR_CAPACITY];
    long long min_lon = 9223372036854775807LL;
    long long min_lat = 9223372036854775807LL;
    long long max_lon = -9223372036854775807LL;
    long long max_lat = -9223372036854775807LL;
    unsigned long long nodes_resolved = 0ULL;
    unsigned int member_index;
    int result = -1;

    rt_memset(&node_index, 0, sizeof(node_index));
    rt_memset(&way_index, 0, sizeof(way_index));
    rt_memset(&relation_index, 0, sizeof(relation_index));
    node_index.fd = -1;
    way_index.fd = -1;
    relation_index.fd = -1;
    if (!context->city_enabled || context->city_name == 0 || context->node_index_path == 0 || context->way_index_path == 0 || context->relation_index_path == 0) return 0;
    error[0] = '\0';
    if (osm_node_index_open(&node_index, context->node_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (osm_way_index_open(&way_index, context->way_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (osm_relation_index_open(&relation_index, context->relation_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (!osm_relation_index_find_city(&relation_index, context->city_name, &relation_record)) {
        result = 0;
        goto cleanup;
    }
    for (member_index = 0U; member_index < relation_record.member_count; ++member_index) {
        unsigned long long relation_member_index = relation_record.member_offset + (unsigned long long)member_index;
        OsmWayIndexRecord way_record;
        long long *refs = 0;
        unsigned int ref_index;
        int found;

        if (relation_member_index >= relation_index.member_count) goto cleanup;
        found = osm_way_index_find(&way_index, relation_index.members[relation_member_index], &way_record, error, sizeof(error));
        if (found < 0) goto cleanup;
        if (found == 0) continue;
        if (osm_way_index_read_refs(&way_index, &way_record, &refs, error, sizeof(error)) != 0) goto cleanup;
        for (ref_index = 0U; ref_index < way_record.ref_count; ++ref_index) {
            OsmNodeIndexRecord node_record;

            found = osm_node_index_find(&node_index, refs[ref_index], &node_record, error, sizeof(error));
            if (found < 0) {
                rt_free(refs);
                goto cleanup;
            }
            if (found == 0) continue;
            if (node_record.lon_nano < min_lon) min_lon = node_record.lon_nano;
            if (node_record.lon_nano > max_lon) max_lon = node_record.lon_nano;
            if (node_record.lat_nano < min_lat) min_lat = node_record.lat_nano;
            if (node_record.lat_nano > max_lat) max_lat = node_record.lat_nano;
            nodes_resolved += 1ULL;
        }
        rt_free(refs);
    }
    if (nodes_resolved != 0ULL && min_lon < max_lon && min_lat < max_lat) {
        long long lon_padding = (max_lon - min_lon) / 10LL;
        long long lat_padding = (max_lat - min_lat) / 10LL;
        if (lon_padding < 5000000LL) lon_padding = 5000000LL;
        if (lat_padding < 5000000LL) lat_padding = 5000000LL;
        context->min_lon_nano = min_lon - lon_padding;
        context->max_lon_nano = max_lon + lon_padding;
        context->min_lat_nano = min_lat - lat_padding;
        context->max_lat_nano = max_lat + lat_padding;
        result = 0;
    }

cleanup:
    osm_relation_index_close(&relation_index);
    osm_way_index_close(&way_index);
    osm_node_index_close(&node_index);
    return result;
}

static int collect_boundary_way(RenderContext *context, OsmNodeIndex *node_index, OsmWayIndex *way_index, long long way_id, char *error, size_t error_capacity) {
    OsmWayIndexRecord way_record;
    long long *refs = 0;
    int *projected_points;
    unsigned int ref_index;
    int found;

    found = osm_way_index_find(way_index, way_id, &way_record, error, error_capacity);
    if (found < 0) return -1;
    if (found == 0 || way_record.ref_count < 2U) return 0;
    if (osm_way_index_read_refs(way_index, &way_record, &refs, error, error_capacity) != 0) return -1;
    projected_points = (int *)rt_malloc(sizeof(int) * (size_t)way_record.ref_count * 2U);
    if (projected_points == 0) {
        rt_free(refs);
        return -1;
    }
    for (ref_index = 0U; ref_index < way_record.ref_count; ++ref_index) {
        OsmNodeIndexRecord node_record;
        long long pixel_x;
        long long pixel_y;

        found = osm_node_index_find(node_index, refs[ref_index], &node_record, error, error_capacity);
        if (found < 0) {
            rt_free(projected_points);
            rt_free(refs);
            return -1;
        }
        if (found == 0 || project_point(context, node_record.lon_nano, node_record.lat_nano, &pixel_x, &pixel_y) != 0) {
            rt_free(projected_points);
            rt_free(refs);
            return 0;
        }
        projected_points[ref_index * 2U + 0U] = clamp_to_int(pixel_x);
        projected_points[ref_index * 2U + 1U] = clamp_to_int(pixel_y);
    }
    if (append_render_feature(context, PACK_STYLE_BOUNDARY, 0U, way_record.ref_count, projected_points) != 0) {
        rt_free(projected_points);
        rt_free(refs);
        return -1;
    }
    context->boundary_feature_count += 1U;
    rt_free(projected_points);
    rt_free(refs);
    return 0;
}

static int collect_city_boundary(RenderContext *context) {
    OsmNodeIndex node_index;
    OsmWayIndex way_index;
    OsmRelationIndex relation_index;
    OsmRelationIndexRecord relation_record;
    char error[OSM_INDEX_ERROR_CAPACITY];
    unsigned int member_index;
    int result = -1;

    rt_memset(&node_index, 0, sizeof(node_index));
    rt_memset(&way_index, 0, sizeof(way_index));
    rt_memset(&relation_index, 0, sizeof(relation_index));
    node_index.fd = -1;
    way_index.fd = -1;
    relation_index.fd = -1;
    if (!context->city_enabled || context->city_name == 0 || context->node_index_path == 0 || context->way_index_path == 0 || context->relation_index_path == 0) return 0;
    error[0] = '\0';
    if (osm_node_index_open(&node_index, context->node_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (osm_way_index_open(&way_index, context->way_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (osm_relation_index_open(&relation_index, context->relation_index_path, error, sizeof(error)) != 0) goto cleanup;
    if (!osm_relation_index_find_city(&relation_index, context->city_name, &relation_record)) {
        result = 0;
        goto cleanup;
    }
    for (member_index = 0U; member_index < relation_record.member_count; ++member_index) {
        unsigned long long relation_member_index = relation_record.member_offset + (unsigned long long)member_index;
        if (relation_member_index >= relation_index.member_count) goto cleanup;
        if (collect_boundary_way(context, &node_index, &way_index, relation_index.members[relation_member_index], error, sizeof(error)) != 0) goto cleanup;
    }
    result = 0;

cleanup:
    osm_relation_index_close(&relation_index);
    osm_way_index_close(&way_index);
    osm_node_index_close(&node_index);
    return result;
}

#define CLIP_LEFT 1U
#define CLIP_RIGHT 2U
#define CLIP_TOP 4U
#define CLIP_BOTTOM 8U

static unsigned int clip_code(long long pixel_x, long long pixel_y, long long max_x, long long max_y) {
    unsigned int code = 0U;

    if (pixel_x < 0) code |= CLIP_LEFT;
    else if (pixel_x > max_x) code |= CLIP_RIGHT;
    if (pixel_y < 0) code |= CLIP_TOP;
    else if (pixel_y > max_y) code |= CLIP_BOTTOM;
    return code;
}

static int clip_segment(const RenderContext *context, long long *x0, long long *y0, long long *x1, long long *y1) {
    long long max_x = (long long)context->width - 1LL;
    long long max_y = (long long)context->height - 1LL;
    unsigned int iterations = 0U;

    for (;;) {
        unsigned int code0;
        unsigned int code1;
        unsigned int outside;
        long long clipped_x;
        long long clipped_y;

        if (iterations++ > 16U) return 0;
        code0 = clip_code(*x0, *y0, max_x, max_y);
        code1 = clip_code(*x1, *y1, max_x, max_y);
        if ((code0 | code1) == 0U) return 1;
        if ((code0 & code1) != 0U) return 0;
        outside = code0 != 0U ? code0 : code1;
        if ((outside & CLIP_TOP) != 0U) {
            if (*y1 == *y0) return 0;
            clipped_y = 0;
            clipped_x = *x0 + (*x1 - *x0) * (clipped_y - *y0) / (*y1 - *y0);
        } else if ((outside & CLIP_BOTTOM) != 0U) {
            if (*y1 == *y0) return 0;
            clipped_y = max_y;
            clipped_x = *x0 + (*x1 - *x0) * (clipped_y - *y0) / (*y1 - *y0);
        } else if ((outside & CLIP_RIGHT) != 0U) {
            if (*x1 == *x0) return 0;
            clipped_x = max_x;
            clipped_y = *y0 + (*y1 - *y0) * (clipped_x - *x0) / (*x1 - *x0);
        } else {
            if (*x1 == *x0) return 0;
            clipped_x = 0;
            clipped_y = *y0 + (*y1 - *y0) * (clipped_x - *x0) / (*x1 - *x0);
        }
        if (outside == code0) {
            *x0 = clipped_x;
            *y0 = clipped_y;
        } else {
            *x1 = clipped_x;
            *y1 = clipped_y;
        }
    }
}

static unsigned char blend_channel(unsigned char dst, unsigned char src, unsigned char alpha) {
    unsigned int inverse_alpha = 255U - (unsigned int)alpha;
    unsigned int value = (unsigned int)src * (unsigned int)alpha + (unsigned int)dst * inverse_alpha + 127U;

    return (unsigned char)(value / 255U);
}

static void put_pixel_rgb(RenderContext *context, int pixel_x, int pixel_y, unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
    unsigned char *pixel;

    if (pixel_x < 0 || pixel_y < 0 || pixel_x >= (int)context->width || pixel_y >= (int)context->height) return;
    pixel = context->pixels + ((size_t)pixel_y * (size_t)context->width + (size_t)pixel_x) * 3U;
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

static void put_brush(RenderContext *context, int pixel_x, int pixel_y, const RenderStyle *style) {
    int radius = style->width > 1U ? (int)((style->width + 1U) / 2U) : 0;
    int inner_radius = radius > 0 ? radius - 1 : 0;
    int inner_limit = inner_radius * inner_radius;
    int outer_limit = radius * radius;
    int offset_y;

    if (radius == 0) {
        put_pixel_rgb(context, pixel_x, pixel_y, style->red, style->green, style->blue, style->alpha);
        return;
    }
    for (offset_y = -radius; offset_y <= radius; ++offset_y) {
        int offset_x;
        for (offset_x = -radius; offset_x <= radius; ++offset_x) {
            int distance = offset_x * offset_x + offset_y * offset_y;
            if (distance <= inner_limit) {
                put_pixel_rgb(context, pixel_x + offset_x, pixel_y + offset_y, style->red, style->green, style->blue, style->alpha);
            } else if (distance <= outer_limit) {
                put_pixel_rgb(context, pixel_x + offset_x, pixel_y + offset_y, style->red, style->green, style->blue, (unsigned char)((unsigned int)style->alpha / 2U));
            }
        }
    }
}

static int abs_int(int value) {
    return value < 0 ? -value : value;
}

static void draw_line(RenderContext *context, int x0, int y0, int x1, int y1, const RenderStyle *style) {
    int delta_x = abs_int(x1 - x0);
    int step_x = x0 < x1 ? 1 : -1;
    int delta_y = -abs_int(y1 - y0);
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;

    for (;;) {
        int twice_error;

        put_brush(context, x0, y0, style);
        if (x0 == x1 && y0 == y1) break;
        twice_error = 2 * error;
        if (twice_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (twice_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static void sort_intersections(int *values, unsigned int count) {
    unsigned int value_index;

    for (value_index = 1U; value_index < count; ++value_index) {
        int value = values[value_index];
        unsigned int scan = value_index;
        while (scan > 0U && values[scan - 1U] > value) {
            values[scan] = values[scan - 1U];
            scan -= 1U;
        }
        values[scan] = value;
    }
}

static void draw_filled_polygon(RenderContext *context, const int *points, unsigned int point_count, const RenderStyle *style) {
    int min_y = (int)context->height;
    int max_y = -1;
    int *intersections;
    unsigned int point_index;
    int scan_y;

    if ((style->flags & STYLE_FLAG_FILL) == 0U || point_count < 4U) return;
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
                intersections[intersection_count] = x0 + (int)(numerator / (long long)(y1 - y0));
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
            for (fill_x = start_x; fill_x <= end_x; ++fill_x) put_pixel_rgb(context, fill_x, scan_y, style->fill_red, style->fill_green, style->fill_blue, style->fill_alpha);
        }
    }
    rt_free(intersections);
}

static void fade_pixel_outside_boundary(RenderContext *context, int x, int y) {
    unsigned char *pixel;

    if (x < 0 || y < 0 || x >= (int)context->width || y >= (int)context->height) return;
    pixel = context->pixels + ((size_t)y * (size_t)context->width + (size_t)x) * 3U;
    pixel[0] = blend_channel(pixel[0], 250U, 150U);
    pixel[1] = blend_channel(pixel[1], 250U, 150U);
    pixel[2] = blend_channel(pixel[2], 246U, 150U);
}

static void boundary_mask_mark(unsigned char *mask, unsigned int width, unsigned int height, int x, int y) {
    int radius = 3;
    int dy;

    for (dy = -radius; dy <= radius; ++dy) {
        int dx;
        int yy = y + dy;
        if (yy < 0 || yy >= (int)height) continue;
        for (dx = -radius; dx <= radius; ++dx) {
            int xx = x + dx;
            if (xx < 0 || xx >= (int)width) continue;
            if (dx * dx + dy * dy <= radius * radius) mask[(size_t)yy * (size_t)width + (size_t)xx] = 1U;
        }
    }
}

static void boundary_mask_line(unsigned char *mask, unsigned int width, unsigned int height, int x0, int y0, int x1, int y1) {
    int delta_x = abs_int(x1 - x0);
    int step_x = x0 < x1 ? 1 : -1;
    int delta_y = -abs_int(y1 - y0);
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;

    for (;;) {
        int twice_error;

        boundary_mask_mark(mask, width, height, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        twice_error = 2 * error;
        if (twice_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (twice_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static int boundary_mask_enqueue(unsigned char *mask, unsigned int *queue, unsigned int width, unsigned int height, unsigned int *tail_io, int x, int y) {
    unsigned int index;

    if (x < 0 || y < 0 || x >= (int)width || y >= (int)height) return 0;
    index = (unsigned int)((size_t)y * (size_t)width + (size_t)x);
    if (mask[index] != 0U) return 0;
    mask[index] = 2U;
    queue[*tail_io] = index;
    *tail_io += 1U;
    return 0;
}

static void fade_outside_boundary(RenderContext *context) {
    unsigned int pixel_count;
    unsigned char *mask;
    unsigned int *queue;
    BoundaryEndpoint *endpoints;
    unsigned int endpoint_count = 0U;
    unsigned int head = 0U;
    unsigned int tail = 0U;
    unsigned int feature_index;
    unsigned int index;
    unsigned int x;
    unsigned int y;

    if (!context->boundary_fade || context->boundary_fade_applied || context->boundary_feature_count < 2U) return;
    if ((size_t)context->width > ((size_t)-1) / (size_t)context->height) return;
    pixel_count = (unsigned int)((size_t)context->width * (size_t)context->height);
    if ((size_t)pixel_count != (size_t)context->width * (size_t)context->height) return;
    mask = (unsigned char *)rt_malloc((size_t)pixel_count);
    queue = (unsigned int *)rt_malloc(sizeof(unsigned int) * (size_t)pixel_count);
    endpoints = (BoundaryEndpoint *)rt_malloc(sizeof(BoundaryEndpoint) * (size_t)context->boundary_feature_count * 2U);
    if (mask == 0 || queue == 0 || endpoints == 0) {
        rt_free(mask);
        rt_free(queue);
        rt_free(endpoints);
        return;
    }
    rt_memset(mask, 0, (size_t)pixel_count);
    for (feature_index = 0U; feature_index < context->feature_count; ++feature_index) {
        const RenderFeature *feature = &context->features[feature_index];
        const int *points;
        unsigned int point_index;

        if (feature->style_id != PACK_STYLE_BOUNDARY || feature->point_count < 2U) continue;
        points = context->points + feature->point_offset;
        for (point_index = 0U; point_index + 1U < feature->point_count; ++point_index) {
            boundary_mask_line(mask, context->width, context->height,
                               points[point_index * 2U + 0U], points[point_index * 2U + 1U],
                               points[(point_index + 1U) * 2U + 0U], points[(point_index + 1U) * 2U + 1U]);
        }
        if (endpoint_count + 1U < context->boundary_feature_count * 2U) {
            endpoints[endpoint_count].x = points[0];
            endpoints[endpoint_count].y = points[1];
            endpoint_count += 1U;
            endpoints[endpoint_count].x = points[(feature->point_count - 1U) * 2U + 0U];
            endpoints[endpoint_count].y = points[(feature->point_count - 1U) * 2U + 1U];
            endpoint_count += 1U;
        }
    }
    for (index = 0U; index < endpoint_count; ++index) {
        unsigned int other;
        for (other = index + 1U; other < endpoint_count; ++other) {
            int dx = endpoints[index].x - endpoints[other].x;
            int dy = endpoints[index].y - endpoints[other].y;
            int distance2 = dx * dx + dy * dy;
            if (distance2 > 0 && distance2 <= 32 * 32) boundary_mask_line(mask, context->width, context->height, endpoints[index].x, endpoints[index].y, endpoints[other].x, endpoints[other].y);
        }
    }
    for (x = 0U; x < context->width; ++x) {
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, (int)x, 0);
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, (int)x, (int)context->height - 1);
    }
    for (y = 1U; y + 1U < context->height; ++y) {
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, 0, (int)y);
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, (int)context->width - 1, (int)y);
    }
    while (head < tail) {
        unsigned int pixel = queue[head++];
        int px = (int)(pixel % context->width);
        int py = (int)(pixel / context->width);

        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, px - 1, py);
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, px + 1, py);
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, px, py - 1);
        (void)boundary_mask_enqueue(mask, queue, context->width, context->height, &tail, px, py + 1);
    }
    for (index = 0U; index < pixel_count; ++index) {
        if (mask[index] == 2U) fade_pixel_outside_boundary(context, (int)(index % context->width), (int)(index / context->width));
    }
    context->boundary_fade_applied = 1;
    rt_free(mask);
    rt_free(queue);
    rt_free(endpoints);
}

static int draw_feature(RenderContext *context, unsigned int step, const PackFeatureHeader *feature, const long long *lonlat_points) {
    const RenderStyle *base_style;
    RenderStyle casing_style;
    unsigned int point_index;
    int *projected = 0;

    if (feature->style_id >= PACK_STYLE_COUNT) return 0;
    base_style = &context->styles[feature->style_id];
    if (step == RENDER_STEP_AREA) {
        if ((feature->flags & OSMRPACK_FEATURE_FLAG_AREA) == 0U) return 0;
        projected = (int *)rt_malloc(sizeof(int) * (size_t)feature->point_count * 2U);
        if (projected == 0) return -1;
        for (point_index = 0U; point_index < feature->point_count; ++point_index) {
            long long pixel_x;
            long long pixel_y;
            if (project_point(context, lonlat_points[point_index * 2U + 0U], lonlat_points[point_index * 2U + 1U], &pixel_x, &pixel_y) != 0) {
                rt_free(projected);
                return -1;
            }
            projected[point_index * 2U + 0U] = (int)pixel_x;
            projected[point_index * 2U + 1U] = (int)pixel_y;
        }
        draw_filled_polygon(context, projected, feature->point_count, base_style);
        context->polygons_drawn += 1ULL;
        rt_free(projected);
        return 1;
    }
    if (step == RENDER_STEP_CASING) {
        if ((base_style->flags & STYLE_FLAG_CASING) == 0U || base_style->casing_width <= base_style->width) return 0;
        rt_memset(&casing_style, 0, sizeof(casing_style));
        casing_style.red = base_style->casing_red;
        casing_style.green = base_style->casing_green;
        casing_style.blue = base_style->casing_blue;
        casing_style.alpha = base_style->casing_alpha;
        casing_style.width = base_style->casing_width;
        base_style = &casing_style;
    } else if ((base_style->flags & STYLE_FLAG_LINE) == 0U) {
        return 0;
    }
    for (point_index = 0U; point_index + 1U < feature->point_count; ++point_index) {
        long long x0;
        long long y0;
        long long x1;
        long long y1;
        if (project_point(context, lonlat_points[point_index * 2U + 0U], lonlat_points[point_index * 2U + 1U], &x0, &y0) != 0 ||
            project_point(context, lonlat_points[(point_index + 1U) * 2U + 0U], lonlat_points[(point_index + 1U) * 2U + 1U], &x1, &y1) != 0) {
            return -1;
        }
        if (clip_segment(context, &x0, &y0, &x1, &y1)) {
            draw_line(context, (int)x0, (int)y0, (int)x1, (int)y1, base_style);
            context->segments_drawn += 1ULL;
        }
    }
    return 1;
}

static int draw_collected_feature(RenderContext *context, unsigned int step, const RenderFeature *feature) {
    const RenderStyle *base_style;
    RenderStyle casing_style;
    const int *projected_points;
    unsigned int point_index;

    if (feature->style_id >= PACK_STYLE_COUNT) return 0;
    base_style = &context->styles[feature->style_id];
    projected_points = context->points + feature->point_offset;
    if (step == RENDER_STEP_AREA) {
        if ((feature->flags & OSMRPACK_FEATURE_FLAG_AREA) == 0U) return 0;
        draw_filled_polygon(context, projected_points, feature->point_count, base_style);
        context->polygons_drawn += 1ULL;
        return 1;
    }
    if (step == RENDER_STEP_CASING) {
        if ((base_style->flags & STYLE_FLAG_CASING) == 0U || base_style->casing_width <= base_style->width) return 0;
        rt_memset(&casing_style, 0, sizeof(casing_style));
        casing_style.red = base_style->casing_red;
        casing_style.green = base_style->casing_green;
        casing_style.blue = base_style->casing_blue;
        casing_style.alpha = base_style->casing_alpha;
        casing_style.width = base_style->casing_width;
        base_style = &casing_style;
    } else if ((base_style->flags & STYLE_FLAG_LINE) == 0U) {
        return 0;
    }
    for (point_index = 0U; point_index + 1U < feature->point_count; ++point_index) {
        long long x0 = projected_points[point_index * 2U + 0U];
        long long y0 = projected_points[point_index * 2U + 1U];
        long long x1 = projected_points[(point_index + 1U) * 2U + 0U];
        long long y1 = projected_points[(point_index + 1U) * 2U + 1U];

        if (clip_segment(context, &x0, &y0, &x1, &y1)) {
            draw_line(context, (int)x0, (int)y0, (int)x1, (int)y1, base_style);
            context->segments_drawn += 1ULL;
        }
    }
    return 1;
}

static void draw_collected_layers(RenderContext *context) {
    unsigned int layer_index;

    for (layer_index = 0U; layer_index < (unsigned int)(sizeof(render_layers) / sizeof(render_layers[0])); ++layer_index) {
        unsigned int feature_index;
        if (render_layers[layer_index].style_id == PACK_STYLE_BOUNDARY && !context->boundary_fade_applied) fade_outside_boundary(context);
        for (feature_index = 0U; feature_index < context->feature_count; ++feature_index) {
            const RenderFeature *feature = &context->features[feature_index];
            if (feature->style_id == render_layers[layer_index].style_id) {
                if (draw_collected_feature(context, render_layers[layer_index].step, feature) > 0) context->features_drawn += 1ULL;
            }
        }
    }
}

static int render_layer(int fd, const OsmrPackHeader *pack_header, RenderContext *context, unsigned int step, unsigned int style_id) {
    unsigned char count_data[8];
    unsigned long long feature_count;
    unsigned long long feature_index;

    if (platform_seek(fd, (long long)pack_header->feature_data_offset, PLATFORM_SEEK_SET) < 0) return -1;
    if (read_exact(fd, count_data, sizeof(count_data)) != 0) return -1;
    feature_count = read_u64_le(count_data);
    for (feature_index = 0ULL; feature_index < feature_count; ++feature_index) {
        PackFeatureHeader feature;
        long long *points;
        int drawn;

        if (read_feature_header(fd, &feature) != 0) return -1;
        context->features_seen += 1ULL;
        if (feature.point_count < 2U || feature.point_count > 10000000U) return -1;
        if (feature.style_id != style_id || !feature_intersects_render_bbox(context, &feature)) {
            if (skip_points(fd, feature.point_count) != 0) return -1;
            continue;
        }
        points = (long long *)rt_malloc(sizeof(long long) * (size_t)feature.point_count * 2U);
        if (points == 0) return -1;
        if (read_points(fd, points, feature.point_count) != 0) {
            rt_free(points);
            return -1;
        }
        drawn = draw_feature(context, step, &feature, points);
        rt_free(points);
        if (drawn < 0) return -1;
        if (drawn > 0) context->features_drawn += 1ULL;
    }
    return 0;
}

static int fill_background(RenderContext *context) {
    size_t pixel_count;
    size_t pixel_index;

    if ((size_t)context->width > ((size_t)-1) / (size_t)context->height) return -1;
    pixel_count = (size_t)context->width * (size_t)context->height;
    context->pixels = (unsigned char *)rt_malloc(pixel_count * 3U);
    if (context->pixels == 0) return -1;
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        context->pixels[pixel_index * 3U + 0U] = 242U;
        context->pixels[pixel_index * 3U + 1U] = 239U;
        context->pixels[pixel_index * 3U + 2U] = 232U;
    }
    return 0;
}

static int write_png_chunk(int fd, const char type[4], const unsigned char *data, size_t data_size) {
    unsigned char header[8];
    unsigned char crc_bytes[4];
    unsigned int crc;

    if (data_size > 0xffffffffU) return -1;
    write_u32_be(header, (unsigned int)data_size);
    header[4] = (unsigned char)type[0];
    header[5] = (unsigned char)type[1];
    header[6] = (unsigned char)type[2];
    header[7] = (unsigned char)type[3];
    if (rt_write_all(fd, header, sizeof(header)) != 0) return -1;
    if (data_size != 0U && rt_write_all(fd, data, data_size) != 0) return -1;
    crc = compression_crc32_update(0xffffffffU, (const unsigned char *)type, 4U);
    if (data_size != 0U) crc = compression_crc32_update(crc, data, data_size);
    write_u32_be(crc_bytes, compression_crc32_finish(crc));
    return rt_write_all(fd, crc_bytes, sizeof(crc_bytes));
}

static unsigned int png_filter_abs(unsigned int value) {
    return value < 128U ? value : 256U - value;
}

static unsigned int png_paeth(unsigned int left, unsigned int up, unsigned int up_left) {
    int p = (int)left + (int)up - (int)up_left;
    int pa = abs_int(p - (int)left);
    int pb = abs_int(p - (int)up);
    int pc = abs_int(p - (int)up_left);

    if (pa <= pb && pa <= pc) return left;
    if (pb <= pc) return up;
    return up_left;
}

static unsigned long long png_filter_score(const unsigned char *row, const unsigned char *previous, size_t row_bytes, unsigned int bpp, unsigned int filter, unsigned char *out) {
    unsigned long long score = 0ULL;
    size_t index;

    for (index = 0U; index < row_bytes; ++index) {
        unsigned int raw = row[index];
        unsigned int left = index >= (size_t)bpp ? row[index - (size_t)bpp] : 0U;
        unsigned int up = previous != 0 ? previous[index] : 0U;
        unsigned int up_left = previous != 0 && index >= (size_t)bpp ? previous[index - (size_t)bpp] : 0U;
        unsigned int filtered;

        if (filter == 1U) filtered = (raw - left) & 0xffU;
        else if (filter == 2U) filtered = (raw - up) & 0xffU;
        else if (filter == 3U) filtered = (raw - ((left + up) >> 1U)) & 0xffU;
        else if (filter == 4U) filtered = (raw - png_paeth(left, up, up_left)) & 0xffU;
        else filtered = raw;
        score += (unsigned long long)png_filter_abs(filtered);
        if (out != 0) out[index] = (unsigned char)filtered;
    }
    return score;
}

static void png_write_filtered_rows(const RenderContext *context, unsigned char *raw, size_t row_size) {
    size_t row_bytes = (size_t)context->width * 3U;
    unsigned int row_index;

    for (row_index = 0U; row_index < context->height; ++row_index) {
        const unsigned char *row = context->pixels + (size_t)row_index * row_bytes;
        const unsigned char *previous = row_index == 0U ? 0 : context->pixels + (size_t)(row_index - 1U) * row_bytes;
        unsigned char *out = raw + (size_t)row_index * row_size;
        unsigned int filter;
        unsigned int best_filter = 0U;
        unsigned long long best_score = png_filter_score(row, previous, row_bytes, 3U, 0U, 0);

        for (filter = 1U; filter <= 4U; ++filter) {
            unsigned long long score = png_filter_score(row, previous, row_bytes, 3U, filter, 0);
            if (score < best_score) {
                best_score = score;
                best_filter = filter;
            }
        }
        out[0] = (unsigned char)best_filter;
        (void)png_filter_score(row, previous, row_bytes, 3U, best_filter, out + 1U);
    }
}

static void png_palette_init(PngPalette *palette) {
    unsigned int index;

    palette->count = 0U;
    for (index = 0U; index < 1024U; ++index) palette->table[index] = -1;
}

static unsigned int png_palette_hash(unsigned int color) {
    return (color * 2654435761U) & 1023U;
}

static int png_palette_find(const PngPalette *palette, unsigned int color) {
    unsigned int slot = png_palette_hash(color);
    unsigned int probe;

    for (probe = 0U; probe < 1024U; ++probe) {
        short index = palette->table[(slot + probe) & 1023U];
        if (index < 0) return -1;
        if (palette->colors[(unsigned int)index] == color) return (int)index;
    }
    return -1;
}

static int png_palette_insert(PngPalette *palette, unsigned int color) {
    unsigned int slot;
    unsigned int probe;
    int existing = png_palette_find(palette, color);

    if (existing >= 0) return existing;
    if (palette->count >= 256U) return -1;
    slot = png_palette_hash(color);
    for (probe = 0U; probe < 1024U; ++probe) {
        unsigned int table_index = (slot + probe) & 1023U;
        if (palette->table[table_index] < 0) {
            palette->colors[palette->count] = color;
            palette->table[table_index] = (short)palette->count;
            palette->count += 1U;
            return (int)(palette->count - 1U);
        }
    }
    return -1;
}

static int png_build_exact_palette(const RenderContext *context, PngPalette *palette, unsigned char *indexed_pixels) {
    size_t pixel_count = (size_t)context->width * (size_t)context->height;
    size_t pixel_index;

    png_palette_init(palette);
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        const unsigned char *pixel = context->pixels + pixel_index * 3U;
        unsigned int color = ((unsigned int)pixel[0] << 16U) | ((unsigned int)pixel[1] << 8U) | (unsigned int)pixel[2];
        int palette_index = png_palette_insert(palette, color);
        if (palette_index < 0) return -1;
        indexed_pixels[pixel_index] = (unsigned char)palette_index;
    }
    return 0;
}

static unsigned int png_quant_bucket_key(unsigned int red, unsigned int green, unsigned int blue) {
    return ((red >> 3U) << 10U) | ((green >> 3U) << 5U) | (blue >> 3U);
}

static unsigned int png_quant_bucket_center(unsigned int key) {
    unsigned int red = (key >> 10U) & 31U;
    unsigned int green = (key >> 5U) & 31U;
    unsigned int blue = key & 31U;

    return (((red * 255U + 15U) / 31U) << 16U) | (((green * 255U + 15U) / 31U) << 8U) | ((blue * 255U + 15U) / 31U);
}

static unsigned int png_bucket_average_color(const PngColorBucket *bucket, unsigned int fallback_key) {
    if (bucket->count == 0U) return png_quant_bucket_center(fallback_key);
    return (unsigned int)(((bucket->red_sum / bucket->count) & 0xffULL) << 16U) |
           (unsigned int)(((bucket->green_sum / bucket->count) & 0xffULL) << 8U) |
           (unsigned int)((bucket->blue_sum / bucket->count) & 0xffULL);
}

static unsigned int png_color_distance2(unsigned int left, unsigned int right) {
    int red = (int)((left >> 16U) & 0xffU) - (int)((right >> 16U) & 0xffU);
    int green = (int)((left >> 8U) & 0xffU) - (int)((right >> 8U) & 0xffU);
    int blue = (int)(left & 0xffU) - (int)(right & 0xffU);

    return (unsigned int)(red * red + green * green + blue * blue);
}

static int png_build_quantized_palette(const RenderContext *context, PngPalette *palette, unsigned char *indexed_pixels) {
    PngColorBucket *buckets;
    unsigned char *bucket_to_palette;
    unsigned int selected[256];
    unsigned int selected_count = 0U;
    size_t pixel_count = (size_t)context->width * (size_t)context->height;
    size_t pixel_index;
    unsigned int bucket_index;
    int result = -1;

    buckets = (PngColorBucket *)rt_malloc(sizeof(PngColorBucket) * 32768U);
    bucket_to_palette = (unsigned char *)rt_malloc(32768U);
    if (buckets == 0 || bucket_to_palette == 0) {
        rt_free(buckets);
        rt_free(bucket_to_palette);
        return -1;
    }
    rt_memset(buckets, 0, sizeof(PngColorBucket) * 32768U);
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        const unsigned char *pixel = context->pixels + pixel_index * 3U;
        unsigned int key = png_quant_bucket_key(pixel[0], pixel[1], pixel[2]);

        buckets[key].count += 1U;
        buckets[key].red_sum += pixel[0];
        buckets[key].green_sum += pixel[1];
        buckets[key].blue_sum += pixel[2];
    }
    for (bucket_index = 0U; bucket_index < 32768U; ++bucket_index) {
        unsigned int selected_index;
        unsigned int min_index = 0U;
        unsigned int min_count = 0xffffffffU;

        if (buckets[bucket_index].count == 0U) continue;
        if (selected_count < 256U) {
            selected[selected_count++] = bucket_index;
            continue;
        }
        for (selected_index = 0U; selected_index < selected_count; ++selected_index) {
            unsigned int count = buckets[selected[selected_index]].count;
            if (count < min_count) {
                min_count = count;
                min_index = selected_index;
            }
        }
        if (buckets[bucket_index].count > min_count) selected[min_index] = bucket_index;
    }
    if (selected_count == 0U) goto cleanup;
    png_palette_init(palette);
    for (bucket_index = 0U; bucket_index < selected_count; ++bucket_index) {
        unsigned int color = png_bucket_average_color(&buckets[selected[bucket_index]], selected[bucket_index]);
        if (png_palette_insert(palette, color) < 0) goto cleanup;
    }
    if (palette->count == 0U) goto cleanup;
    for (bucket_index = 0U; bucket_index < 32768U; ++bucket_index) {
        unsigned int color = png_bucket_average_color(&buckets[bucket_index], bucket_index);
        unsigned int best_distance = 0xffffffffU;
        unsigned int palette_index;
        unsigned int best_index = 0U;

        for (palette_index = 0U; palette_index < palette->count; ++palette_index) {
            unsigned int distance = png_color_distance2(color, palette->colors[palette_index]);
            if (distance < best_distance) {
                best_distance = distance;
                best_index = palette_index;
            }
        }
        bucket_to_palette[bucket_index] = (unsigned char)best_index;
    }
    for (pixel_index = 0U; pixel_index < pixel_count; ++pixel_index) {
        const unsigned char *pixel = context->pixels + pixel_index * 3U;
        indexed_pixels[pixel_index] = bucket_to_palette[png_quant_bucket_key(pixel[0], pixel[1], pixel[2])];
    }
    result = 0;

cleanup:
    rt_free(bucket_to_palette);
    rt_free(buckets);
    return result;
}

static void png_write_filtered_index_rows(const unsigned char *indexed_pixels, unsigned int width, unsigned int height, unsigned char *raw, size_t row_size) {
    unsigned int row_index;

    for (row_index = 0U; row_index < height; ++row_index) {
        const unsigned char *row = indexed_pixels + (size_t)row_index * (size_t)width;
        const unsigned char *previous = row_index == 0U ? 0 : indexed_pixels + (size_t)(row_index - 1U) * (size_t)width;
        unsigned char *out = raw + (size_t)row_index * row_size;
        unsigned int filter;
        unsigned int best_filter = 0U;
        unsigned long long best_score = png_filter_score(row, previous, (size_t)width, 1U, 0U, 0);

        for (filter = 1U; filter <= 4U; ++filter) {
            unsigned long long score = png_filter_score(row, previous, (size_t)width, 1U, filter, 0);
            if (score < best_score) {
                best_score = score;
                best_filter = filter;
            }
        }
        out[0] = (unsigned char)best_filter;
        (void)png_filter_score(row, previous, (size_t)width, 1U, best_filter, out + 1U);
    }
}

static int write_png(const char *path, const RenderContext *context) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    unsigned char ihdr[13];
    unsigned char plte[256U * 3U];
    unsigned char *raw;
    unsigned char *compressed;
    unsigned char *indexed_pixels = 0;
    PngPalette palette;
    size_t row_size;
    size_t raw_size;
    size_t compressed_capacity;
    size_t compressed_size = 0U;
    int use_palette = 0;
    int fd;
    int result = -1;

    if ((size_t)context->width > ((size_t)-1) / (size_t)context->height) return -1;
    if (context->png_palette) indexed_pixels = (unsigned char *)rt_malloc((size_t)context->width * (size_t)context->height);
    if (indexed_pixels != 0 && (png_build_exact_palette(context, &palette, indexed_pixels) == 0 || png_build_quantized_palette(context, &palette, indexed_pixels) == 0)) {
        unsigned int palette_index;

        use_palette = 1;
        for (palette_index = 0U; palette_index < palette.count; ++palette_index) {
            unsigned int color = palette.colors[palette_index];
            plte[palette_index * 3U + 0U] = (unsigned char)((color >> 16U) & 0xffU);
            plte[palette_index * 3U + 1U] = (unsigned char)((color >> 8U) & 0xffU);
            plte[palette_index * 3U + 2U] = (unsigned char)(color & 0xffU);
        }
    }
    row_size = use_palette ? (size_t)context->width + 1U : (size_t)context->width * 3U + 1U;
    raw_size = row_size * (size_t)context->height;
    raw = (unsigned char *)rt_malloc(raw_size);
    if (raw == 0) {
        rt_free(indexed_pixels);
        return -1;
    }
    if (use_palette) png_write_filtered_index_rows(indexed_pixels, context->width, context->height, raw, row_size);
    else png_write_filtered_rows(context, raw, row_size);
    compressed_capacity = compression_zlib_fixed_rle_bound(raw_size);
    compressed = (unsigned char *)rt_malloc(compressed_capacity);
    if (compressed == 0 || compression_zlib_fixed_rle(raw, raw_size, compressed, compressed_capacity, &compressed_size) != 0) {
        rt_free(compressed);
        rt_free(raw);
        rt_free(indexed_pixels);
        return -1;
    }
    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        rt_free(compressed);
        rt_free(raw);
        rt_free(indexed_pixels);
        return -1;
    }
    write_u32_be(ihdr + 0U, context->width);
    write_u32_be(ihdr + 4U, context->height);
    ihdr[8] = 8U;
    ihdr[9] = use_palette ? 3U : 2U;
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = 0U;
    if (rt_write_all(fd, signature, sizeof(signature)) == 0 &&
        write_png_chunk(fd, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
        (!use_palette || write_png_chunk(fd, "PLTE", plte, (size_t)palette.count * 3U) == 0) &&
        write_png_chunk(fd, "IDAT", compressed, compressed_size) == 0 &&
        write_png_chunk(fd, "IEND", 0, 0U) == 0) {
        result = 0;
    }
    if (platform_close(fd) != 0) result = -1;
    rt_free(compressed);
    rt_free(raw);
    rt_free(indexed_pixels);
    return result;
}

static void write_render_summary(const char *out_path, const RenderContext *context, unsigned long long tile_count, unsigned long long selected_tile_count, unsigned long long tile_features,
                                 unsigned long long elapsed_ms, int profile_enabled, unsigned long long total_elapsed_ms, unsigned long long bbox_elapsed_ms,
                                 unsigned long long header_elapsed_ms, unsigned long long open_tile_elapsed_ms, unsigned long long fill_elapsed_ms,
                                 unsigned long long collect_elapsed_ms, unsigned long long boundary_elapsed_ms, unsigned long long draw_elapsed_ms,
                                 unsigned long long png_elapsed_ms) {
    rt_write_cstr(1, "pack_valid: yes\n");
    rt_write_cstr(1, "tile_count: ");
    rt_write_uint(1, tile_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "selected_tiles: ");
    rt_write_uint(1, selected_tile_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_features: ");
    rt_write_uint(1, tile_features);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_seen: ");
    rt_write_uint(1, context->features_seen);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_skipped: ");
    rt_write_uint(1, context->features_skipped);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_collected: ");
    rt_write_uint(1, context->features_collected);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "points_skipped: ");
    rt_write_uint(1, context->points_skipped);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "points_collected: ");
    rt_write_uint(1, context->points_collected);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_skip_count: ");
    rt_write_uint(1, context->collect_skip_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_header_bytes: ");
    rt_write_uint(1, context->collect_header_bytes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_skipped_bytes: ");
    rt_write_uint(1, context->collect_skipped_bytes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_point_bytes: ");
    rt_write_uint(1, context->collect_point_bytes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_refills: ");
    rt_write_uint(1, context->collect_refills);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "collect_bytes_read: ");
    rt_write_uint(1, context->collect_bytes_read);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_drawn: ");
    rt_write_uint(1, context->features_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "segments_drawn: ");
    rt_write_uint(1, context->segments_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "polygons_drawn: ");
    rt_write_uint(1, context->polygons_drawn);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "boundary_features: ");
    rt_write_uint(1, context->boundary_feature_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "boundary_fade: ");
    rt_write_cstr(1, context->boundary_fade_applied ? "yes" : "no");
    rt_write_char(1, '\n');
    rt_write_cstr(1, "render_elapsed_ms: ");
    rt_write_uint(1, elapsed_ms);
    rt_write_char(1, '\n');
    if (profile_enabled) {
        write_profile_ms("profile_total_ms: ", total_elapsed_ms);
        write_profile_ms("profile_city_bbox_ms: ", bbox_elapsed_ms);
        write_profile_ms("profile_header_ms: ", header_elapsed_ms);
        write_profile_ms("profile_open_tile_ms: ", open_tile_elapsed_ms);
        write_profile_ms("profile_fill_background_ms: ", fill_elapsed_ms);
        write_profile_ms("profile_collect_features_ms: ", collect_elapsed_ms);
        write_profile_ms("profile_collect_boundary_ms: ", boundary_elapsed_ms);
        write_profile_ms("profile_draw_layers_ms: ", draw_elapsed_ms);
        write_profile_ms("profile_write_png_ms: ", png_elapsed_ms);
    }
    rt_write_cstr(1, "output: ");
    rt_write_cstr(1, out_path);
    rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmrender-rpack";
    const char *pack_path;
    const char *out_path;
    char default_node_index_path[256];
    char default_way_index_path[256];
    char default_relation_index_path[256];
    OsmrPackHeader pack_header;
    OsmrPackTileRecord tile_record;
    OsmrPackV2Header v2_header;
    RenderContext context;
    char error[OSMRPACK_ERROR_CAPACITY];
    int bbox_set = 0;
    int profile_enabled = 0;
    int width_explicit = 0;
    int height_explicit = 0;
    int fd;
    int argi = 3;
    int v2_header_result;
    unsigned long long phase_start_ns;
    unsigned long long total_start_ns;
    unsigned long long bbox_elapsed_ms = 0ULL;
    unsigned long long header_elapsed_ms = 0ULL;
    unsigned long long open_tile_elapsed_ms = 0ULL;
    unsigned long long fill_elapsed_ms = 0ULL;
    unsigned long long collect_elapsed_ms = 0ULL;
    unsigned long long boundary_elapsed_ms = 0ULL;
    unsigned long long draw_elapsed_ms = 0ULL;
    unsigned long long png_elapsed_ms = 0ULL;
    unsigned long long elapsed_ms;
    unsigned long long total_elapsed_ms;

    if (argc < 5) {
        write_usage(program);
        return 1;
    }
    total_start_ns = platform_get_monotonic_time_ns();
    pack_path = argv[1];
    out_path = argv[2];
    rt_memset(&context, 0, sizeof(context));
    context.width = 1600U;
    context.height = 1200U;
    context.boundary_fade = 1;
    context.png_palette = 1;
    default_node_index_path[0] = '\0';
    default_way_index_path[0] = '\0';
    default_relation_index_path[0] = '\0';
    styles_init(&context);
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--bbox") == 0) {
            argi += 1;
            if (argi >= argc || parse_bbox_arg(argv[argi], &context) != 0) {
                write_usage(program);
                return 1;
            }
            bbox_set = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--city") == 0) {
            argi += 1;
            if (argi >= argc) {
                write_usage(program);
                return 1;
            }
            if (set_city_bbox(argv[argi], &context) == 0) bbox_set = 1;
            context.city_name = argv[argi];
            context.city_enabled = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--width") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.width) != 0) {
                write_usage(program);
                return 1;
            }
            width_explicit = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--height") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.height) != 0) {
                write_usage(program);
                return 1;
            }
            height_explicit = 1;
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
        } else if (rt_strcmp(argv[argi], "--relation-index") == 0) {
            argi += 1;
            if (argi >= argc) {
                write_usage(program);
                return 1;
            }
            context.relation_index_path = argv[argi];
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-boundary-fade") == 0) {
            context.boundary_fade = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--png-rgb") == 0) {
            context.png_palette = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--profile") == 0) {
            profile_enabled = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            write_usage(program);
            return 0;
        } else {
            write_usage(program);
            return 1;
        }
    }
    if (!bbox_set && !context.city_enabled) {
        rt_write_cstr(2, "osmrender-rpack: missing --bbox or --city\n");
        return 1;
    }
    phase_start_ns = platform_get_monotonic_time_ns();
    if (context.city_enabled) {
        if (context.node_index_path == 0 && infer_index_path_from_pack(pack_path, ".osmnidx", default_node_index_path, sizeof(default_node_index_path)) == 0 && path_exists(default_node_index_path)) context.node_index_path = default_node_index_path;
        if (context.way_index_path == 0 && infer_index_path_from_pack(pack_path, ".osmwidx", default_way_index_path, sizeof(default_way_index_path)) == 0 && path_exists(default_way_index_path)) context.way_index_path = default_way_index_path;
        if (context.relation_index_path == 0 && infer_index_path_from_pack(pack_path, ".osmridx", default_relation_index_path, sizeof(default_relation_index_path)) == 0 && path_exists(default_relation_index_path)) context.relation_index_path = default_relation_index_path;
        if (compute_city_boundary_bbox(&context) != 0) {
            rt_write_cstr(2, "osmrender-rpack: failed while computing city boundary bbox\n");
            return 1;
        }
        if (render_bbox_is_valid(&context)) bbox_set = 1;
    }
    bbox_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    error[0] = '\0';
    phase_start_ns = platform_get_monotonic_time_ns();
    v2_header_result = read_v2_header_path(pack_path, &v2_header);
    if (v2_header_result < 0) {
        rt_write_cstr(2, "osmrender-rpack: could not read pack header\n");
        return 1;
    }
    if (v2_header_result > 0) {
        unsigned long long selected_tile_count = 0ULL;
        unsigned long long selected_tile_features = 0ULL;

        header_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        phase_start_ns = platform_get_monotonic_time_ns();
        fd = platform_open_read(pack_path);
        if (fd < 0) {
            rt_write_cstr(2, "osmrender-rpack: could not open pack\n");
            return 1;
        }
        if (context.city_enabled) {
            int place_bbox_result = apply_v2_place_bbox(fd, &v2_header, &context);
            if (place_bbox_result < 0) {
                (void)platform_close(fd);
                rt_write_cstr(2, "osmrender-rpack: failed while resolving v2 place bbox\n");
                return 1;
            }
            if (place_bbox_result > 0 && render_bbox_is_valid(&context)) bbox_set = 1;
        }
        if (!bbox_set) {
            (void)platform_close(fd);
            rt_write_cstr(2, "osmrender-rpack: could not resolve city bbox\n");
            return 1;
        }
        choose_missing_dimension(&context, width_explicit, height_explicit);
        open_tile_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        phase_start_ns = platform_get_monotonic_time_ns();
        if (fill_background(&context) != 0) {
            (void)platform_close(fd);
            rt_write_cstr(2, "osmrender-rpack: could not allocate framebuffer\n");
            return 1;
        }
        fill_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        phase_start_ns = platform_get_monotonic_time_ns();
        if (collect_visible_features_v2(fd, &v2_header, &context, &selected_tile_count, &selected_tile_features) != 0) {
            rt_free(context.pixels);
            (void)platform_close(fd);
            rt_write_cstr(2, "osmrender-rpack: failed while collecting visible v2 pack features\n");
            return 1;
        }
        collect_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        phase_start_ns = platform_get_monotonic_time_ns();
        if (collect_v2_place_boundary(&context, fd) != 0 || (context.boundary_feature_count == 0U && collect_city_boundary(&context) != 0)) {
            rt_free(context.features);
            rt_free(context.points);
            rt_free(context.pixels);
            (void)platform_close(fd);
            rt_write_cstr(2, "osmrender-rpack: failed while collecting city boundary\n");
            return 1;
        }
        boundary_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        phase_start_ns = platform_get_monotonic_time_ns();
        draw_collected_layers(&context);
        draw_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        elapsed_ms = collect_elapsed_ms + boundary_elapsed_ms + draw_elapsed_ms;
        (void)platform_close(fd);
        phase_start_ns = platform_get_monotonic_time_ns();
        if (write_png(out_path, &context) != 0) {
            rt_free(context.features);
            rt_free(context.points);
            rt_free(context.pixels);
            rt_write_cstr(2, "osmrender-rpack: could not write PNG\n");
            return 1;
        }
        png_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
        total_elapsed_ms = (platform_get_monotonic_time_ns() - total_start_ns) / 1000000ULL;
        rt_free(context.features);
        rt_free(context.points);
        rt_free(context.pixels);
        write_render_summary(out_path, &context, v2_header.tile_count, selected_tile_count, selected_tile_features, elapsed_ms, profile_enabled, total_elapsed_ms, bbox_elapsed_ms,
                             header_elapsed_ms, open_tile_elapsed_ms, fill_elapsed_ms, collect_elapsed_ms, boundary_elapsed_ms, draw_elapsed_ms, png_elapsed_ms);
        return 0;
    }
    if (osmrpack_read_header(pack_path, &pack_header, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmrender-rpack: ");
        rt_write_cstr(2, error[0] == '\0' ? "could not read pack" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    if (pack_header.tile_count == 0ULL || pack_header.feature_data_size == 0ULL) {
        rt_write_cstr(2, "osmrender-rpack: pack contains no render tiles yet\n");
        return 2;
    }
    if (!bbox_set) {
        rt_write_cstr(2, "osmrender-rpack: could not resolve city bbox\n");
        return 1;
    }
    choose_missing_dimension(&context, width_explicit, height_explicit);
    header_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    phase_start_ns = platform_get_monotonic_time_ns();
    fd = platform_open_read(pack_path);
    if (fd < 0) {
        rt_write_cstr(2, "osmrender-rpack: could not open pack\n");
        return 1;
    }
    if (platform_seek(fd, (long long)pack_header.tile_directory_offset, PLATFORM_SEEK_SET) < 0 || osmrpack_read_tile_record_fd(fd, &tile_record) != 0) {
        (void)platform_close(fd);
        rt_write_cstr(2, "osmrender-rpack: could not read tile directory\n");
        return 1;
    }
    open_tile_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    phase_start_ns = platform_get_monotonic_time_ns();
    if (fill_background(&context) != 0) {
        (void)platform_close(fd);
        rt_write_cstr(2, "osmrender-rpack: could not allocate framebuffer\n");
        return 1;
    }
    fill_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    phase_start_ns = platform_get_monotonic_time_ns();
    if (collect_visible_features(fd, &pack_header, &context) != 0) {
        rt_free(context.pixels);
        (void)platform_close(fd);
        rt_write_cstr(2, "osmrender-rpack: failed while collecting visible pack features\n");
        return 1;
    }
    collect_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    phase_start_ns = platform_get_monotonic_time_ns();
    if (collect_city_boundary(&context) != 0) {
        rt_free(context.features);
        rt_free(context.points);
        rt_free(context.pixels);
        (void)platform_close(fd);
        rt_write_cstr(2, "osmrender-rpack: failed while collecting city boundary\n");
        return 1;
    }
    boundary_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    phase_start_ns = platform_get_monotonic_time_ns();
    draw_collected_layers(&context);
    draw_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    elapsed_ms = collect_elapsed_ms + boundary_elapsed_ms + draw_elapsed_ms;
    (void)platform_close(fd);
    phase_start_ns = platform_get_monotonic_time_ns();
    if (write_png(out_path, &context) != 0) {
        rt_free(context.pixels);
        rt_write_cstr(2, "osmrender-rpack: could not write PNG\n");
        return 1;
    }
    png_elapsed_ms = (platform_get_monotonic_time_ns() - phase_start_ns) / 1000000ULL;
    total_elapsed_ms = (platform_get_monotonic_time_ns() - total_start_ns) / 1000000ULL;
    rt_free(context.features);
    rt_free(context.points);
    rt_free(context.pixels);
    write_render_summary(out_path, &context, pack_header.tile_count, 1ULL, tile_record.feature_count, elapsed_ms, profile_enabled, total_elapsed_ms, bbox_elapsed_ms,
                         header_elapsed_ms, open_tile_elapsed_ms, fill_elapsed_ms, collect_elapsed_ms, boundary_elapsed_ms, draw_elapsed_ms, png_elapsed_ms);
    return 0;
}
