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
#define OSMRTE_SECTION_ADDRESS_DICTIONARIES 0x0400U
#define OSMRTE_SECTION_FLAG_REQUIRED 1U
#define OSMRTE_SECTION_FLAG_HOT_QUERY_PATH 4U
#define OSMRTE_SECTION_FLAG_GLOBAL_PAYLOAD 16U
#define OSMRTE_DEFAULT_TILE_SIZE_M 4000U
#define OSMRTE_ADDRESS_SECTION_HEADER_SIZE 64U
#define OSMRTE_ADDRESS_RECORD_SIZE 80U
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

typedef struct RoutePackTile RoutePackTile;

struct RoutePackTile {
    unsigned long long tile_id;
    int x;
    int y;
    int min_lat_e7;
    int min_lon_e7;
    int max_lat_e7;
    int max_lon_e7;
    unsigned int source_node_count;
    unsigned int address_count;
    unsigned int graph_segment_count;
    unsigned int graph_segment_offset;
    unsigned int graph_segment_cursor;
    unsigned long long payload_offset;
    unsigned long long payload_size;
    unsigned long long neighbor_mask;
    void *graph;
};

typedef struct {
    long long id;
    int lat_e7;
    int lon_e7;
    int tile_x;
    int tile_y;
    RoutePackTile *tile;
    unsigned int graph_index;
    int found;
} RoutePackWalkCoord;

typedef struct {
    long long id;
    unsigned int index;
    int used;
} RoutePackWalkCoordSlot;

typedef struct {
    RoutePackWalkCoord *items;
    RoutePackWalkCoordSlot *slots;
    unsigned int count;
    unsigned int capacity;
    unsigned int slot_capacity;
} RoutePackWalkCoordStore;

typedef struct {
    long long left_id;
    long long right_id;
} RoutePackWalkSegment;

typedef struct {
    RoutePackWalkSegment *items;
    unsigned int count;
    unsigned int capacity;
} RoutePackWalkSegmentStore;

typedef struct {
    unsigned int tile_index;
    unsigned int left_coord_index;
    unsigned int right_coord_index;
} RoutePackTileSegment;

typedef struct {
    RoutePackTileSegment *items;
    unsigned int count;
    unsigned int capacity;
} RoutePackTileSegmentStore;

typedef struct {
    long long id;
    int lat_e7;
    int lon_e7;
    unsigned int first_edge;
} RoutePackTileGraphNode;

typedef struct {
    long long id;
    unsigned int index;
    int used;
} RoutePackTileGraphNodeSlot;

typedef struct {
    unsigned int from;
    unsigned int to;
    unsigned int meters;
} RoutePackTileGraphEdge;

typedef struct {
    RoutePackTileGraphNode *nodes;
    RoutePackTileGraphNodeSlot *slots;
    RoutePackTileGraphEdge *edges;
    unsigned int node_count;
    unsigned int node_capacity;
    unsigned int slot_capacity;
    unsigned int edge_count;
    unsigned int edge_capacity;
} RoutePackTileGraph;

typedef struct {
    RoutePackWalkCoordStore *coords;
    unsigned long long walkable_way_count;
    unsigned long long ref_count;
    int failed;
} RoutePackWalkCollectContext;

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
    RoutePackWalkCoordStore *coords;
    RoutePackTileStore *tiles;
    unsigned long long segment_count;
    unsigned long long tile_segment_count;
    int failed;
} RoutePackWalkCountContext;

typedef struct {
    RoutePackWalkCoordStore *coords;
    RoutePackTileStore *tiles;
    RoutePackTileSegmentStore *segments;
    int origin_lat_e7;
    int origin_lon_e7;
    unsigned int meters_per_degree_lon;
    unsigned int tile_size_m;
    unsigned long long walkable_way_count;
    unsigned long long segment_count;
    unsigned long long inserted_segment_count;
    unsigned long long next_progress_segment_count;
    int failed;
} RoutePackWalkMaterializeContext;

typedef struct {
    RoutePackTileStore *store;
    int origin_lat_e7;
    int origin_lon_e7;
    unsigned int tile_size_m;
    unsigned int meters_per_degree_lon;
    int failed;
} RoutePackTileCollectContext;

typedef struct {
    PbfText state;
    PbfText city;
    PbfText suburb;
    PbfText street;
    PbfText housenumber;
    PbfText postcode;
    int has_state;
    int has_city;
    int has_suburb;
    int has_street;
    int has_housenumber;
    int has_postcode;
} RoutePackAddressFields;

typedef struct {
    long long node_id;
    unsigned int address_index;
} RoutePackAddressNodeRef;

typedef struct {
    unsigned int entity_type;
    unsigned int flags;
    long long id;
    int lat_e7;
    int lon_e7;
    unsigned long long tile_id;
    long long lat_nano_sum;
    long long lon_nano_sum;
    unsigned int coord_ref_count;
    unsigned int state_offset;
    unsigned int state_size;
    unsigned int city_offset;
    unsigned int city_size;
    unsigned int suburb_offset;
    unsigned int suburb_size;
    unsigned int street_offset;
    unsigned int street_size;
    unsigned int housenumber_offset;
    unsigned int housenumber_size;
    unsigned int postcode_offset;
    unsigned int postcode_size;
} RoutePackAddressRecord;

typedef struct {
    RoutePackAddressRecord *records;
    unsigned int count;
    unsigned int capacity;
    char *strings;
    unsigned int string_size;
    unsigned int string_capacity;
    RoutePackAddressNodeRef *node_refs;
    unsigned int node_ref_count;
    unsigned int node_ref_capacity;
} RoutePackAddressStore;

typedef struct {
    RoutePackAddressStore *addresses;
    RoutePackTileStore *tiles;
    int origin_lat_e7;
    int origin_lon_e7;
    unsigned int tile_size_m;
    unsigned int meters_per_degree_lon;
    int failed;
} RoutePackAddressCollectContext;

typedef struct {
    RoutePackAddressStore *addresses;
    int failed;
} RoutePackAddressResolveContext;

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

static unsigned int hash_i64(long long value) {
    unsigned long long x = (unsigned long long)value;
    x ^= x >> 33U;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33U;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33U;
    return (unsigned int)x;
}

static int compare_tile_by_id(const void *left_ptr, const void *right_ptr) {
    const RoutePackTile *left = (const RoutePackTile *)left_ptr;
    const RoutePackTile *right = (const RoutePackTile *)right_ptr;

    if (left->tile_id < right->tile_id) return -1;
    if (left->tile_id > right->tile_id) return 1;
    return 0;
}

static int compare_tile_segment_by_tile(const void *left_ptr, const void *right_ptr) {
    const RoutePackTileSegment *left = (const RoutePackTileSegment *)left_ptr;
    const RoutePackTileSegment *right = (const RoutePackTileSegment *)right_ptr;

    if (left->tile_index < right->tile_index) return -1;
    if (left->tile_index > right->tile_index) return 1;
    if (left->left_coord_index < right->left_coord_index) return -1;
    if (left->left_coord_index > right->left_coord_index) return 1;
    if (left->right_coord_index < right->right_coord_index) return -1;
    if (left->right_coord_index > right->right_coord_index) return 1;
    return 0;
}

static int compare_u32_value(const void *left_ptr, const void *right_ptr) {
    unsigned int left = *(const unsigned int *)left_ptr;
    unsigned int right = *(const unsigned int *)right_ptr;

    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

static unsigned int lower_bound_u32(const unsigned int *items, unsigned int count, unsigned int value) {
    unsigned int low = 0U;
    unsigned int high = count;

    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        if (items[mid] < value) low = mid + 1U;
        else high = mid;
    }
    return low;
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

static unsigned long long abs_i64(long long value) {
    return value < 0 ? (unsigned long long)(-value) : (unsigned long long)value;
}

static unsigned long long isqrt_u64(unsigned long long value) {
    unsigned long long result = 0ULL;
    unsigned long long bit = 1ULL << 62U;

    while (bit > value) bit >>= 2U;
    while (bit != 0ULL) {
        if (value >= result + bit) { value -= result + bit; result = (result >> 1U) + bit; }
        else result >>= 1U;
        bit >>= 2U;
    }
    return result;
}

static unsigned int distance_e7_m(int lat_a, int lon_a, int lat_b, int lon_b) {
    unsigned long long dx = (abs_i64((long long)lon_a - (long long)lon_b) * 68000ULL + 5000000ULL) / 10000000ULL;
    unsigned long long dy = (abs_i64((long long)lat_a - (long long)lat_b) * 111320ULL + 5000000ULL) / 10000000ULL;
    unsigned long long distance = isqrt_u64(dx * dx + dy * dy);
    if (distance == 0ULL) distance = 1ULL;
    return distance > 0xffffffffULL ? 0xffffffffU : (unsigned int)distance;
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

static RoutePackTile *tile_store_find_by_xy(RoutePackTileStore *store, int x, int y) {
    unsigned long long tile_id = route_tile_id(x, y);
    unsigned int slot_index;

    if (store->slot_capacity == 0U) return 0;
    slot_index = (unsigned int)(hash_tile_id(tile_id) & (unsigned long long)(store->slot_capacity - 1U));
    while (store->slots[slot_index].used) {
        if (store->slots[slot_index].tile_id == tile_id) return store->tiles + store->slots[slot_index].index;
        slot_index = (slot_index + 1U) & (store->slot_capacity - 1U);
    }
    return 0;
}

static int text_equals_cstr(PbfText text, const char *value) {
    size_t value_size = rt_strlen(value);

    return text.size == value_size && memcmp(text.data, value, value_size) == 0;
}

static int text_one_of(PbfText text, const char *a, const char *b, const char *c, const char *d) {
    return (a != 0 && text_equals_cstr(text, a)) || (b != 0 && text_equals_cstr(text, b)) || (c != 0 && text_equals_cstr(text, c)) || (d != 0 && text_equals_cstr(text, d));
}

static PbfText tag_value(const PbfTag *tags, unsigned int tag_count, const char *key) {
    PbfText empty;
    unsigned int index;

    empty.data = "";
    empty.size = 0U;
    for (index = 0U; index < tag_count; ++index) if (text_equals_cstr(tags[index].key, key)) return tags[index].value;
    return empty;
}

static int way_walkable(const PbfTag *tags, unsigned int tag_count) {
    PbfText highway = tag_value(tags, tag_count, "highway");
    PbfText access = tag_value(tags, tag_count, "access");
    PbfText foot = tag_value(tags, tag_count, "foot");

    if (highway.size == 0U) return 0;
    if (text_one_of(access, "private", "no", 0, 0) || text_one_of(foot, "private", "no", 0, 0)) return 0;
    if (text_one_of(foot, "yes", "designated", "permissive", "official")) return 1;
    if (text_one_of(highway, "motorway", "motorway_link", "trunk", "trunk_link")) return 0;
    if (text_one_of(highway, "construction", "proposed", "raceway", "bus_guideway")) return 0;
    if (text_one_of(highway, "footway", "path", "pedestrian", "steps")) return 1;
    if (text_one_of(highway, "residential", "living_street", "service", "track")) return 1;
    if (text_one_of(highway, "unclassified", "tertiary", "secondary", "primary")) return 1;
    return text_one_of(highway, "tertiary_link", "secondary_link", "primary_link", "road");
}

static int walk_coord_reserve(RoutePackWalkCoordStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackWalkCoord *new_items;

    if (needed_count <= store->capacity) return 0;
    new_capacity = store->capacity == 0U ? 262144U : store->capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_items = (RoutePackWalkCoord *)rt_realloc(store->items, sizeof(*new_items) * new_capacity);
    if (new_items == 0) return -1;
    store->items = new_items;
    store->capacity = new_capacity;
    return 0;
}

static int walk_coord_rehash(RoutePackWalkCoordStore *store, unsigned int new_slot_capacity) {
    RoutePackWalkCoordSlot *new_slots;
    unsigned int index;

    new_slots = (RoutePackWalkCoordSlot *)rt_malloc(sizeof(*new_slots) * new_slot_capacity);
    if (new_slots == 0) return -1;
    rt_memset(new_slots, 0, sizeof(*new_slots) * new_slot_capacity);
    for (index = 0U; index < store->count; ++index) {
        unsigned int slot = hash_i64(store->items[index].id) & (new_slot_capacity - 1U);
        while (new_slots[slot].used) slot = (slot + 1U) & (new_slot_capacity - 1U);
        new_slots[slot].used = 1;
        new_slots[slot].id = store->items[index].id;
        new_slots[slot].index = index;
    }
    rt_free(store->slots);
    store->slots = new_slots;
    store->slot_capacity = new_slot_capacity;
    return 0;
}

static RoutePackWalkCoord *walk_coord_find(RoutePackWalkCoordStore *store, long long id) {
    unsigned int slot;

    if (store->slot_capacity == 0U) return 0;
    slot = hash_i64(id) & (store->slot_capacity - 1U);
    while (store->slots[slot].used) {
        if (store->slots[slot].id == id) return store->items + store->slots[slot].index;
        slot = (slot + 1U) & (store->slot_capacity - 1U);
    }
    return 0;
}

static int walk_coord_find_or_add(RoutePackWalkCoordStore *store, long long id) {
    unsigned int slot;

    if (store->slot_capacity == 0U) {
        if (walk_coord_rehash(store, 524288U) != 0) return -1;
    } else if ((store->count + 1U) * 2U >= store->slot_capacity) {
        if (walk_coord_rehash(store, store->slot_capacity * 2U) != 0) return -1;
    }
    slot = hash_i64(id) & (store->slot_capacity - 1U);
    while (store->slots[slot].used) {
        if (store->slots[slot].id == id) return 0;
        slot = (slot + 1U) & (store->slot_capacity - 1U);
    }
    if (walk_coord_reserve(store, store->count + 1U) != 0) return -1;
    store->slots[slot].used = 1;
    store->slots[slot].id = id;
    store->slots[slot].index = store->count;
    rt_memset(store->items + store->count, 0, sizeof(*store->items));
    store->items[store->count].id = id;
    store->items[store->count].graph_index = 0xffffffffU;
    store->count += 1U;
    return 0;
}

static int walk_segment_reserve(RoutePackWalkSegmentStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackWalkSegment *new_items;

    if (needed_count <= store->capacity) return 0;
    new_capacity = store->capacity == 0U ? 262144U : store->capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_items = (RoutePackWalkSegment *)rt_realloc(store->items, sizeof(*new_items) * new_capacity);
    if (new_items == 0) return -1;
    store->items = new_items;
    store->capacity = new_capacity;
    return 0;
}

static int walk_segment_add(RoutePackWalkSegmentStore *store, long long left_id, long long right_id) {
    RoutePackWalkSegment *segment;

    if (left_id == right_id) return 0;
    if (walk_segment_reserve(store, store->count + 1U) != 0) return -1;
    segment = store->items + store->count;
    segment->left_id = left_id;
    segment->right_id = right_id;
    store->count += 1U;
    return 0;
}

static int tile_segment_reserve(RoutePackTileSegmentStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackTileSegment *new_items;

    if (needed_count <= store->capacity) return 0;
    new_capacity = store->capacity == 0U ? 262144U : store->capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_items = (RoutePackTileSegment *)rt_realloc(store->items, sizeof(*new_items) * new_capacity);
    if (new_items == 0) return -1;
    store->items = new_items;
    store->capacity = new_capacity;
    return 0;
}

static int tile_segment_add(RoutePackTileSegmentStore *store, unsigned int tile_index, unsigned int left_coord_index, unsigned int right_coord_index) {
    RoutePackTileSegment *segment;

    if (tile_segment_reserve(store, store->count + 1U) != 0) return -1;
    segment = store->items + store->count;
    segment->tile_index = tile_index;
    segment->left_coord_index = left_coord_index;
    segment->right_coord_index = right_coord_index;
    store->count += 1U;
    return 0;
}

static int tile_segment_add_to_tile(RoutePackTileSegmentStore *store, RoutePackTile *tile, unsigned int tile_index, unsigned int left_coord_index, unsigned int right_coord_index) {
    RoutePackTileSegment *segment;
    unsigned int position = tile->graph_segment_cursor;

    if (position >= store->capacity) return -1;
    tile->graph_segment_cursor += 1U;
    if (store->count < tile->graph_segment_cursor) store->count = tile->graph_segment_cursor;
    segment = store->items + position;
    segment->tile_index = tile_index;
    segment->left_coord_index = left_coord_index;
    segment->right_coord_index = right_coord_index;
    return 0;
}

static void find_address_fields(const PbfTag *tags, unsigned int tag_count, RoutePackAddressFields *fields) {
    unsigned int index;

    rt_memset(fields, 0, sizeof(*fields));
    for (index = 0U; index < tag_count; ++index) {
        if (!fields->has_state && text_equals_cstr(tags[index].key, "addr:state")) { fields->state = tags[index].value; fields->has_state = 1; }
        else if (!fields->has_city && text_equals_cstr(tags[index].key, "addr:city")) { fields->city = tags[index].value; fields->has_city = 1; }
        else if (!fields->has_suburb && text_equals_cstr(tags[index].key, "addr:suburb")) { fields->suburb = tags[index].value; fields->has_suburb = 1; }
        else if (!fields->has_street && text_equals_cstr(tags[index].key, "addr:street")) { fields->street = tags[index].value; fields->has_street = 1; }
        else if (!fields->has_housenumber && text_equals_cstr(tags[index].key, "addr:housenumber")) { fields->housenumber = tags[index].value; fields->has_housenumber = 1; }
        else if (!fields->has_postcode && text_equals_cstr(tags[index].key, "addr:postcode")) { fields->postcode = tags[index].value; fields->has_postcode = 1; }
    }
}

static int address_fields_complete(const RoutePackAddressFields *fields) {
    return fields->has_street && fields->has_housenumber && fields->has_postcode;
}

static int address_store_reserve_records(RoutePackAddressStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackAddressRecord *new_records;

    if (needed_count <= store->capacity) return 0;
    new_capacity = store->capacity == 0U ? 65536U : store->capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_records = (RoutePackAddressRecord *)rt_realloc(store->records, sizeof(*new_records) * new_capacity);
    if (new_records == 0) return -1;
    store->records = new_records;
    store->capacity = new_capacity;
    return 0;
}

static int address_store_reserve_strings(RoutePackAddressStore *store, unsigned int needed_size) {
    unsigned int new_capacity;
    char *new_strings;

    if (needed_size <= store->string_capacity) return 0;
    new_capacity = store->string_capacity == 0U ? 1048576U : store->string_capacity * 2U;
    while (new_capacity < needed_size) new_capacity *= 2U;
    new_strings = (char *)rt_realloc(store->strings, new_capacity);
    if (new_strings == 0) return -1;
    store->strings = new_strings;
    store->string_capacity = new_capacity;
    return 0;
}

static int address_store_reserve_node_refs(RoutePackAddressStore *store, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackAddressNodeRef *new_refs;

    if (needed_count <= store->node_ref_capacity) return 0;
    new_capacity = store->node_ref_capacity == 0U ? 262144U : store->node_ref_capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_refs = (RoutePackAddressNodeRef *)rt_realloc(store->node_refs, sizeof(*new_refs) * new_capacity);
    if (new_refs == 0) return -1;
    store->node_refs = new_refs;
    store->node_ref_capacity = new_capacity;
    return 0;
}

static int address_store_add_node_ref(RoutePackAddressStore *store, long long node_id, unsigned int address_index) {
    RoutePackAddressNodeRef *ref;

    if (address_store_reserve_node_refs(store, store->node_ref_count + 1U) != 0) return -1;
    ref = store->node_refs + store->node_ref_count;
    ref->node_id = node_id;
    ref->address_index = address_index;
    store->node_ref_count += 1U;
    return 0;
}

static int address_store_add_text(RoutePackAddressStore *store, PbfText text, unsigned int *offset_out, unsigned int *size_out) {
    if (text.size > 0xffffffffU || store->string_size > 0xffffffffU - (unsigned int)text.size) return -1;
    if (address_store_reserve_strings(store, store->string_size + (unsigned int)text.size) != 0) return -1;
    *offset_out = store->string_size;
    *size_out = (unsigned int)text.size;
    if (text.size != 0U) memcpy(store->strings + store->string_size, text.data, text.size);
    store->string_size += (unsigned int)text.size;
    return 0;
}

static int address_store_add(RoutePackAddressStore *store, unsigned int entity_type, long long id, int has_coord, int lat_e7, int lon_e7, unsigned long long tile_id, const RoutePackAddressFields *fields, unsigned int *index_out) {
    RoutePackAddressRecord *record;
    unsigned int record_index;

    if (!address_fields_complete(fields)) return 0;
    if (address_store_reserve_records(store, store->count + 1U) != 0) return -1;
    record_index = store->count;
    record = store->records + record_index;
    rt_memset(record, 0, sizeof(*record));
    record->entity_type = entity_type;
    record->flags = has_coord ? 1U : 0U;
    record->id = id;
    record->lat_e7 = has_coord ? lat_e7 : 0;
    record->lon_e7 = has_coord ? lon_e7 : 0;
    record->tile_id = tile_id;
    if (address_store_add_text(store, fields->state, &record->state_offset, &record->state_size) != 0) return -1;
    if (address_store_add_text(store, fields->city, &record->city_offset, &record->city_size) != 0) return -1;
    if (address_store_add_text(store, fields->suburb, &record->suburb_offset, &record->suburb_size) != 0) return -1;
    if (address_store_add_text(store, fields->street, &record->street_offset, &record->street_size) != 0) return -1;
    if (address_store_add_text(store, fields->housenumber, &record->housenumber_offset, &record->housenumber_size) != 0) return -1;
    if (address_store_add_text(store, fields->postcode, &record->postcode_offset, &record->postcode_size) != 0) return -1;
    store->count += 1U;
    if (index_out != 0) *index_out = record_index;
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

static int route_pack_coord_to_tile(int lat_e7, int lon_e7, int origin_lat_e7, int origin_lon_e7, unsigned int meters_per_degree_lon, unsigned int tile_size_m, int *tile_x_out, int *tile_y_out, unsigned long long *tile_id_out) {
    long long metric_x_m = ((long long)(lon_e7 - origin_lon_e7) * (long long)meters_per_degree_lon) / 10000000LL;
    long long metric_y_m = ((long long)(lat_e7 - origin_lat_e7) * 111320LL) / 10000000LL;
    int tile_x = floor_div_ll(metric_x_m, (long long)tile_size_m);
    int tile_y = floor_div_ll(metric_y_m, (long long)tile_size_m);

    *tile_x_out = tile_x;
    *tile_y_out = tile_y;
    *tile_id_out = route_tile_id(tile_x, tile_y);
    return 0;
}

static int tile_graph_node_rehash(RoutePackTileGraph *graph, unsigned int new_slot_capacity) {
    RoutePackTileGraphNodeSlot *new_slots;
    unsigned int index;

    new_slots = (RoutePackTileGraphNodeSlot *)rt_malloc(sizeof(*new_slots) * new_slot_capacity);
    if (new_slots == 0) return -1;
    rt_memset(new_slots, 0, sizeof(*new_slots) * new_slot_capacity);
    for (index = 0U; index < graph->node_count; ++index) {
        unsigned int slot = hash_i64(graph->nodes[index].id) & (new_slot_capacity - 1U);
        while (new_slots[slot].used) slot = (slot + 1U) & (new_slot_capacity - 1U);
        new_slots[slot].used = 1;
        new_slots[slot].id = graph->nodes[index].id;
        new_slots[slot].index = index;
    }
    rt_free(graph->slots);
    graph->slots = new_slots;
    graph->slot_capacity = new_slot_capacity;
    return 0;
}

static int tile_graph_reserve_nodes(RoutePackTileGraph *graph, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackTileGraphNode *new_nodes;

    if (needed_count <= graph->node_capacity) return 0;
    new_capacity = graph->node_capacity == 0U ? 512U : graph->node_capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_nodes = (RoutePackTileGraphNode *)rt_realloc(graph->nodes, sizeof(*new_nodes) * new_capacity);
    if (new_nodes == 0) return -1;
    graph->nodes = new_nodes;
    graph->node_capacity = new_capacity;
    return 0;
}

static int tile_graph_reserve_edges(RoutePackTileGraph *graph, unsigned int needed_count) {
    unsigned int new_capacity;
    RoutePackTileGraphEdge *new_edges;

    if (needed_count <= graph->edge_capacity) return 0;
    new_capacity = graph->edge_capacity == 0U ? 1024U : graph->edge_capacity * 2U;
    while (new_capacity < needed_count) new_capacity *= 2U;
    new_edges = (RoutePackTileGraphEdge *)rt_realloc(graph->edges, sizeof(*new_edges) * new_capacity);
    if (new_edges == 0) return -1;
    graph->edges = new_edges;
    graph->edge_capacity = new_capacity;
    return 0;
}

static unsigned int next_power_of_two_u32(unsigned int value) {
    unsigned int result = 1U;
    while (result < value && result < 0x80000000U) result <<= 1U;
    return result < value ? value : result;
}

static int tile_graph_preallocate(RoutePackTile *tile) {
    RoutePackTileGraph *graph;
    unsigned int node_capacity;
    unsigned int slot_capacity;
    unsigned int edge_capacity;

    if (tile->graph_segment_count == 0U) return 0;
    if (tile->graph == 0) {
        tile->graph = rt_malloc(sizeof(RoutePackTileGraph));
        if (tile->graph == 0) return -1;
        rt_memset(tile->graph, 0, sizeof(RoutePackTileGraph));
    }
    graph = (RoutePackTileGraph *)tile->graph;
    edge_capacity = tile->graph_segment_count > 0x7fffffffU ? 0xffffffffU : tile->graph_segment_count * 2U;
    node_capacity = tile->graph_segment_count + 1U;
    if (node_capacity < 512U) node_capacity = 512U;
    slot_capacity = next_power_of_two_u32(node_capacity * 2U);
    if (tile_graph_reserve_edges(graph, edge_capacity) != 0 || tile_graph_reserve_nodes(graph, node_capacity) != 0 || tile_graph_node_rehash(graph, slot_capacity) != 0) return -1;
    return 0;
}

static int tile_graph_node_for_coord(RoutePackTileGraph *graph, const RoutePackWalkCoord *coord, unsigned int *node_out) {
    unsigned int slot;

    if (graph->slot_capacity == 0U) {
        if (tile_graph_node_rehash(graph, 1024U) != 0) return -1;
    } else if ((graph->node_count + 1U) * 2U >= graph->slot_capacity) {
        if (tile_graph_node_rehash(graph, graph->slot_capacity * 2U) != 0) return -1;
    }
    slot = hash_i64(coord->id) & (graph->slot_capacity - 1U);
    while (graph->slots[slot].used) {
        if (graph->slots[slot].id == coord->id) { *node_out = graph->slots[slot].index; return 0; }
        slot = (slot + 1U) & (graph->slot_capacity - 1U);
    }
    if (tile_graph_reserve_nodes(graph, graph->node_count + 1U) != 0) return -1;
    graph->slots[slot].used = 1;
    graph->slots[slot].id = coord->id;
    graph->slots[slot].index = graph->node_count;
    graph->nodes[graph->node_count].id = coord->id;
    graph->nodes[graph->node_count].lat_e7 = coord->lat_e7;
    graph->nodes[graph->node_count].lon_e7 = coord->lon_e7;
    graph->nodes[graph->node_count].first_edge = 0U;
    *node_out = graph->node_count;
    graph->node_count += 1U;
    return 0;
}

static int tile_graph_add_directed(RoutePackTileGraph *graph, unsigned int from, unsigned int to, unsigned int meters) {
    RoutePackTileGraphEdge *edge;

    if (from == to) return 0;
    if (tile_graph_reserve_edges(graph, graph->edge_count + 1U) != 0) return -1;
    edge = graph->edges + graph->edge_count;
    edge->from = from;
    edge->to = to;
    edge->meters = meters;
    graph->edge_count += 1U;
    return 0;
}

static int tile_graph_node_for_coord_cached(RoutePackTile *tile, RoutePackTileGraph *graph, RoutePackWalkCoord *coord, unsigned int *node_out) {
    if (coord->tile == tile && coord->graph_index != 0xffffffffU) {
        *node_out = coord->graph_index;
        return 0;
    }
    if (tile_graph_node_for_coord(graph, coord, node_out) != 0) return -1;
    if (coord->tile == tile) coord->graph_index = *node_out;
    return 0;
}

static int tile_graph_add_segment(RoutePackTile *tile, RoutePackWalkCoord *left, RoutePackWalkCoord *right) {
    RoutePackTileGraph *graph;
    unsigned int left_index;
    unsigned int right_index;
    unsigned int meters;

    if (tile->graph == 0) {
        tile->graph = rt_malloc(sizeof(RoutePackTileGraph));
        if (tile->graph == 0) return -1;
        rt_memset(tile->graph, 0, sizeof(RoutePackTileGraph));
    }
    graph = (RoutePackTileGraph *)tile->graph;
    if (tile_graph_node_for_coord_cached(tile, graph, left, &left_index) != 0 || tile_graph_node_for_coord_cached(tile, graph, right, &right_index) != 0) return -1;
    meters = distance_e7_m(left->lat_e7, left->lon_e7, right->lat_e7, right->lon_e7);
    if (tile_graph_add_directed(graph, left_index, right_index, meters) != 0 || tile_graph_add_directed(graph, right_index, left_index, meters) != 0) return -1;
    return 0;
}

static int on_walk_way_refs(void *user, const PbfWay *way) {
    RoutePackWalkCollectContext *context = (RoutePackWalkCollectContext *)user;
    unsigned int index;

    if (context->failed) return -1;
    if (!way_walkable(way->tags, way->tag_count)) return 0;
    context->walkable_way_count += 1ULL;
    context->ref_count += way->ref_count;
    for (index = 0U; index < way->ref_count; ++index) {
        if (walk_coord_find_or_add(context->coords, way->refs[index]) != 0) { context->failed = 1; return -1; }
    }
    return 0;
}

static int materialize_walk_segment(RoutePackWalkMaterializeContext *context, RoutePackWalkCoord *left, RoutePackWalkCoord *right) {
    unsigned int left_coord_index;
    unsigned int right_coord_index;

    if (left == 0 || right == 0 || !left->found || !right->found) return 0;
    left_coord_index = (unsigned int)(left - context->coords->items);
    right_coord_index = (unsigned int)(right - context->coords->items);
    if (left->tile != 0) {
        unsigned int tile_index = (unsigned int)(left->tile - context->tiles->tiles);
        if (tile_segment_add_to_tile(context->segments, left->tile, tile_index, left_coord_index, right_coord_index) != 0) return -1;
        context->inserted_segment_count += 1ULL;
    }
    if (right->tile != 0 && right->tile != left->tile) {
        unsigned int tile_index = (unsigned int)(right->tile - context->tiles->tiles);
        if (tile_segment_add_to_tile(context->segments, right->tile, tile_index, left_coord_index, right_coord_index) != 0) return -1;
        context->inserted_segment_count += 1ULL;
    }
    return 0;
}

static int count_walk_segment(RoutePackWalkCountContext *context, const RoutePackWalkCoord *left, const RoutePackWalkCoord *right) {
    if (left == 0 || right == 0 || !left->found || !right->found) return 0;
    if (left->tile != 0 && left->tile->graph_segment_count != 0xffffffffU) {
        left->tile->graph_segment_count += 1U;
        context->tile_segment_count += 1ULL;
    }
    if (right->tile != 0 && right->tile != left->tile && right->tile->graph_segment_count != 0xffffffffU) {
        right->tile->graph_segment_count += 1U;
        context->tile_segment_count += 1ULL;
    }
    context->segment_count += 1ULL;
    return 0;
}

static int on_walk_way_count_segments(void *user, const PbfWay *way) {
    RoutePackWalkCountContext *context = (RoutePackWalkCountContext *)user;
    unsigned int index;

    if (context->failed) return -1;
    if (!way_walkable(way->tags, way->tag_count)) return 0;
    for (index = 1U; index < way->ref_count; ++index) {
        RoutePackWalkCoord *left = walk_coord_find(context->coords, way->refs[index - 1U]);
        RoutePackWalkCoord *right = walk_coord_find(context->coords, way->refs[index]);
        if (count_walk_segment(context, left, right) != 0) { context->failed = 1; return -1; }
    }
    return 0;
}

static int on_walk_way_graph(void *user, const PbfWay *way) {
    RoutePackWalkMaterializeContext *context = (RoutePackWalkMaterializeContext *)user;
    unsigned int index;

    if (context->failed) return -1;
    if (!way_walkable(way->tags, way->tag_count)) return 0;
    context->walkable_way_count += 1ULL;
    if (way->ref_count > 1U) context->segment_count += (unsigned long long)(way->ref_count - 1U);
    if (context->segment_count >= context->next_progress_segment_count) {
        rt_write_cstr(1, "walking_materialized_segments: ");
        rt_write_uint(1, context->segment_count);
        rt_write_char(1, '\n');
        context->next_progress_segment_count += 500000ULL;
    }
    for (index = 1U; index < way->ref_count; ++index) {
        RoutePackWalkCoord *left = walk_coord_find(context->coords, way->refs[index - 1U]);
        RoutePackWalkCoord *right = walk_coord_find(context->coords, way->refs[index]);
        if (materialize_walk_segment(context, left, right) != 0) { context->failed = 1; return -1; }
    }
    return 0;
}

static int build_tile_graphs_from_segments(RoutePackTileStore *tiles, const RoutePackWalkCoordStore *coords, RoutePackTileSegmentStore *segments) {
    RoutePackTileGraph *graph_pool;
    RoutePackTileGraphNode *node_pool;
    RoutePackTileGraphNodeSlot *slot_pool;
    RoutePackTileGraphEdge *edge_pool;
    unsigned long long total_node_capacity = 0ULL;
    unsigned long long total_slot_capacity = 0ULL;
    unsigned long long total_edge_capacity = 0ULL;
    unsigned int node_cursor = 0U;
    unsigned int slot_cursor = 0U;
    unsigned int edge_cursor = 0U;
    unsigned int tile_index;

    if (segments->count == 0U) return 0;
    for (tile_index = 0U; tile_index < tiles->count; ++tile_index) {
        unsigned int group_count = tiles->tiles[tile_index].graph_segment_count;
        unsigned int node_capacity;
        unsigned int slot_capacity;
        if (group_count == 0U) continue;
        node_capacity = group_count > 0x7fffffffU ? 0xffffffffU : group_count * 2U;
        slot_capacity = next_power_of_two_u32(node_capacity > 0x3fffffffU ? 0xffffffffU : node_capacity * 4U);
        total_node_capacity += node_capacity;
        total_edge_capacity += (unsigned long long)group_count * 2ULL;
        total_slot_capacity += slot_capacity;
    }
    if (total_node_capacity > 0xffffffffULL || total_edge_capacity > 0xffffffffULL || total_slot_capacity > 0xffffffffULL) return -1;
    graph_pool = (RoutePackTileGraph *)rt_malloc(sizeof(*graph_pool) * tiles->count);
    node_pool = (RoutePackTileGraphNode *)rt_malloc(sizeof(*node_pool) * (unsigned int)total_node_capacity);
    slot_pool = (RoutePackTileGraphNodeSlot *)rt_malloc(sizeof(*slot_pool) * (unsigned int)total_slot_capacity);
    edge_pool = (RoutePackTileGraphEdge *)rt_malloc(sizeof(*edge_pool) * (unsigned int)total_edge_capacity);
    if (graph_pool == 0 || node_pool == 0 || slot_pool == 0 || edge_pool == 0) return -1;
    rt_memset(graph_pool, 0, sizeof(*graph_pool) * tiles->count);
    for (tile_index = 0U; tile_index < tiles->count; ++tile_index) {
        unsigned int group_begin = tiles->tiles[tile_index].graph_segment_offset;
        unsigned int group_count;
        unsigned int group_end;
        RoutePackTileGraph *graph;
        unsigned int node_capacity;
        unsigned int slot_capacity;
        unsigned int index;

        group_count = tiles->tiles[tile_index].graph_segment_count;
        group_end = group_begin + group_count;
        if (group_count == 0U) continue;
        if (group_end > segments->count) return -1;
        node_capacity = group_count > 0x7fffffffU ? 0xffffffffU : group_count * 2U;
        slot_capacity = next_power_of_two_u32(node_capacity > 0x3fffffffU ? 0xffffffffU : node_capacity * 4U);
        graph = graph_pool + tile_index;
        tiles->tiles[tile_index].graph = graph;
        graph->nodes = node_pool + node_cursor;
        graph->node_capacity = node_capacity;
        graph->slots = slot_pool + slot_cursor;
        graph->slot_capacity = slot_capacity;
        graph->edges = edge_pool + edge_cursor;
        graph->edge_capacity = group_count * 2U;
        rt_memset(graph->slots, 0, sizeof(*graph->slots) * slot_capacity);
        node_cursor += node_capacity;
        slot_cursor += slot_capacity;
        edge_cursor += group_count * 2U;
        graph->node_count = 0U;
        graph->edge_count = 0U;
        for (index = group_begin; index < group_end; ++index) {
            RoutePackWalkCoord *left = coords->items + segments->items[index].left_coord_index;
            RoutePackWalkCoord *right = coords->items + segments->items[index].right_coord_index;
            if (tile_graph_add_segment(tiles->tiles + tile_index, left, right) != 0) return -1;
        }
    }
    return 0;
}

static int on_walk_node_coord(void *user, const PbfNode *node) {
    RoutePackWalkCoordStore *coords = (RoutePackWalkCoordStore *)user;
    RoutePackWalkCoord *coord = walk_coord_find(coords, node->id);

    if (coord == 0) return 0;
    if (int32_from_nano_e7(node->lat_nano, &coord->lat_e7) != 0 || int32_from_nano_e7(node->lon_nano, &coord->lon_e7) != 0) return 0;
    coord->found = 1;
    return 0;
}

static int collect_walking_graph(const char *pbf_path, RoutePackTileStore *tiles, const RoutePackBounds *bounds, unsigned int tile_size_m, char *error, size_t error_capacity) {
    RoutePackWalkCoordStore coords;
    RoutePackTileSegmentStore tile_segments;
    RoutePackWalkCollectContext context;
    RoutePackWalkCountContext count_context;
    RoutePackWalkMaterializeContext materialize_context;
    PbfStreamCallbacks callbacks;
    int origin_lat_e7 = 0;
    int origin_lon_e7 = 0;
    unsigned int meters_per_degree_lon;
    unsigned int coord_index;

    rt_memset(&coords, 0, sizeof(coords));
    rt_memset(&tile_segments, 0, sizeof(tile_segments));
    rt_memset(&context, 0, sizeof(context));
    context.coords = &coords;
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.way = on_walk_way_refs;
    rt_write_cstr(1, "walking_graph_phase: collect_refs\n");
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, error_capacity) != 0 || context.failed) return -1;
    rt_write_cstr(1, "walking_ref_nodes: ");
    rt_write_uint(1, coords.count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "walking_ref_total: ");
    rt_write_uint(1, context.ref_count);
    rt_write_char(1, '\n');

    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_walk_node_coord;
    rt_write_cstr(1, "walking_graph_phase: resolve_coords\n");
    if (pbf_stream_entities(pbf_path, &callbacks, &coords, error, error_capacity) != 0) return -1;

    if (bounds->have_bounds) {
        (void)int32_from_nano_e7((bounds->min_lat_nano + bounds->max_lat_nano) / 2LL, &origin_lat_e7);
        (void)int32_from_nano_e7((bounds->min_lon_nano + bounds->max_lon_nano) / 2LL, &origin_lon_e7);
    }
    meters_per_degree_lon = meters_per_degree_lon_from_lat_e7(origin_lat_e7);
    for (coord_index = 0U; coord_index < coords.count; ++coord_index) {
        RoutePackWalkCoord *coord = coords.items + coord_index;
        unsigned long long tile_id;
        if (!coord->found) continue;
        (void)route_pack_coord_to_tile(coord->lat_e7, coord->lon_e7, origin_lat_e7, origin_lon_e7, meters_per_degree_lon, tile_size_m, &coord->tile_x, &coord->tile_y, &tile_id);
        coord->tile = tile_store_find_by_xy(tiles, coord->tile_x, coord->tile_y);
    }
    rt_memset(&count_context, 0, sizeof(count_context));
    count_context.coords = &coords;
    count_context.tiles = tiles;
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.way = on_walk_way_count_segments;
    rt_write_cstr(1, "walking_graph_phase: count_tile_segments\n");
    if (pbf_stream_entities(pbf_path, &callbacks, &count_context, error, error_capacity) != 0 || count_context.failed) return -1;
    rt_write_cstr(1, "walking_count_segments: ");
    rt_write_uint(1, count_context.segment_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "walking_count_tile_segments: ");
    rt_write_uint(1, count_context.tile_segment_count);
    rt_write_char(1, '\n');
    if (count_context.tile_segment_count > 0xffffffffULL) return -1;
    if (tile_segment_reserve(&tile_segments, (unsigned int)count_context.tile_segment_count) != 0) return -1;
    {
        unsigned int tile_index;
        unsigned int offset = 0U;
        for (tile_index = 0U; tile_index < tiles->count; ++tile_index) {
            tiles->tiles[tile_index].graph_segment_offset = offset;
            tiles->tiles[tile_index].graph_segment_cursor = offset;
            offset += tiles->tiles[tile_index].graph_segment_count;
        }
    }
    rt_memset(&materialize_context, 0, sizeof(materialize_context));
    materialize_context.coords = &coords;
    materialize_context.tiles = tiles;
    materialize_context.segments = &tile_segments;
    materialize_context.origin_lat_e7 = origin_lat_e7;
    materialize_context.origin_lon_e7 = origin_lon_e7;
    materialize_context.meters_per_degree_lon = meters_per_degree_lon;
    materialize_context.tile_size_m = tile_size_m;
    materialize_context.next_progress_segment_count = 500000ULL;
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.way = on_walk_way_graph;
    rt_write_cstr(1, "walking_graph_phase: collect_tile_segments\n");
    if (pbf_stream_entities(pbf_path, &callbacks, &materialize_context, error, error_capacity) != 0 || materialize_context.failed) return -1;
    rt_write_cstr(1, "walking_way_count: ");
    rt_write_uint(1, materialize_context.walkable_way_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "walking_segments: ");
    rt_write_uint(1, materialize_context.segment_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "walking_tile_segments: ");
    rt_write_uint(1, materialize_context.inserted_segment_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "walking_graph_phase: build_tile_graphs\n");
    if (build_tile_graphs_from_segments(tiles, &coords, &tile_segments) != 0) return -1;
    rt_free(coords.items);
    rt_free(coords.slots);
    rt_free(tile_segments.items);
    return 0;
}

static int on_node_address(void *user, const PbfNode *node) {
    RoutePackAddressCollectContext *context = (RoutePackAddressCollectContext *)user;
    RoutePackAddressFields fields;
    RoutePackTile *tile;
    int lat_e7;
    int lon_e7;
    int tile_x;
    int tile_y;
    unsigned long long tile_id;

    if (context->failed) return -1;
    find_address_fields(node->tags, node->tag_count, &fields);
    if (!address_fields_complete(&fields)) return 0;
    if (int32_from_nano_e7(node->lat_nano, &lat_e7) != 0 || int32_from_nano_e7(node->lon_nano, &lon_e7) != 0) return 0;
    (void)route_pack_coord_to_tile(lat_e7, lon_e7, context->origin_lat_e7, context->origin_lon_e7, context->meters_per_degree_lon, context->tile_size_m, &tile_x, &tile_y, &tile_id);
    tile = tile_store_find_by_xy(context->tiles, tile_x, tile_y);
    if (tile != 0 && tile->address_count != 0xffffffffU) tile->address_count += 1U;
    if (address_store_add(context->addresses, 1U, node->id, 1, lat_e7, lon_e7, tile_id, &fields, 0) != 0) {
        context->failed = 1;
        return -1;
    }
    return 0;
}

static int on_way_address(void *user, const PbfWay *way) {
    RoutePackAddressCollectContext *context = (RoutePackAddressCollectContext *)user;
    RoutePackAddressFields fields;
    unsigned int address_index = 0U;
    unsigned int ref_index;

    if (context->failed) return -1;
    find_address_fields(way->tags, way->tag_count, &fields);
    if (!address_fields_complete(&fields)) return 0;
    if (address_store_add(context->addresses, 2U, way->id, 0, 0, 0, 0ULL, &fields, &address_index) != 0) {
        context->failed = 1;
        return -1;
    }
    for (ref_index = 0U; ref_index < way->ref_count; ++ref_index) {
        if (address_store_add_node_ref(context->addresses, way->refs[ref_index], address_index) != 0) {
            context->failed = 1;
            return -1;
        }
    }
    return 0;
}

static int on_relation_address(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    RoutePackAddressCollectContext *context = (RoutePackAddressCollectContext *)user;
    RoutePackAddressFields fields;

    if (context->failed) return -1;
    find_address_fields(tags, tag_count, &fields);
    if (address_store_add(context->addresses, 3U, id, 0, 0, 0, 0ULL, &fields, 0) != 0) {
        context->failed = 1;
        return -1;
    }
    return 0;
}

static int compare_address_node_ref_by_id(const void *left_ptr, const void *right_ptr) {
    const RoutePackAddressNodeRef *left = (const RoutePackAddressNodeRef *)left_ptr;
    const RoutePackAddressNodeRef *right = (const RoutePackAddressNodeRef *)right_ptr;

    if (left->node_id < right->node_id) return -1;
    if (left->node_id > right->node_id) return 1;
    if (left->address_index < right->address_index) return -1;
    if (left->address_index > right->address_index) return 1;
    return 0;
}

static unsigned int lower_bound_address_node_ref(const RoutePackAddressStore *store, long long node_id) {
    unsigned int low = 0U;
    unsigned int high = store->node_ref_count;

    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        if (store->node_refs[mid].node_id < node_id) low = mid + 1U;
        else high = mid;
    }
    return low;
}

static int on_node_address_coord(void *user, const PbfNode *node) {
    RoutePackAddressResolveContext *context = (RoutePackAddressResolveContext *)user;
    RoutePackAddressStore *store = context->addresses;
    unsigned int ref_index;

    if (context->failed) return -1;
    ref_index = lower_bound_address_node_ref(store, node->id);
    while (ref_index < store->node_ref_count && store->node_refs[ref_index].node_id == node->id) {
        RoutePackAddressRecord *record = store->records + store->node_refs[ref_index].address_index;
        record->lat_nano_sum += node->lat_nano;
        record->lon_nano_sum += node->lon_nano;
        if (record->coord_ref_count != 0xffffffffU) record->coord_ref_count += 1U;
        ref_index += 1U;
    }
    return 0;
}

static int collect_addresses(const char *pbf_path, RoutePackAddressStore *addresses, RoutePackTileStore *tiles, const RoutePackBounds *bounds, unsigned int tile_size_m, char *error, size_t error_capacity) {
    RoutePackAddressCollectContext context;
    RoutePackAddressResolveContext resolve_context;
    PbfStreamCallbacks callbacks;
    int origin_lat_e7 = 0;
    int origin_lon_e7 = 0;
    unsigned int address_index;

    rt_memset(addresses, 0, sizeof(*addresses));
    if (bounds->have_bounds) {
        (void)int32_from_nano_e7((bounds->min_lat_nano + bounds->max_lat_nano) / 2LL, &origin_lat_e7);
        (void)int32_from_nano_e7((bounds->min_lon_nano + bounds->max_lon_nano) / 2LL, &origin_lon_e7);
    }
    rt_memset(&context, 0, sizeof(context));
    context.addresses = addresses;
    context.tiles = tiles;
    context.origin_lat_e7 = origin_lat_e7;
    context.origin_lon_e7 = origin_lon_e7;
    context.tile_size_m = tile_size_m;
    context.meters_per_degree_lon = meters_per_degree_lon_from_lat_e7(origin_lat_e7);
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_node_address;
    callbacks.way = on_way_address;
    callbacks.relation_tags = on_relation_address;
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, error_capacity) != 0) return -1;
    if (context.failed) return -1;

    if (addresses->node_ref_count != 0U) {
        rt_sort(addresses->node_refs, addresses->node_ref_count, sizeof(*addresses->node_refs), compare_address_node_ref_by_id);
        rt_memset(&resolve_context, 0, sizeof(resolve_context));
        resolve_context.addresses = addresses;
        rt_memset(&callbacks, 0, sizeof(callbacks));
        callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
        callbacks.node = on_node_address_coord;
        if (pbf_stream_entities(pbf_path, &callbacks, &resolve_context, error, error_capacity) != 0) return -1;
        if (resolve_context.failed) return -1;
    }

    for (address_index = 0U; address_index < addresses->count; ++address_index) {
        RoutePackAddressRecord *record = addresses->records + address_index;
        if ((record->flags & 1U) == 0U && record->coord_ref_count != 0U) {
            int lat_e7;
            int lon_e7;
            int tile_x;
            int tile_y;
            RoutePackTile *tile;
            long long lat_nano = record->lat_nano_sum / (long long)record->coord_ref_count;
            long long lon_nano = record->lon_nano_sum / (long long)record->coord_ref_count;
            if (int32_from_nano_e7(lat_nano, &lat_e7) == 0 && int32_from_nano_e7(lon_nano, &lon_e7) == 0) {
                record->lat_e7 = lat_e7;
                record->lon_e7 = lon_e7;
                record->flags |= 1U;
                (void)route_pack_coord_to_tile(lat_e7, lon_e7, origin_lat_e7, origin_lon_e7, meters_per_degree_lon_from_lat_e7(origin_lat_e7), tile_size_m, &tile_x, &tile_y, &record->tile_id);
                tile = tile_store_find_by_xy(tiles, tile_x, tile_y);
                if (tile != 0 && tile->address_count != 0xffffffffU) tile->address_count += 1U;
            }
        }
    }
    return 0;
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

static int write_zero_padding(int fd, unsigned long long size) {
    unsigned char zeros[64];

    rt_memset(zeros, 0, sizeof(zeros));
    while (size != 0ULL) {
        size_t chunk = size > sizeof(zeros) ? sizeof(zeros) : (size_t)size;
        if (rt_write_all(fd, zeros, chunk) != 0) return -1;
        size -= (unsigned long long)chunk;
    }
    return 0;
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

static unsigned int tile_graph_node_count(const RoutePackTile *tile) {
    RoutePackTileGraph *graph = (RoutePackTileGraph *)tile->graph;
    return graph == 0 ? 0U : graph->node_count;
}

static unsigned int tile_graph_edge_count(const RoutePackTile *tile) {
    RoutePackTileGraph *graph = (RoutePackTileGraph *)tile->graph;
    return graph == 0 ? 0U : graph->edge_count;
}

static unsigned long long tile_payload_size_for_counts(unsigned int node_count, unsigned int edge_count) {
    unsigned long long offset = OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 4ULL * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE;
    offset += (unsigned long long)node_count * 16ULL;
    offset = align_u64(offset, 8ULL);
    offset += (unsigned long long)(node_count + 1U) * 4ULL;
    offset = align_u64(offset, 8ULL);
    offset += (unsigned long long)edge_count * 20ULL;
    offset = align_u64(offset, 8ULL);
    offset += 64ULL;
    return align_u64(offset, 64ULL);
}

static int write_tile_payload(int fd, const RoutePackTile *tile) {
    RoutePackTileGraph *graph = (RoutePackTileGraph *)tile->graph;
    unsigned int node_count = graph == 0 ? 0U : graph->node_count;
    unsigned int edge_count = graph == 0 ? 0U : graph->edge_count;
    unsigned long long nodes_offset = OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 4ULL * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE;
    unsigned long long nodes_size = (unsigned long long)node_count * 16ULL;
    unsigned long long offsets_offset = align_u64(nodes_offset + nodes_size, 8ULL);
    unsigned long long offsets_size = (unsigned long long)(node_count + 1U) * 4ULL;
    unsigned long long edges_offset = align_u64(offsets_offset + offsets_size, 8ULL);
    unsigned long long edges_size = (unsigned long long)edge_count * 20ULL;
    unsigned long long snap_offset = align_u64(edges_offset + edges_size, 8ULL);
    unsigned long long payload_size = tile_payload_size_for_counts(node_count, edge_count);
    unsigned char header[OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 4U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE];
    unsigned char node_record[16];
    unsigned char edge_record[20];
    unsigned char snap_header[64];
    unsigned int *counts = 0;
    unsigned int *offsets = 0;
    unsigned int *cursor = 0;
    RoutePackTileGraphEdge *ordered_edges = 0;
    unsigned int index;
    unsigned long long current_offset;

    rt_memset(header, 0, sizeof(header));
    write_u64_le(header + 0U, tile->tile_id);
    write_u32_le(header + 8U, 1U);
    write_u32_le(header + 16U, 4U);
    write_u32_le(header + 20U, OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE);
    write_u64_le(header + 24U, OSMRTE_TILE_PAYLOAD_HEADER_SIZE);
    write_tile_payload_directory_record(header + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 0U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_NODES, nodes_offset, nodes_size, node_count, 16U);
    write_tile_payload_directory_record(header + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 1U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_OFFSETS, offsets_offset, offsets_size, node_count + 1U, 4U);
    write_tile_payload_directory_record(header + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 2U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_WALKING_EDGES, edges_offset, edges_size, edge_count, 20U);
    write_tile_payload_directory_record(header + OSMRTE_TILE_PAYLOAD_HEADER_SIZE + 3U * OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE, OSMRTE_TILE_TYPE_SNAP_GRID, snap_offset, 64ULL, 1U, 64U);
    if (rt_write_all(fd, header, sizeof(header)) != 0) return -1;
    current_offset = sizeof(header);

    if (node_count != 0U) {
        counts = (unsigned int *)rt_malloc(sizeof(unsigned int) * (node_count + 1U));
        offsets = (unsigned int *)rt_malloc(sizeof(unsigned int) * (node_count + 1U));
        cursor = (unsigned int *)rt_malloc(sizeof(unsigned int) * (node_count + 1U));
        ordered_edges = (RoutePackTileGraphEdge *)rt_malloc(sizeof(*ordered_edges) * edge_count);
        if (counts == 0 || offsets == 0 || cursor == 0 || (edge_count != 0U && ordered_edges == 0)) return -1;
        rt_memset(counts, 0, sizeof(unsigned int) * (node_count + 1U));
        for (index = 0U; index < edge_count; ++index) if (graph->edges[index].from < node_count) counts[graph->edges[index].from] += 1U;
        offsets[0] = 0U;
        for (index = 0U; index < node_count; ++index) offsets[index + 1U] = offsets[index] + counts[index];
        memcpy(cursor, offsets, sizeof(unsigned int) * (node_count + 1U));
        for (index = 0U; index < edge_count; ++index) {
            unsigned int from = graph->edges[index].from;
            if (from < node_count) ordered_edges[cursor[from]++] = graph->edges[index];
        }
    }

    for (index = 0U; index < node_count; ++index) {
        rt_memset(node_record, 0, sizeof(node_record));
        write_i32_le(node_record + 0U, graph->nodes[index].lat_e7);
        write_i32_le(node_record + 4U, graph->nodes[index].lon_e7);
        write_u32_le(node_record + 8U, 0xffffffffU);
        if (rt_write_all(fd, node_record, sizeof(node_record)) != 0) return -1;
    }
    current_offset += nodes_size;
    if (current_offset < offsets_offset && write_zero_padding(fd, offsets_offset - current_offset) != 0) return -1;
    current_offset = offsets_offset;
    if (node_count == 0U) {
        unsigned char zero_offset[4];
        rt_memset(zero_offset, 0, sizeof(zero_offset));
        if (rt_write_all(fd, zero_offset, sizeof(zero_offset)) != 0) return -1;
    } else {
        for (index = 0U; index <= node_count; ++index) {
            unsigned char offset_record[4];
            write_u32_le(offset_record, offsets[index]);
            if (rt_write_all(fd, offset_record, sizeof(offset_record)) != 0) return -1;
        }
    }
    current_offset += offsets_size;
    if (current_offset < edges_offset && write_zero_padding(fd, edges_offset - current_offset) != 0) return -1;
    current_offset = edges_offset;
    for (index = 0U; index < edge_count; ++index) {
        rt_memset(edge_record, 0, sizeof(edge_record));
        write_u32_le(edge_record + 0U, ordered_edges[index].to);
        write_u32_le(edge_record + 4U, ordered_edges[index].meters);
        write_u32_le(edge_record + 8U, ordered_edges[index].meters);
        write_u32_le(edge_record + 12U, 0U);
        write_u32_le(edge_record + 16U, 0U);
        if (rt_write_all(fd, edge_record, sizeof(edge_record)) != 0) return -1;
    }
    current_offset += edges_size;
    if (current_offset < snap_offset && write_zero_padding(fd, snap_offset - current_offset) != 0) return -1;
    current_offset = snap_offset;
    rt_memset(snap_header, 0, sizeof(snap_header));
    write_u32_le(snap_header + 0U, 1U);
    write_u32_le(snap_header + 4U, 0U);
    write_u32_le(snap_header + 8U, 1U);
    write_u32_le(snap_header + 12U, 1U);
    write_u32_le(snap_header + 16U, 0U);
    write_u32_le(snap_header + 20U, 0U);
    if (rt_write_all(fd, snap_header, sizeof(snap_header)) != 0) return -1;
    current_offset += sizeof(snap_header);
    if (current_offset < payload_size && write_zero_padding(fd, payload_size - current_offset) != 0) return -1;
    rt_free(counts);
    rt_free(offsets);
    rt_free(cursor);
    rt_free(ordered_edges);
    return 0;
}

static void write_tile_record(unsigned char out[OSMRTE_TILE_RECORD_SIZE], const RoutePackTile *tile) {
    rt_memset(out, 0, OSMRTE_TILE_RECORD_SIZE);
    write_u64_le(out + 0U, tile->tile_id);
    write_u32_le(out + 8U, 0U);
    write_i32_le(out + 12U, tile->x);
    write_i32_le(out + 16U, tile->y);
    write_u32_le(out + 20U, tile_graph_node_count(tile));
    write_u32_le(out + 24U, tile_graph_edge_count(tile));
    write_u32_le(out + 28U, 1U);
    write_u32_le(out + 32U, 0U);
    write_u32_le(out + 36U, 0U);
    write_u32_le(out + 40U, tile->address_count);
    write_u32_le(out + 44U, 0U);
    write_i32_le(out + 48U, tile->min_lon_e7);
    write_i32_le(out + 52U, tile->min_lat_e7);
    write_i32_le(out + 56U, tile->max_lon_e7);
    write_i32_le(out + 60U, tile->max_lat_e7);
    write_u64_le(out + 64U, tile->payload_offset);
    write_u64_le(out + 72U, tile->payload_size);
    write_u64_le(out + 80U, tile->payload_offset + OSMRTE_TILE_PAYLOAD_HEADER_SIZE);
    write_u32_le(out + 88U, 4U);
    write_u32_le(out + 92U, OSMRTE_TILE_PAYLOAD_DIRECTORY_RECORD_SIZE);
    write_u64_le(out + 96U, tile->neighbor_mask);
}

static void write_address_record(unsigned char out[OSMRTE_ADDRESS_RECORD_SIZE], const RoutePackAddressRecord *record) {
    rt_memset(out, 0, OSMRTE_ADDRESS_RECORD_SIZE);
    write_u32_le(out + 0U, record->entity_type);
    write_u32_le(out + 4U, record->flags);
    write_i64_le(out + 8U, record->id);
    write_i32_le(out + 16U, record->lat_e7);
    write_i32_le(out + 20U, record->lon_e7);
    write_u64_le(out + 24U, record->tile_id);
    write_u32_le(out + 32U, record->state_offset);
    write_u32_le(out + 36U, record->state_size);
    write_u32_le(out + 40U, record->city_offset);
    write_u32_le(out + 44U, record->city_size);
    write_u32_le(out + 48U, record->suburb_offset);
    write_u32_le(out + 52U, record->suburb_size);
    write_u32_le(out + 56U, record->street_offset);
    write_u32_le(out + 60U, record->street_size);
    write_u32_le(out + 64U, record->housenumber_offset);
    write_u32_le(out + 68U, record->housenumber_size);
    write_u32_le(out + 72U, record->postcode_offset);
    write_u32_le(out + 76U, record->postcode_size);
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
    const RoutePackAddressStore *address_store,
    unsigned int tile_size_m
) {
    unsigned char header[OSMRTE_HEADER_SIZE];
    unsigned char section_records[OSMRTE_SECTION_RECORD_SIZE * 3U];
    unsigned char tile_record[OSMRTE_TILE_RECORD_SIZE];
    unsigned char address_header[OSMRTE_ADDRESS_SECTION_HEADER_SIZE];
    unsigned char address_record[OSMRTE_ADDRESS_RECORD_SIZE];
    unsigned char empty_string = 0;
    unsigned long long section_directory_offset = OSMRTE_HEADER_SIZE;
    unsigned int have_addresses = address_store->count != 0U ? 1U : 0U;
    unsigned int section_count = have_addresses ? 3U : 2U;
    unsigned long long after_section_directory = section_directory_offset + (unsigned long long)section_count * OSMRTE_SECTION_RECORD_SIZE;
    unsigned long long tile_directory_offset = align_u64(after_section_directory, 64ULL);
    unsigned long long tile_directory_size = (unsigned long long)tile_store->count * OSMRTE_TILE_RECORD_SIZE;
    unsigned long long tile_payload_offset = align_u64(tile_directory_offset + tile_directory_size, 64ULL);
    unsigned long long address_section_offset;
    unsigned long long address_records_size = (unsigned long long)address_store->count * OSMRTE_ADDRESS_RECORD_SIZE;
    unsigned long long address_section_size = have_addresses ? OSMRTE_ADDRESS_SECTION_HEADER_SIZE + address_records_size + (unsigned long long)address_store->string_size : 0ULL;
    unsigned long long string_table_offset;
    unsigned long long string_table_size = 1ULL;
    unsigned long long file_size;
    unsigned long long current_offset;
    int output_fd;
    unsigned int tile_index;
    unsigned int address_index;

    for (tile_index = 0U; tile_index < tile_store->count; ++tile_index) {
        tile_store->tiles[tile_index].payload_size = tile_payload_size_for_counts(tile_graph_node_count(tile_store->tiles + tile_index), tile_graph_edge_count(tile_store->tiles + tile_index));
        tile_store->tiles[tile_index].payload_offset = tile_payload_offset;
        tile_payload_offset += tile_store->tiles[tile_index].payload_size;
        tile_store->tiles[tile_index].neighbor_mask = compute_neighbor_mask(tile_store, tile_store->tiles + tile_index);
    }
    address_section_offset = align_u64(tile_payload_offset, 64ULL);
    string_table_offset = align_u64(address_section_offset + address_section_size, 64ULL);
    file_size = string_table_offset + string_table_size;
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
    if (have_addresses) {
        write_section_record(
            section_records + 2U * OSMRTE_SECTION_RECORD_SIZE,
            OSMRTE_SECTION_ADDRESS_DICTIONARIES,
            OSMRTE_SECTION_FLAG_GLOBAL_PAYLOAD,
            address_section_offset,
            address_section_size,
            address_store->count,
            OSMRTE_ADDRESS_RECORD_SIZE
        );
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) return -1;
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 ||
        rt_write_all(output_fd, section_records, (size_t)section_count * OSMRTE_SECTION_RECORD_SIZE) != 0) {
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
        if (write_tile_payload(output_fd, tile_store->tiles + tile_index) != 0) {
            (void)platform_close(output_fd);
            return -1;
        }
    }
    current_offset = tile_payload_offset;
    if (have_addresses) {
        rt_memset(address_header, 0, sizeof(address_header));
        memcpy(address_header, "ADDRIDX1", 8U);
        write_u32_le(address_header + 8U, 1U);
        write_u32_le(address_header + 12U, OSMRTE_ADDRESS_SECTION_HEADER_SIZE);
        write_u64_le(address_header + 16U, address_store->count);
        write_u32_le(address_header + 24U, OSMRTE_ADDRESS_RECORD_SIZE);
        write_u32_le(address_header + 28U, address_store->string_size);
        write_u64_le(address_header + 32U, OSMRTE_ADDRESS_SECTION_HEADER_SIZE + address_records_size);
        if (rt_write_all(output_fd, address_header, sizeof(address_header)) != 0) {
            (void)platform_close(output_fd);
            return -1;
        }
        for (address_index = 0U; address_index < address_store->count; ++address_index) {
            write_address_record(address_record, address_store->records + address_index);
            if (rt_write_all(output_fd, address_record, sizeof(address_record)) != 0) {
                (void)platform_close(output_fd);
                return -1;
            }
        }
        if (address_store->string_size != 0U && rt_write_all(output_fd, address_store->strings, address_store->string_size) != 0) {
            (void)platform_close(output_fd);
            return -1;
        }
        current_offset = address_section_offset + address_section_size;
    }
    if (current_offset < string_table_offset && write_zero_padding(output_fd, string_table_offset - current_offset) != 0) {
        (void)platform_close(output_fd);
        return -1;
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
    RoutePackAddressStore address_store;
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
    error[0] = '\0';
    if (collect_walking_graph(pbf_path, &tile_store, &bounds, tile_size_m, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmroutepack: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to collect walking graph" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    error[0] = '\0';
    if (collect_addresses(pbf_path, &address_store, &tile_store, &bounds, tile_size_m, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmroutepack: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to collect addresses" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    if (write_route_pack(output_path, &summary, &bounds, &tile_store, &address_store, tile_size_m) != 0) {
        rt_write_cstr(2, "osmroutepack: failed to write output route pack\n");
        return 1;
    }

    rt_write_cstr(1, "format: OSMRTE01\n");
    rt_write_cstr(1, "mode: tiled-walking-graph-addresses\n");
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
    rt_write_cstr(1, "addresses: ");
    rt_write_uint(1, address_store.count);
    rt_write_char(1, '\n');
    {
        unsigned int tile_index;
        unsigned long long graph_nodes = 0ULL;
        unsigned long long graph_edges = 0ULL;
        for (tile_index = 0U; tile_index < tile_store.count; ++tile_index) {
            graph_nodes += tile_graph_node_count(tile_store.tiles + tile_index);
            graph_edges += tile_graph_edge_count(tile_store.tiles + tile_index);
        }
        rt_write_cstr(1, "walking_nodes: ");
        rt_write_uint(1, graph_nodes);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "walking_directed_edges: ");
        rt_write_uint(1, graph_edges);
        rt_write_char(1, '\n');
    }
    rt_write_cstr(1, "tile_size_m: ");
    rt_write_uint(1, tile_size_m);
    rt_write_char(1, '\n');
    return 0;
}
