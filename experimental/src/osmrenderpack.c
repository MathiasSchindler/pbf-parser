#include "osmrpack.h"

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

typedef struct {
    unsigned int style_id;
    unsigned int flags;
    unsigned int ref_offset;
    unsigned int ref_count;
} PackBuildFeature;

typedef struct {
    long long id;
    long long lon_nano;
    long long lat_nano;
    int found;
} PackBuildNode;

typedef struct {
    PackBuildFeature *features;
    long long *refs;
    PackBuildNode *nodes;
    unsigned int feature_count;
    unsigned int feature_capacity;
    unsigned int ref_count;
    unsigned int ref_capacity;
    unsigned int unique_node_count;
    unsigned long long source_nodes;
    unsigned long long source_ways;
    unsigned long long classified_ways;
    unsigned long long skipped_buildings;
    int include_buildings;
    int failed;
} PackBuildContext;

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--tile-zoom N] [--buildings] FILE.osm.pbf OUT.osmrpack\n");
}

static int parse_uint_arg(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > 0xffffffffULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static int text_equals(PbfText text, const char *value) {
    size_t value_size = rt_strlen(value);
    return text.size == value_size && memcmp(text.data, value, value_size) == 0;
}

static const PbfText *find_tag_value(const PbfTag *tags, unsigned int tag_count, const char *key) {
    unsigned int tag_index;

    for (tag_index = 0U; tag_index < tag_count; ++tag_index) {
        if (text_equals(tags[tag_index].key, key)) return &tags[tag_index].value;
    }
    return 0;
}

static int tag_value_equals(const PbfText *value, const char *expected) {
    return value != 0 && text_equals(*value, expected);
}

static int classify_tags(const PbfTag *tags, unsigned int tag_count, int include_buildings, unsigned int *style_id_out, int *skipped_building_out) {
    const PbfText *highway = find_tag_value(tags, tag_count, "highway");
    const PbfText *railway = find_tag_value(tags, tag_count, "railway");
    const PbfText *waterway = find_tag_value(tags, tag_count, "waterway");
    const PbfText *natural = find_tag_value(tags, tag_count, "natural");
    const PbfText *landuse = find_tag_value(tags, tag_count, "landuse");
    const PbfText *leisure = find_tag_value(tags, tag_count, "leisure");
    const PbfText *building = find_tag_value(tags, tag_count, "building");

    *skipped_building_out = 0;
    if (tag_value_equals(natural, "water") || tag_value_equals(waterway, "riverbank")) {
        *style_id_out = PACK_STYLE_WATER;
        return 1;
    }
    if (waterway != 0) {
        *style_id_out = PACK_STYLE_WATERWAY;
        return 1;
    }
    if (tag_value_equals(landuse, "forest") || tag_value_equals(landuse, "orchard") || tag_value_equals(natural, "wood") ||
        tag_value_equals(natural, "scrub") || tag_value_equals(natural, "tree_row")) {
        *style_id_out = PACK_STYLE_FOREST;
        return 1;
    }
    if (tag_value_equals(leisure, "park") || tag_value_equals(leisure, "garden") || tag_value_equals(leisure, "nature_reserve") ||
        tag_value_equals(landuse, "grass") || tag_value_equals(landuse, "meadow") || tag_value_equals(landuse, "recreation_ground") ||
        tag_value_equals(landuse, "village_green") || tag_value_equals(natural, "grassland") || tag_value_equals(natural, "heath")) {
        *style_id_out = PACK_STYLE_PARK;
        return 1;
    }
    if (building != 0) {
        if (!include_buildings) {
            *skipped_building_out = 1;
            return 0;
        }
        *style_id_out = PACK_STYLE_BUILDING;
        return 1;
    }
    if (highway != 0) {
        if (tag_value_equals(highway, "motorway") || tag_value_equals(highway, "trunk")) *style_id_out = PACK_STYLE_MOTORWAY;
        else if (tag_value_equals(highway, "primary")) *style_id_out = PACK_STYLE_PRIMARY;
        else if (tag_value_equals(highway, "secondary")) *style_id_out = PACK_STYLE_SECONDARY;
        else if (tag_value_equals(highway, "footway") || tag_value_equals(highway, "path") || tag_value_equals(highway, "cycleway") || tag_value_equals(highway, "track")) *style_id_out = PACK_STYLE_PATH;
        else *style_id_out = PACK_STYLE_MINOR_ROAD;
        return 1;
    }
    if (railway != 0) {
        *style_id_out = PACK_STYLE_RAIL;
        return 1;
    }
    return 0;
}

static int style_can_be_area(unsigned int style_id) {
    return style_id == PACK_STYLE_WATER || style_id == PACK_STYLE_FOREST || style_id == PACK_STYLE_PARK || style_id == PACK_STYLE_BUILDING;
}

static int grow_features(PackBuildContext *context, unsigned int needed) {
    unsigned int capacity = context->feature_capacity == 0U ? 4096U : context->feature_capacity;
    PackBuildFeature *features;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->feature_capacity) return 0;
    features = (PackBuildFeature *)rt_realloc(context->features, sizeof(*features) * (size_t)capacity);
    if (features == 0) return -1;
    context->features = features;
    context->feature_capacity = capacity;
    return 0;
}

static int grow_refs(PackBuildContext *context, unsigned int needed) {
    unsigned int capacity = context->ref_capacity == 0U ? 32768U : context->ref_capacity;
    long long *refs;

    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->ref_capacity) return 0;
    refs = (long long *)rt_realloc(context->refs, sizeof(*refs) * (size_t)capacity);
    if (refs == 0) return -1;
    context->refs = refs;
    context->ref_capacity = capacity;
    return 0;
}

static int on_pack_way(void *user, const PbfWay *way) {
    PackBuildContext *context = (PackBuildContext *)user;
    unsigned int style_id;
    unsigned int flags = 0U;
    unsigned int ref_index;
    int skipped_building = 0;
    PackBuildFeature *feature;

    context->source_ways += 1ULL;
    if (way->ref_count < 2U) return 0;
    if (!classify_tags(way->tags, way->tag_count, context->include_buildings, &style_id, &skipped_building)) {
        if (skipped_building) context->skipped_buildings += 1ULL;
        return 0;
    }
    if (style_can_be_area(style_id) && way->ref_count >= 4U && way->refs[0] == way->refs[way->ref_count - 1U]) flags |= OSMRPACK_FEATURE_FLAG_AREA;
    if (grow_features(context, context->feature_count + 1U) != 0 || grow_refs(context, context->ref_count + way->ref_count) != 0) {
        context->failed = 1;
        return -1;
    }
    for (ref_index = 0U; ref_index < way->ref_count; ++ref_index) context->refs[context->ref_count + ref_index] = way->refs[ref_index];
    feature = &context->features[context->feature_count];
    feature->style_id = style_id;
    feature->flags = flags;
    feature->ref_offset = context->ref_count;
    feature->ref_count = way->ref_count;
    context->ref_count += way->ref_count;
    context->feature_count += 1U;
    context->classified_ways += 1ULL;
    return 0;
}

static int compare_node_id(const void *left_ptr, const void *right_ptr) {
    const PackBuildNode *left = (const PackBuildNode *)left_ptr;
    const PackBuildNode *right = (const PackBuildNode *)right_ptr;

    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    return 0;
}

static int build_unique_nodes(PackBuildContext *context) {
    unsigned int ref_index;
    unsigned int unique_count = 0U;

    context->nodes = (PackBuildNode *)rt_malloc(sizeof(*context->nodes) * (size_t)context->ref_count);
    if (context->nodes == 0 && context->ref_count != 0U) return -1;
    for (ref_index = 0U; ref_index < context->ref_count; ++ref_index) {
        context->nodes[ref_index].id = context->refs[ref_index];
        context->nodes[ref_index].lon_nano = 0;
        context->nodes[ref_index].lat_nano = 0;
        context->nodes[ref_index].found = 0;
    }
    rt_sort(context->nodes, context->ref_count, sizeof(*context->nodes), compare_node_id);
    for (ref_index = 0U; ref_index < context->ref_count; ++ref_index) {
        if (unique_count == 0U || context->nodes[ref_index].id != context->nodes[unique_count - 1U].id) {
            context->nodes[unique_count] = context->nodes[ref_index];
            unique_count += 1U;
        }
    }
    context->unique_node_count = unique_count;
    return 0;
}

static PackBuildNode *find_node(PackBuildContext *context, long long id) {
    unsigned int low = 0U;
    unsigned int high = context->unique_node_count;

    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        long long mid_id = context->nodes[mid].id;
        if (mid_id == id) return &context->nodes[mid];
        if (mid_id < id) low = mid + 1U;
        else high = mid;
    }
    return 0;
}

static int on_pack_node(void *user, const PbfNode *node) {
    PackBuildContext *context = (PackBuildContext *)user;
    PackBuildNode *entry;

    context->source_nodes += 1ULL;
    entry = find_node(context, node->id);
    if (entry != 0) {
        entry->lat_nano = node->lat_nano;
        entry->lon_nano = node->lon_nano;
        entry->found = 1;
    }
    return 0;
}

static void write_u32_le(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
    out[2] = (unsigned char)((value >> 16U) & 0xffU);
    out[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void write_u64_le(unsigned char *out, unsigned long long value) {
    unsigned int byte_index;

    for (byte_index = 0U; byte_index < 8U; ++byte_index) out[byte_index] = (unsigned char)((value >> (byte_index * 8U)) & 0xffU);
}

static void write_i64_le(unsigned char *out, long long value) {
    write_u64_le(out, (unsigned long long)value);
}

static int write_feature_count_placeholder(int fd) {
    unsigned char data[8];

    write_u64_le(data, 0ULL);
    return rt_write_all(fd, data, sizeof(data));
}

static int patch_feature_count(int fd, unsigned long long offset, unsigned long long feature_count) {
    unsigned char data[8];

    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) return -1;
    write_u64_le(data, feature_count);
    return rt_write_all(fd, data, sizeof(data));
}

static int write_feature_header(int fd, const PackBuildFeature *feature, unsigned int point_count, long long min_lon, long long min_lat, long long max_lon, long long max_lat) {
    unsigned char data[OSMRPACK_FEATURE_HEADER_SIZE];

    rt_memset(data, 0, sizeof(data));
    write_u32_le(data + 0U, feature->style_id);
    write_u32_le(data + 4U, feature->flags);
    write_u32_le(data + 8U, point_count);
    write_i64_le(data + 16U, min_lon);
    write_i64_le(data + 24U, min_lat);
    write_i64_le(data + 32U, max_lon);
    write_i64_le(data + 40U, max_lat);
    return rt_write_all(fd, data, sizeof(data));
}

static int write_feature_points(int fd, const long long *points, unsigned int point_count) {
    unsigned char data[16];
    unsigned int point_index;

    for (point_index = 0U; point_index < point_count; ++point_index) {
        write_i64_le(data + 0U, points[point_index * 2U + 0U]);
        write_i64_le(data + 8U, points[point_index * 2U + 1U]);
        if (rt_write_all(fd, data, sizeof(data)) != 0) return -1;
    }
    return 0;
}

static int write_geometry_pack(const char *path, unsigned int tile_zoom, PackBuildContext *context, unsigned long long elapsed_ms) {
    OsmrPackHeader header;
    OsmrPackTileRecord tile_record;
    PbfSummary summary;
    unsigned int feature_index;
    unsigned long long written_features = 0ULL;
    unsigned long long written_points = 0ULL;
    unsigned long long missing_features = 0ULL;
    unsigned int layer_mask = 0U;
    long long pack_min_lon = 0;
    long long pack_min_lat = 0;
    long long pack_max_lon = 0;
    long long pack_max_lat = 0;
    int have_pack_bbox = 0;
    int fd;
    long long feature_start;
    long long feature_end;

    pbf_summary_init(&summary);
    summary.nodes = context->source_nodes;
    summary.ways = context->source_ways;
    osmrpack_header_init(&header, tile_zoom, &summary);
    header.flags = 0U;
    header.layer_count = PACK_STYLE_COUNT;
    header.tile_count = 1ULL;
    header.tile_directory_offset = OSMRPACK_HEADER_SIZE;
    header.feature_data_offset = OSMRPACK_HEADER_SIZE + OSMRPACK_TILE_RECORD_SIZE;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) return -1;
    if (osmrpack_write_header_fd(fd, &header) != 0) goto fail;
    rt_memset(&tile_record, 0, sizeof(tile_record));
    if (osmrpack_write_tile_record_fd(fd, &tile_record) != 0) goto fail;
    feature_start = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    if (feature_start < 0) goto fail;
    if (write_feature_count_placeholder(fd) != 0) goto fail;
    for (feature_index = 0U; feature_index < context->feature_count; ++feature_index) {
        const PackBuildFeature *feature = &context->features[feature_index];
        long long *points;
        long long min_lon = 0;
        long long min_lat = 0;
        long long max_lon = 0;
        long long max_lat = 0;
        int have_bbox = 0;
        int missing = 0;
        unsigned int ref_index;

        points = (long long *)rt_malloc(sizeof(long long) * (size_t)feature->ref_count * 2U);
        if (points == 0) goto fail;
        for (ref_index = 0U; ref_index < feature->ref_count; ++ref_index) {
            PackBuildNode *node = find_node(context, context->refs[feature->ref_offset + ref_index]);
            if (node == 0 || !node->found) {
                missing = 1;
                break;
            }
            points[ref_index * 2U + 0U] = node->lon_nano;
            points[ref_index * 2U + 1U] = node->lat_nano;
            if (!have_bbox) {
                min_lon = max_lon = node->lon_nano;
                min_lat = max_lat = node->lat_nano;
                have_bbox = 1;
            } else {
                if (node->lon_nano < min_lon) min_lon = node->lon_nano;
                if (node->lon_nano > max_lon) max_lon = node->lon_nano;
                if (node->lat_nano < min_lat) min_lat = node->lat_nano;
                if (node->lat_nano > max_lat) max_lat = node->lat_nano;
            }
        }
        if (!missing && have_bbox) {
            if (write_feature_header(fd, feature, feature->ref_count, min_lon, min_lat, max_lon, max_lat) != 0 ||
                write_feature_points(fd, points, feature->ref_count) != 0) {
                rt_free(points);
                goto fail;
            }
            written_features += 1ULL;
            written_points += feature->ref_count;
            layer_mask |= 1U << feature->style_id;
            if (!have_pack_bbox) {
                pack_min_lon = min_lon;
                pack_min_lat = min_lat;
                pack_max_lon = max_lon;
                pack_max_lat = max_lat;
                have_pack_bbox = 1;
            } else {
                if (min_lon < pack_min_lon) pack_min_lon = min_lon;
                if (min_lat < pack_min_lat) pack_min_lat = min_lat;
                if (max_lon > pack_max_lon) pack_max_lon = max_lon;
                if (max_lat > pack_max_lat) pack_max_lat = max_lat;
            }
        } else {
            missing_features += 1ULL;
        }
        rt_free(points);
    }
    feature_end = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    if (feature_end < 0) goto fail;
    if (patch_feature_count(fd, (unsigned long long)feature_start, written_features) != 0) goto fail;
    header.feature_data_offset = (unsigned long long)feature_start;
    header.feature_data_size = (unsigned long long)(feature_end - feature_start);
    header.string_table_offset = (unsigned long long)feature_end;
    header.string_table_size = 0ULL;
    header.tile_count = written_features == 0ULL ? 0ULL : 1ULL;
    header.flags = written_features == 0ULL ? OSMRPACK_FLAG_EMPTY_GEOMETRY : 0U;
    tile_record.tile_id = ((unsigned long long)tile_zoom) << 58U;
    tile_record.z = tile_zoom;
    tile_record.x = 0U;
    tile_record.y = 0U;
    tile_record.feature_count = (unsigned int)written_features;
    tile_record.layer_mask = layer_mask;
    tile_record.payload_offset = header.feature_data_offset;
    tile_record.payload_size = header.feature_data_size;
    tile_record.min_lon_nano = pack_min_lon;
    tile_record.min_lat_nano = pack_min_lat;
    tile_record.max_lon_nano = pack_max_lon;
    tile_record.max_lat_nano = pack_max_lat;
    if (platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) goto fail;
    if (osmrpack_write_header_fd(fd, &header) != 0 || osmrpack_write_tile_record_fd(fd, &tile_record) != 0) goto fail;
    if (platform_close(fd) != 0) return -1;
    rt_write_cstr(1, "osmrpack_written: ");
    rt_write_cstr(1, path);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_zoom: ");
    rt_write_uint(1, tile_zoom);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "source_nodes_scanned: ");
    rt_write_uint(1, context->source_nodes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "source_ways_scanned: ");
    rt_write_uint(1, context->source_ways);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "classified_ways: ");
    rt_write_uint(1, context->classified_ways);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "skipped_buildings: ");
    rt_write_uint(1, context->skipped_buildings);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "unique_nodes_needed: ");
    rt_write_uint(1, context->unique_node_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_written: ");
    rt_write_uint(1, written_features);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "features_skipped_missing_nodes: ");
    rt_write_uint(1, missing_features);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "points_written: ");
    rt_write_uint(1, written_points);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "build_elapsed_ms: ");
    rt_write_uint(1, elapsed_ms);
    rt_write_char(1, '\n');
    return 0;

fail:
    (void)platform_close(fd);
    return -1;
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmrenderpack";
    const char *pbf_path;
    const char *pack_path;
    unsigned int tile_zoom = OSMRPACK_DEFAULT_TILE_ZOOM;
    int argi = 1;
    PbfStreamCallbacks callbacks;
    PackBuildContext context;
    char error[PBF_ERROR_CAPACITY];
    unsigned long long start_ns;
    unsigned long long elapsed_ms;

    rt_memset(&context, 0, sizeof(context));
    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "--tile-zoom") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &tile_zoom) != 0 || tile_zoom > OSMRPACK_MAX_TILE_ZOOM) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--buildings") == 0) {
            context.include_buildings = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            write_usage(program);
            return 0;
        } else {
            write_usage(program);
            return 1;
        }
    }
    if (argc - argi != 2) {
        write_usage(program);
        return 1;
    }
    pbf_path = argv[argi];
    pack_path = argv[argi + 1];
    start_ns = platform_get_monotonic_time_ns();

    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.way = on_pack_way;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
        rt_write_cstr(2, "osmrenderpack: ");
        rt_write_cstr(2, context.failed ? "out of memory while collecting way refs" : (error[0] == '\0' ? "could not collect way refs" : error));
        rt_write_char(2, '\n');
        return 1;
    }
    if (build_unique_nodes(&context) != 0) {
        rt_write_cstr(2, "osmrenderpack: out of memory while building node lookup\n");
        return 1;
    }
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_pack_node;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmrenderpack: ");
        rt_write_cstr(2, error[0] == '\0' ? "could not collect node coordinates" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    elapsed_ms = (platform_get_monotonic_time_ns() - start_ns) / 1000000ULL;
    if (write_geometry_pack(pack_path, tile_zoom, &context, elapsed_ms) != 0) {
        rt_write_cstr(2, "osmrenderpack: could not write geometry pack\n");
        return 1;
    }
    rt_free(context.features);
    rt_free(context.refs);
    rt_free(context.nodes);
    return 0;
}
