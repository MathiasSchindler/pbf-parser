#include "pbf.h"
#include "platform.h"
#include "runtime.h"

#define OSMRTE_HEADER_SIZE 256U
#define OSMRTE_SECTION_RECORD_SIZE 64U
#define OSMRTE_TILE_RECORD_SIZE 128U
#define OSMRTE_TILE_PAYLOAD_HEADER_SIZE 64U
#define OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE 32U
#define OSMRTE_EMPTY_TILE_PAYLOAD_SIZE 320U
#define OSMRTE_SECTION_STRING_TABLE 0x0100U
#define OSMRTE_SECTION_TILE_DIRECTORY 0x0200U
#define OSMRTE_SECTION_FLAG_REQUIRED 1U
#define OSMRTE_SECTION_FLAG_HOT_QUERY_PATH 4U
#define OSMRTE_SECTION_FLAG_GLOBAL_PAYLOAD 16U
#define OSMRTE_DEFAULT_TILE_SIZE_M 4000U
#define OSMRTE_TILE_TYPE_WALKING_NODES 0x1000U
#define OSMRTE_TILE_TYPE_WALKING_OFFSETS 0x1001U
#define OSMRTE_TILE_TYPE_WALKING_EDGES 0x1002U
#define OSMRTE_TILE_TYPE_SNAP_GRID 0x1003U

typedef struct {
    int have_bounds;
    long long min_lat_nano;
    long long min_lon_nano;
    long long max_lat_nano;
    long long max_lon_nano;
    unsigned long long nodes_seen;
} RoutePackBounds;

typedef struct {
    unsigned long long tile_id;
    int x;
    int y;
    int min_lat_e7;
    int min_lon_e7;
    int max_lat_e7;
    int max_lon_e7;
    unsigned int source_node_count;
    unsigned long long payload_offset;
    unsigned long long neighbor_mask;
} RoutePackTile;

typedef struct {
    unsigned long long tile_id;
    unsigned int index;
    int used;
} RoutePackTileSlot;

typedef struct {
    RoutePackTile *tiles;
    RoutePackTileSlot *slots;
    unsigned int count;
    unsigned int capacity;
    unsigned int slot_capacity;
} RoutePackTileStore;

typedef struct {
    RoutePackTileStore *store;
    int origin_lat_e7;
    int origin_lon_e7;
    unsigned int tile_size_m;
    unsigned int meters_per_degree_lon;
    int failed;
} RoutePackTileCollectContext;

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--tile-size-m N] [--threads N] FILE.osm.pbf OUT.rte\n");
}

static int parse_uint_arg(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value > 4294967295ULL) return -1;
    *value_out = (unsigned int)value;
    return 0;
}

static unsigned long long align_u64(unsigned long long value, unsigned long long alignment) {
    unsigned long long mask = alignment - 1ULL;

    return (value + mask) & ~mask;
}

static int int32_from_nano_e7(long long nano, int *value_out) {
    long long value;

    if (nano >= 0) {
        value = (nano + 50LL) / 100LL;
    } else {
        value = (nano - 50LL) / 100LL;
    }
    if (value < -2147483648LL || value > 2147483647LL) return -1;
    *value_out = (int)value;
    return 0;
}

static int floor_div_ll(long long value, long long divisor) {
    long long quotient = value / divisor;
    long long remainder = value % divisor;

    if (remainder != 0LL && ((remainder < 0LL) != (divisor < 0LL))) quotient -= 1LL;
    return (int)quotient;
}

static unsigned int abs_i32(int value) {
    return value < 0 ? (unsigned int)(-value) : (unsigned int)value;
}

static unsigned int cos_degrees_q1000000(unsigned int degrees) {
    static const unsigned int table[91] = {
        1000000U, 999848U, 999391U, 998630U, 997564U, 996195U, 994522U, 992546U, 990268U, 987688U,
        984808U, 981627U, 978148U, 974370U, 970296U, 965926U, 961262U, 956305U, 951057U, 945519U,
        939693U, 933580U, 927184U, 920505U, 913545U, 906308U, 898794U, 891007U, 882948U, 874620U,
        866025U, 857167U, 848048U, 838671U, 829038U, 819152U, 809017U, 798636U, 788011U, 777146U,
        766044U, 754710U, 743145U, 731354U, 719340U, 707107U, 694658U, 681998U, 669131U, 656059U,
        642788U, 629320U, 615661U, 601815U, 587785U, 573576U, 559193U, 544639U, 529919U, 515038U,
        500000U, 484810U, 469472U, 453990U, 438371U, 422618U, 406737U, 390731U, 374607U, 358368U,
        342020U, 325568U, 309017U, 292372U, 275637U, 258819U, 241922U, 224951U, 207912U, 190809U,
        173648U, 156434U, 139173U, 121869U, 104528U, 87156U, 69756U, 52336U, 34899U, 17452U, 0U
    };
    unsigned int whole;
    unsigned int fraction;
    unsigned int left;
    unsigned int right;

    if (degrees >= 900000000U) return 0U;
    whole = degrees / 10000000U;
    fraction = degrees % 10000000U;
    if (whole >= 90U) return 0U;
    left = table[whole];
    right = table[whole + 1U];
    return left - (unsigned int)(((unsigned long long)(left - right) * fraction) / 10000000ULL);
}

static unsigned int meters_per_degree_lon_from_lat_e7(int lat_e7) {
    unsigned int cosine = cos_degrees_q1000000(abs_i32(lat_e7));

    return (unsigned int)((111320ULL * (unsigned long long)cosine + 500000ULL) / 1000000ULL);
}

static unsigned long long route_tile_id(int x, int y) {
    long long x_code = (long long)x + 0x10000000LL;
    long long y_code = (long long)y + 0x10000000LL;

    if (x_code < 0LL) x_code = 0LL;
    if (y_code < 0LL) y_code = 0LL;
    return (((unsigned long long)x_code) << 29U) | (unsigned long long)y_code;
}

static unsigned long long hash_tile_id(unsigned long long value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

static int compare_tile_by_id(const void *left_ptr, const void *right_ptr) {
    const RoutePackTile *left = (const RoutePackTile *)left_ptr;
    const RoutePackTile *right = (const RoutePackTile *)right_ptr;

    if (left->tile_id < right->tile_id) return -1;
    if (left->tile_id > right->tile_id) return 1;
    return 0;
}

static void write_u32_le(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
    out[2] = (unsigned char)((value >> 16U) & 0xffU);
    out[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void write_i32_le(unsigned char *out, int value) {
    write_u32_le(out, (unsigned int)value);
}

static void write_u64_le(unsigned char *out, unsigned long long value) {
    unsigned int byte_index;

    for (byte_index = 0U; byte_index < 8U; ++byte_index) {
        out[byte_index] = (unsigned char)((value >> (byte_index * 8U)) & 0xffU);
    }
}

static void write_i64_le(unsigned char *out, long long value) {
    write_u64_le(out, (unsigned long long)value);
}

static int tile_store_rehash(RoutePackTileStore *store, unsigned int new_slot_capacity) {
    RoutePackTileSlot *new_slots;
    unsigned int index;

    new_slots = (RoutePackTileSlot *)rt_malloc(sizeof(*new_slots) * new_slot_capacity);
    if (new_slots == 0) return -1;
    rt_memset(new_slots, 0, sizeof(*new_slots) * new_slot_capacity);
    for (index = 0U; index < store->count; ++index) {
        unsigned int slot_index = (unsigned int)(hash_tile_id(store->tiles[index].tile_id) & (unsigned long long)(new_slot_capacity - 1U));
        while (new_slots[slot_index].used) slot_index = (slot_index + 1U) & (new_slot_capacity - 1U);
        new_slots[slot_index].used = 1;
        new_slots[slot_index].tile_id = store->tiles[index].tile_id;
        new_slots[slot_index].index = index;
    }
    rt_free(store->slots);
    store->slots = new_slots;
    store->slot_capacity = new_slot_capacity;
    return 0;
}

static int tile_store_reserve(RoutePackTileStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackTile *new_tiles;

    if (needed_count <= store->capacity) return 0;
    new_capacity = store->capacity == 0U ? 256U : store->capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_tiles = (RoutePackTile *)rt_realloc(store->tiles, sizeof(*new_tiles) * new_capacity);
    if (new_tiles == 0) return -1;
    store->tiles = new_tiles;
    store->capacity = new_capacity;
    return 0;
}

static int tile_store_find_or_add(RoutePackTileStore *store, int x, int y, int lat_e7, int lon_e7, RoutePackTile **tile_out) {
    unsigned long long tile_id = route_tile_id(x, y);
    unsigned int slot_index;

    if (store->slot_capacity == 0U) {
        if (tile_store_rehash(store, 1024U) != 0) return -1;
    } else if ((store->count + 1U) * 2U >= store->slot_capacity) {
        if (tile_store_rehash(store, store->slot_capacity * 2U) != 0) return -1;
    }

    slot_index = (unsigned int)(hash_tile_id(tile_id) & (unsigned long long)(store->slot_capacity - 1U));
    while (store->slots[slot_index].used) {
        if (store->slots[slot_index].tile_id == tile_id) {
            *tile_out = store->tiles + store->slots[slot_index].index;
            return 0;
        }
        slot_index = (slot_index + 1U) & (store->slot_capacity - 1U);
    }

    if (tile_store_reserve(store, store->count + 1U) != 0) return -1;
    store->slots[slot_index].used = 1;
    store->slots[slot_index].tile_id = tile_id;
    store->slots[slot_index].index = store->count;
    *tile_out = store->tiles + store->count;
    rt_memset(*tile_out, 0, sizeof(**tile_out));
    (*tile_out)->tile_id = tile_id;
    (*tile_out)->x = x;
    (*tile_out)->y = y;
    (*tile_out)->min_lat_e7 = lat_e7;
    (*tile_out)->max_lat_e7 = lat_e7;
    (*tile_out)->min_lon_e7 = lon_e7;
    (*tile_out)->max_lon_e7 = lon_e7;
    store->count += 1U;
    return 0;
}

static int tile_store_add_node(RoutePackTileStore *store, int x, int y, int lat_e7, int lon_e7) {
    RoutePackTile *tile;

    if (tile_store_find_or_add(store, x, y, lat_e7, lon_e7, &tile) != 0) return -1;
    if (lat_e7 < tile->min_lat_e7) tile->min_lat_e7 = lat_e7;
    if (lat_e7 > tile->max_lat_e7) tile->max_lat_e7 = lat_e7;
    if (lon_e7 < tile->min_lon_e7) tile->min_lon_e7 = lon_e7;
    if (lon_e7 > tile->max_lon_e7) tile->max_lon_e7 = lon_e7;
    if (tile->source_node_count != 0xffffffffU) tile->source_node_count += 1U;
    return 0;
}

static int on_node_tile(void *user, const PbfNode *node) {
    RoutePackTileCollectContext *context = (RoutePackTileCollectContext *)user;
    int lat_e7;
    int lon_e7;
    long long metric_x_m;
    long long metric_y_m;
    int tile_x;
    int tile_y;

    if (context->failed) return -1;
    if (int32_from_nano_e7(node->lat_nano, &lat_e7) != 0 || int32_from_nano_e7(node->lon_nano, &lon_e7) != 0) return 0;
    metric_x_m = ((long long)(lon_e7 - context->origin_lon_e7) * (long long)context->meters_per_degree_lon) / 10000000LL;
    metric_y_m = ((long long)(lat_e7 - context->origin_lat_e7) * 111320LL) / 10000000LL;
    tile_x = floor_div_ll(metric_x_m, (long long)context->tile_size_m);
    tile_y = floor_div_ll(metric_y_m, (long long)context->tile_size_m);
    if (tile_store_add_node(context->store, tile_x, tile_y, lat_e7, lon_e7) != 0) {
        context->failed = 1;
        return -1;
    }
    return 0;
}

static int collect_tiles(const char *pbf_path, RoutePackTileStore *store, const RoutePackBounds *bounds, unsigned int tile_size_m, char *error, size_t error_capacity) {
    RoutePackTileCollectContext context;
    PbfStreamCallbacks callbacks;
    int origin_lat_e7 = 0;
    int origin_lon_e7 = 0;

    rt_memset(store, 0, sizeof(*store));
    if (bounds->have_bounds) {
        (void)int32_from_nano_e7((bounds->min_lat_nano + bounds->max_lat_nano) / 2LL, &origin_lat_e7);
        (void)int32_from_nano_e7((bounds->min_lon_nano + bounds->max_lon_nano) / 2LL, &origin_lon_e7);
    }
    rt_memset(&context, 0, sizeof(context));
    context.store = store;
    context.origin_lat_e7 = origin_lat_e7;
    context.origin_lon_e7 = origin_lon_e7;
    context.tile_size_m = tile_size_m;
    context.meters_per_degree_lon = meters_per_degree_lon_from_lat_e7(origin_lat_e7);
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_node_tile;
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, error_capacity) != 0) return -1;
    rt_sort(store->tiles, store->count, sizeof(*store->tiles), compare_tile_by_id);
    return tile_store_rehash(store, store->slot_capacity == 0U ? 1024U : store->slot_capacity);
}

static void write_section_record(
    unsigned char *out,
    unsigned int type,
    unsigned int flags,
    unsigned long long offset,
    unsigned long long size,
    unsigned long long record_count,
    unsigned int record_size
) {
    rt_memset(out, 0, OSMRTE_SECTION_RECORD_SIZE);
    write_u32_le(out + 0U, type);
    write_u32_le(out + 4U, flags);
    write_u64_le(out + 8U, offset);
    write_u64_le(out + 16U, size);
    write_u64_le(out + 24U, size);
    write_u64_le(out + 32U, record_count);
    write_u32_le(out + 40U, record_size);
}

static void write_tile_payload_directory_record(unsigned char *out, unsigned int type, unsigned long long relative_offset, unsigned long long size, unsigned int record_count, unsigned int record_size) {
    rt_memset(out, 0, OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE);
    write_u32_le(out + 0U, type);
    write_u32_le(out + 4U, OSMRTE_SECTION_FLAG_REQUIRED);
    write_u64_le(out + 8U, relative_offset);
    write_u64_le(out + 16U, size);
    write_u32_le(out + 24U, record_count);
    write_u32_le(out + 28U, record_size);
}

static void build_empty_tile_payload(unsigned char out[OSMRTE_EMPTY_TILE_PAYLOAD_SIZE], unsigned long long tile_id) {
    unsigned long long directory_offset = OSMRTE_TILE_PAYLOAD_HEADER_SIZE;
    unsigned long long edge_offsets_offset = OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 4ULL * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE;
    unsigned long long snap_grid_offset = 256ULL;

    rt_memset(out, 0, OSMRTE_EMPTY_TILE_PAYLOAD_SIZE);
    write_u64_le(out + 0U, tile_id);
    write_u32_le(out + 8U, 1U);
    write_u32_le(out + 16U, 4U);
    write_u32_le(out + 20U, OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE);
    write_u64_le(out + 24U, directory_offset);
    write_tile_payload_directory_record(out + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 0U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_NODES, 0ULL, 0ULL, 0U, 16U);
    write_tile_payload_directory_record(out + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 1U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_OFFSETS, edge_offsets_offset, 4ULL, 1U, 4U);
    write_tile_payload_directory_record(out + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 2U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_EDGES, 0ULL, 0ULL, 0U, 20U);
    write_tile_payload_directory_record(out + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 3U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_SNAP_GRID, snap_grid_offset, 64ULL, 1U, 64U);
}

static void write_tile_record(unsigned char out[OSMRTE_TILE_RECORD_SIZE], const RoutePackTile *tile) {
    rt_memset(out, 0, OSMRTE_TILE_RECORD_SIZE);
    write_u64_le(out + 0U, tile->tile_id);
    write_u32_le(out + 8U, 0U);
    write_i32_le(out + 12U, tile->x);
    write_i32_le(out + 16U, tile->y);
    write_u32_le(out + 20U, 0U);
    write_u32_le(out + 24U, 0U);
    write_u32_le(out + 28U, 0U);
    write_u32_le(out + 32U, 0U);
    write_u32_le(out + 36U, 0U);
    write_u32_le(out + 40U, 0U);
    write_u32_le(out + 44U, 0U);
    write_i32_le(out + 48U, tile->min_lon_e7);
    write_i32_le(out + 52U, tile->min_lat_e7);
    write_i32_le(out + 56U, tile->max_lon_e7);
    write_i32_le(out + 60U, tile->max_lat_e7);
    write_u64_le(out + 64U, tile->payload_offset);
    write_u64_le(out + 72U, OSMRTE_EMPTY_TILE_PAYLOAD_SIZE);
    write_u64_le(out + 80U, tile->payload_offset + OSMRTE_TILE_PAYLOAD_HEADER_SIZE);
    write_u32_le(out + 88U, 4U);
    write_u32_le(out + 92U, OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE);
    write_u64_le(out + 96U, tile->neighbor_mask);
}

static int tile_store_has_xy(const RoutePackTileStore *store, int x, int y) {
    unsigned long long tile_id = route_tile_id(x, y);
    unsigned int slot_index;

    if (store->slot_capacity == 0U) return 0;
    slot_index = (unsigned int)(hash_tile_id(tile_id) & (unsigned long long)(store->slot_capacity - 1U));
    while (store->slots[slot_index].used) {
        if (store->slots[slot_index].tile_id == tile_id) return 1;
        slot_index = (slot_index + 1U) & (store->slot_capacity - 1U);
    }
    return 0;
}

static unsigned long long compute_neighbor_mask(const RoutePackTileStore *store, const RoutePackTile *tile) {
    static const int dx[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const int dy[8] = { 1, 1, 1, 0, 0, -1, -1, -1 };
    unsigned long long mask = 0ULL;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        if (tile_store_has_xy(store, tile->x + dx[index], tile->y + dy[index])) mask |= 1ULL << index;
    }
    return mask;
}

static void build_header(
    unsigned char header[OSMRTE_HEADER_SIZE],
    const PbfSummary *summary,
    const RoutePackBounds *bounds,
    unsigned int tile_size_m,
    unsigned long long file_size,
    unsigned long long section_directory_offset,
    unsigned int section_count,
    unsigned long long tile_directory_offset,
    unsigned long long tile_directory_size,
    unsigned int tile_count,
    unsigned long long string_table_offset,
    unsigned long long string_table_size
) {
    int min_lon_e7 = 0;
    int min_lat_e7 = 0;
    int max_lon_e7 = 0;
    int max_lat_e7 = 0;
    int origin_lon_e7 = 0;
    int origin_lat_e7 = 0;
    long long build_time = platform_get_epoch_time();

    if (bounds->have_bounds) {
        (void)int32_from_nano_e7(bounds->min_lon_nano, &min_lon_e7);
        (void)int32_from_nano_e7(bounds->min_lat_nano, &min_lat_e7);
        (void)int32_from_nano_e7(bounds->max_lon_nano, &max_lon_e7);
        (void)int32_from_nano_e7(bounds->max_lat_nano, &max_lat_e7);
        (void)int32_from_nano_e7((bounds->min_lon_nano + bounds->max_lon_nano) / 2LL, &origin_lon_e7);
        (void)int32_from_nano_e7((bounds->min_lat_nano + bounds->max_lat_nano) / 2LL, &origin_lat_e7);
    }
    if (build_time < 0) build_time = 0;

    rt_memset(header, 0, OSMRTE_HEADER_SIZE);
    header[0] = 'O';
    header[1] = 'S';
    header[2] = 'M';
    header[3] = 'R';
    header[4] = 'T';
    header[5] = 'E';
    header[6] = '0';
    header[7] = '1';
    write_u32_le(header + 8U, 1U);
    write_u32_le(header + 12U, OSMRTE_HEADER_SIZE);
    write_u32_le(header + 16U, 0x01020304U);
    write_u64_le(header + 24U, file_size);
    write_i64_le(header + 32U, build_time);
    write_u64_le(header + 40U, summary->nodes);
    write_u64_le(header + 48U, summary->ways);
    write_u64_le(header + 56U, summary->relations);
    write_u64_le(header + 64U, section_directory_offset);
    write_u32_le(header + 72U, section_count);
    write_u32_le(header + 76U, OSMRTE_SECTION_RECORD_SIZE);
    write_u64_le(header + 80U, tile_directory_offset);
    write_u64_le(header + 88U, tile_directory_size);
    write_u32_le(header + 96U, OSMRTE_TILE_RECORD_SIZE);
    write_u32_le(header + 100U, tile_count);
    write_u32_le(header + 104U, 1U);
    write_u32_le(header + 108U, 1U);
    write_u32_le(header + 112U, 10000000U);
    write_u32_le(header + 116U, 0U);
    write_u32_le(header + 120U, 1U);
    write_u32_le(header + 124U, tile_size_m);
    write_i32_le(header + 128U, origin_lat_e7);
    write_i32_le(header + 132U, origin_lon_e7);
    write_u32_le(header + 136U, 0U);
    write_i32_le(header + 144U, min_lon_e7);
    write_i32_le(header + 148U, min_lat_e7);
    write_i32_le(header + 152U, max_lon_e7);
    write_i32_le(header + 156U, max_lat_e7);
    write_u64_le(header + 160U, string_table_offset);
    write_u64_le(header + 168U, string_table_size);
}

static int on_node_bounds(void *user, const PbfNode *node) {
    RoutePackBounds *bounds = (RoutePackBounds *)user;

    if (!bounds->have_bounds) {
        bounds->min_lat_nano = node->lat_nano;
        bounds->max_lat_nano = node->lat_nano;
        bounds->min_lon_nano = node->lon_nano;
        bounds->max_lon_nano = node->lon_nano;
        bounds->have_bounds = 1;
    } else {
        if (node->lat_nano < bounds->min_lat_nano) bounds->min_lat_nano = node->lat_nano;
        if (node->lat_nano > bounds->max_lat_nano) bounds->max_lat_nano = node->lat_nano;
        if (node->lon_nano < bounds->min_lon_nano) bounds->min_lon_nano = node->lon_nano;
        if (node->lon_nano > bounds->max_lon_nano) bounds->max_lon_nano = node->lon_nano;
    }
    bounds->nodes_seen += 1ULL;
    return 0;
}

static int collect_bounds(const char *pbf_path, RoutePackBounds *bounds, char *error, size_t error_capacity) {
    PbfStreamCallbacks callbacks;

    rt_memset(bounds, 0, sizeof(*bounds));
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_node_bounds;
    return pbf_stream_entities(pbf_path, &callbacks, bounds, error, error_capacity);
}

static int write_route_pack(
    const char *output_path,
    const PbfSummary *summary,
    const RoutePackBounds *bounds,
    RoutePackTileStore *tile_store,
    unsigned int tile_size_m
) {
    unsigned char header[OSMRTE_HEADER_SIZE];
    unsigned char section_records[OSMRTE_SECTION_RECORD_SIZE * 2U];
    unsigned char tile_record[OSMRTE_TILE_RECORD_SIZE];
    unsigned char tile_payload[OSMRTE_EMPTY_TILE_PAYLOAD_SIZE];
    unsigned char empty_string = 0;
    unsigned long long section_directory_offset = OSMRTE_HEADER_SIZE;
    unsigned int section_count = 2U;
    unsigned long long after_section_directory = section_directory_offset + (unsigned long long)section_count * OSMRTE_SECTION_RECORD_SIZE;
    unsigned long long tile_directory_offset = align_u64(after_section_directory, 64ULL);
    unsigned long long tile_directory_size = (unsigned long long)tile_store->count * OSMRTE_TILE_RECORD_SIZE;
    unsigned long long tile_payload_offset = align_u64(tile_directory_offset + tile_directory_size, 64ULL);
    unsigned long long string_table_offset = align_u64(tile_payload_offset + (unsigned long long)tile_store->count * OSMRTE_EMPTY_TILE_PAYLOAD_SIZE, 64ULL);
    unsigned long long string_table_size = 1ULL;
    unsigned long long file_size = string_table_offset + string_table_size;
    int output_fd;
    unsigned int tile_index;

    for (tile_index = 0U; tile_index < tile_store->count; ++tile_index) {
        tile_store->tiles[tile_index].payload_offset = tile_payload_offset + (unsigned long long)tile_index * OSMRTE_EMPTY_TILE_PAYLOAD_SIZE;
        tile_store->tiles[tile_index].neighbor_mask = compute_neighbor_mask(tile_store, tile_store->tiles + tile_index);
    }
    build_header(
        header,
        summary,
        bounds,
        tile_size_m,
        file_size,
        section_directory_offset,
        section_count,
        tile_directory_offset,
        tile_directory_size,
        tile_store->count,
        string_table_offset,
        string_table_size
    );
    write_section_record(
        section_records,
        OSMRTE_SECTION_STRING_TABLE,
        OSMRTE_SECTION_FLAG_REQUIRED | OSMRTE_SECTION_FLAG_GLOBAL_PAYLOAD,
        string_table_offset,
        string_table_size,
        1ULL,
        1U
    );
    write_section_record(
        section_records + OSMRTE_SECTION_RECORD_SIZE,
        OSMRTE_SECTION_TILE_DIRECTORY,
        OSMRTE_SECTION_FLAG_REQUIRED | OSMRTE_SECTION_FLAG_HOT_QUERY_PATH | OSMRTE_SECTION_FLAG_GLOBAL_PAYLOAD,
        tile_directory_offset,
        tile_directory_size,
        0ULL,
        OSMRTE_TILE_RECORD_SIZE
    );

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) return -1;
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 ||
        rt_write_all(output_fd, section_records, sizeof(section_records)) != 0) {
        (void)platform_close(output_fd);
        return -1;
    }
    for (tile_index = 0U; tile_index < tile_store->count; ++tile_index) {
        write_tile_record(tile_record, tile_store->tiles + tile_index);
        if (rt_write_all(output_fd, tile_record, sizeof(tile_record)) != 0) {
            (void)platform_close(output_fd);
            return -1;
        }
    }
    for (tile_index = 0U; tile_index < tile_store->count; ++tile_index) {
        build_empty_tile_payload(tile_payload, tile_store->tiles[tile_index].tile_id);
        if (rt_write_all(output_fd, tile_payload, sizeof(tile_payload)) != 0) {
            (void)platform_close(output_fd);
            return -1;
        }
    }
    if (rt_write_all(output_fd, &empty_string, sizeof(empty_string)) != 0) {
        (void)platform_close(output_fd);
        return -1;
    }
    if (platform_close(output_fd) != 0) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmroutepack";
    const char *pbf_path;
    const char *output_path;
    PbfSummary summary;
    RoutePackBounds bounds;
    RoutePackTileStore tile_store;
    char error[PBF_ERROR_CAPACITY];
    unsigned int tile_size_m = OSMRTE_DEFAULT_TILE_SIZE_M;
    unsigned int threads = 1U;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "--tile-size-m") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &tile_size_m) != 0 || tile_size_m == 0U) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--threads") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &threads) != 0 || threads == 0U || threads > 64U) {
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
    if (argc - argi != 2) {
        write_usage(program);
        return 1;
    }
    pbf_path = argv[argi];
    output_path = argv[argi + 1];

    error[0] = '\0';
    if (pbf_read_summary_parallel(pbf_path, threads, &summary, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmroutepack: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to read PBF summary" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    error[0] = '\0';
    if (collect_bounds(pbf_path, &bounds, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmroutepack: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to scan PBF bounds" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    error[0] = '\0';
    if (collect_tiles(pbf_path, &tile_store, &bounds, tile_size_m, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmroutepack: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to collect route tiles" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    if (write_route_pack(output_path, &summary, &bounds, &tile_store, tile_size_m) != 0) {
        rt_write_cstr(2, "osmroutepack: failed to write output route pack\n");
        return 1;
    }

    rt_write_cstr(1, "format: OSMRTE01\n");
    rt_write_cstr(1, "mode: tiled-empty-walking-payloads\n");
    rt_write_cstr(1, "output: ");
    rt_write_cstr(1, output_path);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "source_nodes: ");
    rt_write_uint(1, summary.nodes);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "source_ways: ");
    rt_write_uint(1, summary.ways);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "source_relations: ");
    rt_write_uint(1, summary.relations);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "bounds_nodes_scanned: ");
    rt_write_uint(1, bounds.nodes_seen);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "route_tiles: ");
    rt_write_uint(1, tile_store.count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_size_m: ");
    rt_write_uint(1, tile_size_m);
    rt_write_char(1, '\n');
    return 0;
}
