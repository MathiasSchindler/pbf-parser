#define main osmrenderpack_v1_main
#include "osmrenderpack.c"
#undef main

#define OSMRPACK_V2_HEADER_SIZE 256U
#define OSMRPACK_V2_PLACE_RECORD_SIZE 112U
#define OSMRPACK_V2_TILE_RECORD_SIZE 96U
#define OSMRPACK_V2_VERSION 2U
#define OSMRPACK_V2_DEFAULT_TILE_HALO 1U
#define OSMRPACK_V2_PLACE_KIND_UNKNOWN 0U
#define OSMRPACK_V2_PLACE_KIND_CITY 1U
#define OSMRPACK_V2_PLACE_KIND_TOWN 2U
#define OSMRPACK_V2_PLACE_KIND_VILLAGE 3U
#define OSMRPACK_V2_PLACE_KIND_SUBURB 4U
#define OSMRPACK_V2_PLACE_KIND_DISTRICT 5U
#define OSMRPACK_V2_PLACE_KIND_STATE 6U
#define OSMRPACK_V2_PLACE_KIND_COUNTRY 7U
#define OSMRPACK_V2_SOURCE_RELATION 1U

static const unsigned char osmrpack_v2_magic[8] = { 'O', 'S', 'M', 'R', 'P', 'K', '0', '2' };

typedef struct {
    long long id;
    unsigned int source_type;
    unsigned int place_kind;
    unsigned int admin_level;
    unsigned int rank_score;
    unsigned int flags;
    unsigned long long name_offset;
    unsigned int name_size;
    unsigned int member_offset;
    unsigned int member_count;
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    int have_bbox;
} V2Place;

typedef struct {
    long long id;
    unsigned int ref_offset;
    unsigned int ref_count;
} V2PlaceWay;

typedef struct {
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    int missing;
} V2FeatureInfo;

typedef struct {
    unsigned long long tile_id;
    unsigned int x;
    unsigned int y;
    unsigned int feature_index;
} V2TileAssignment;

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
} V2TileRecord;

typedef struct {
    PackBuildContext pack;
    V2Place *places;
    long long *place_members;
    long long *needed_place_way_ids;
    V2PlaceWay *place_ways;
    char *strings;
    V2FeatureInfo *feature_infos;
    V2TileAssignment *assignments;
    V2TileRecord *tile_records;
    unsigned int place_count;
    unsigned int place_capacity;
    unsigned int place_member_count;
    unsigned int place_member_capacity;
    unsigned int needed_place_way_count;
    unsigned int needed_place_way_capacity;
    unsigned int place_way_count;
    unsigned int place_way_capacity;
    unsigned int string_size;
    unsigned int string_capacity;
    unsigned int assignment_count;
    unsigned int assignment_capacity;
    unsigned int tile_record_count;
    unsigned int tile_record_capacity;
    unsigned long long source_relations;
    unsigned long long place_relations;
    unsigned long long place_relations_with_bbox;
    unsigned long long duplicated_tile_features;
    unsigned int tile_zoom;
    unsigned int tile_halo;
    int failed;
} V2BuildContext;

static void v2_write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--tile-zoom N] [--tile-halo N] [--buildings] FILE.osm.pbf OUT.rpack\n");
}

static int v2_grow_places(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->place_capacity == 0U ? 256U : context->place_capacity;
    V2Place *places;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->place_capacity) return 0;
    places = (V2Place *)rt_realloc(context->places, sizeof(*places) * (size_t)capacity);
    if (places == 0) return -1;
    context->places = places;
    context->place_capacity = capacity;
    return 0;
}

static int v2_grow_place_members(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->place_member_capacity == 0U ? 4096U : context->place_member_capacity;
    long long *members;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->place_member_capacity) return 0;
    members = (long long *)rt_realloc(context->place_members, sizeof(*members) * (size_t)capacity);
    if (members == 0) return -1;
    context->place_members = members;
    context->place_member_capacity = capacity;
    return 0;
}

static int v2_grow_needed_place_way_ids(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->needed_place_way_capacity == 0U ? 4096U : context->needed_place_way_capacity;
    long long *ids;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->needed_place_way_capacity) return 0;
    ids = (long long *)rt_realloc(context->needed_place_way_ids, sizeof(*ids) * (size_t)capacity);
    if (ids == 0) return -1;
    context->needed_place_way_ids = ids;
    context->needed_place_way_capacity = capacity;
    return 0;
}

static int v2_grow_place_ways(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->place_way_capacity == 0U ? 4096U : context->place_way_capacity;
    V2PlaceWay *ways;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->place_way_capacity) return 0;
    ways = (V2PlaceWay *)rt_realloc(context->place_ways, sizeof(*ways) * (size_t)capacity);
    if (ways == 0) return -1;
    context->place_ways = ways;
    context->place_way_capacity = capacity;
    return 0;
}

static int v2_grow_strings(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->string_capacity == 0U ? 4096U : context->string_capacity;
    char *strings;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->string_capacity) return 0;
    strings = (char *)rt_realloc(context->strings, (size_t)capacity);
    if (strings == 0) return -1;
    context->strings = strings;
    context->string_capacity = capacity;
    return 0;
}

static int v2_grow_assignments(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->assignment_capacity == 0U ? 65536U : context->assignment_capacity;
    V2TileAssignment *assignments;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->assignment_capacity) return 0;
    assignments = (V2TileAssignment *)rt_realloc(context->assignments, sizeof(*assignments) * (size_t)capacity);
    if (assignments == 0) return -1;
    context->assignments = assignments;
    context->assignment_capacity = capacity;
    return 0;
}

static int v2_grow_tile_records(V2BuildContext *context, unsigned int needed) {
    unsigned int capacity = context->tile_record_capacity == 0U ? 1024U : context->tile_record_capacity;
    V2TileRecord *records;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == context->tile_record_capacity) return 0;
    records = (V2TileRecord *)rt_realloc(context->tile_records, sizeof(*records) * (size_t)capacity);
    if (records == 0) return -1;
    context->tile_records = records;
    context->tile_record_capacity = capacity;
    return 0;
}

static int v2_append_string(V2BuildContext *context, const char *data, size_t size, unsigned long long *offset_out, unsigned int *size_out) {
    if (size > 0xffffffffU || context->string_size > 0xffffffffU - (unsigned int)size) return -1;
    if (v2_grow_strings(context, context->string_size + (unsigned int)size) != 0) return -1;
    *offset_out = (unsigned long long)context->string_size;
    *size_out = (unsigned int)size;
    memcpy(context->strings + context->string_size, data, size);
    context->string_size += (unsigned int)size;
    return 0;
}

static unsigned int v2_parse_admin_level(const PbfText *value) {
    unsigned int level = 0U;
    size_t index;
    if (value == 0 || value->size == 0U) return 0U;
    for (index = 0U; index < value->size; ++index) {
        if (value->data[index] < '0' || value->data[index] > '9') return 0U;
        level = level * 10U + (unsigned int)(value->data[index] - '0');
        if (level > 20U) return 0U;
    }
    return level;
}

static unsigned int v2_place_kind_from_tags(const PbfTag *tags, unsigned int tag_count, unsigned int admin_level) {
    const PbfText *place = find_tag_value(tags, tag_count, "place");
    if (tag_value_equals(place, "city")) return OSMRPACK_V2_PLACE_KIND_CITY;
    if (tag_value_equals(place, "town")) return OSMRPACK_V2_PLACE_KIND_TOWN;
    if (tag_value_equals(place, "village")) return OSMRPACK_V2_PLACE_KIND_VILLAGE;
    if (tag_value_equals(place, "suburb") || tag_value_equals(place, "quarter") || tag_value_equals(place, "neighbourhood")) return OSMRPACK_V2_PLACE_KIND_SUBURB;
    if (admin_level == 2U) return OSMRPACK_V2_PLACE_KIND_COUNTRY;
    if (admin_level == 4U) return OSMRPACK_V2_PLACE_KIND_STATE;
    if (admin_level >= 5U && admin_level <= 10U) return OSMRPACK_V2_PLACE_KIND_DISTRICT;
    return OSMRPACK_V2_PLACE_KIND_UNKNOWN;
}

static unsigned int v2_rank_score(unsigned int kind, unsigned int admin_level, unsigned int way_members) {
    unsigned int score = 0U;
    if (kind == OSMRPACK_V2_PLACE_KIND_CITY) score += 100000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_TOWN) score += 90000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_STATE) score += 80000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_DISTRICT) score += 70000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_VILLAGE) score += 60000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_SUBURB) score += 50000U;
    else if (kind == OSMRPACK_V2_PLACE_KIND_COUNTRY) score += 40000U;
    if (admin_level != 0U && admin_level < 20U) score += (20U - admin_level) * 1000U;
    score += way_members > 999U ? 999U : way_members;
    return score;
}

static int v2_relation_is_place(const PbfTag *tags, unsigned int tag_count, unsigned int *kind_out, unsigned int *admin_level_out) {
    const PbfText *name = find_tag_value(tags, tag_count, "name");
    const PbfText *boundary = find_tag_value(tags, tag_count, "boundary");
    const PbfText *type = find_tag_value(tags, tag_count, "type");
    const PbfText *place = find_tag_value(tags, tag_count, "place");
    unsigned int admin_level = v2_parse_admin_level(find_tag_value(tags, tag_count, "admin_level"));
    unsigned int kind = v2_place_kind_from_tags(tags, tag_count, admin_level);
    if (name == 0 || name->size == 0U) return 0;
    if (tag_value_equals(boundary, "administrative")) {
        if (type != 0 && !tag_value_equals(type, "boundary") && !tag_value_equals(type, "multipolygon")) return 0;
        if (admin_level == 0U || admin_level > 10U) return 0;
        *kind_out = kind;
        *admin_level_out = admin_level;
        return 1;
    }
    if (place != 0 && kind != OSMRPACK_V2_PLACE_KIND_UNKNOWN) {
        *kind_out = kind;
        *admin_level_out = admin_level;
        return 1;
    }
    return 0;
}

static int v2_on_relation_tags(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    V2BuildContext *context = (V2BuildContext *)user;
    unsigned int kind;
    unsigned int admin_level;
    (void)id;
    context->source_relations += 1ULL;
    return v2_relation_is_place(tags, tag_count, &kind, &admin_level);
}

static int v2_on_relation(void *user, const PbfRelation *relation) {
    V2BuildContext *context = (V2BuildContext *)user;
    const PbfText *name = find_tag_value(relation->tags, relation->tag_count, "name");
    unsigned int kind;
    unsigned int admin_level;
    unsigned int member_index;
    unsigned int way_member_count = 0U;
    V2Place *place;

    if (!v2_relation_is_place(relation->tags, relation->tag_count, &kind, &admin_level)) return 0;
    for (member_index = 0U; member_index < relation->member_count; ++member_index) {
        if (relation->members[member_index].type == PBF_RELATION_MEMBER_WAY) way_member_count += 1U;
    }
    if (way_member_count == 0U) return 0;
    if (v2_grow_places(context, context->place_count + 1U) != 0 ||
        v2_grow_place_members(context, context->place_member_count + way_member_count) != 0 ||
        v2_grow_needed_place_way_ids(context, context->needed_place_way_count + way_member_count) != 0) {
        context->failed = 1;
        return -1;
    }
    place = &context->places[context->place_count];
    rt_memset(place, 0, sizeof(*place));
    place->id = relation->id;
    place->source_type = OSMRPACK_V2_SOURCE_RELATION;
    place->place_kind = kind;
    place->admin_level = admin_level;
    place->rank_score = v2_rank_score(kind, admin_level, way_member_count);
    place->member_offset = context->place_member_count;
    place->member_count = way_member_count;
    if (v2_append_string(context, name->data, name->size, &place->name_offset, &place->name_size) != 0) {
        context->failed = 1;
        return -1;
    }
    for (member_index = 0U; member_index < relation->member_count; ++member_index) {
        if (relation->members[member_index].type == PBF_RELATION_MEMBER_WAY) {
            long long id = relation->members[member_index].id;
            context->place_members[context->place_member_count++] = id;
            context->needed_place_way_ids[context->needed_place_way_count++] = id;
        }
    }
    context->place_count += 1U;
    context->place_relations += 1ULL;
    return 0;
}

static int compare_i64(const void *left_ptr, const void *right_ptr) {
    long long left = *(const long long *)left_ptr;
    long long right = *(const long long *)right_ptr;
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

static int compare_place_way(const void *left_ptr, const void *right_ptr) {
    const V2PlaceWay *left = (const V2PlaceWay *)left_ptr;
    const V2PlaceWay *right = (const V2PlaceWay *)right_ptr;
    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    return 0;
}

static int compare_assignment(const void *left_ptr, const void *right_ptr) {
    const V2TileAssignment *left = (const V2TileAssignment *)left_ptr;
    const V2TileAssignment *right = (const V2TileAssignment *)right_ptr;
    if (left->tile_id < right->tile_id) return -1;
    if (left->tile_id > right->tile_id) return 1;
    if (left->feature_index < right->feature_index) return -1;
    if (left->feature_index > right->feature_index) return 1;
    return 0;
}

static int v2_prepare_needed_place_way_ids(V2BuildContext *context) {
    unsigned int read_index;
    unsigned int write_index = 0U;
    if (context->needed_place_way_count == 0U) return 0;
    rt_sort(context->needed_place_way_ids, context->needed_place_way_count, sizeof(long long), compare_i64);
    for (read_index = 0U; read_index < context->needed_place_way_count; ++read_index) {
        if (write_index == 0U || context->needed_place_way_ids[read_index] != context->needed_place_way_ids[write_index - 1U]) {
            context->needed_place_way_ids[write_index++] = context->needed_place_way_ids[read_index];
        }
    }
    context->needed_place_way_count = write_index;
    return 0;
}

static int v2_needed_place_way_contains(V2BuildContext *context, long long id) {
    unsigned int low = 0U;
    unsigned int high = context->needed_place_way_count;
    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        long long mid_id = context->needed_place_way_ids[mid];
        if (mid_id == id) return 1;
        if (mid_id < id) low = mid + 1U;
        else high = mid;
    }
    return 0;
}

static V2PlaceWay *v2_find_place_way(V2BuildContext *context, long long id) {
    unsigned int low = 0U;
    unsigned int high = context->place_way_count;
    while (low < high) {
        unsigned int mid = low + (high - low) / 2U;
        long long mid_id = context->place_ways[mid].id;
        if (mid_id == id) return &context->place_ways[mid];
        if (mid_id < id) low = mid + 1U;
        else high = mid;
    }
    return 0;
}

static unsigned int v2_i64_sort_bucket(long long value, unsigned int shift) {
    unsigned long long key = ((unsigned long long)value) ^ 0x8000000000000000ULL;
    return (unsigned int)((key >> shift) & 0xffULL);
}

static int v2_radix_sort_i64(long long *items, unsigned int count) {
    long long *temp;
    long long *source;
    long long *dest;
    unsigned int pass;

    if (items == 0 || count < 2U) return 0;
    temp = (long long *)rt_malloc(sizeof(*temp) * (size_t)count);
    if (temp == 0) {
        rt_sort(items, count, sizeof(*items), compare_i64);
        return 0;
    }
    source = items;
    dest = temp;
    for (pass = 0U; pass < 8U; ++pass) {
        size_t counts[256];
        size_t offsets[256];
        size_t sum = 0U;
        unsigned int index;
        unsigned int bucket;
        unsigned int shift = pass * 8U;

        rt_memset(counts, 0, sizeof(counts));
        for (index = 0U; index < count; ++index) counts[v2_i64_sort_bucket(source[index], shift)] += 1U;
        for (bucket = 0U; bucket < 256U; ++bucket) {
            offsets[bucket] = sum;
            sum += counts[bucket];
        }
        for (index = 0U; index < count; ++index) {
            bucket = v2_i64_sort_bucket(source[index], shift);
            dest[offsets[bucket]++] = source[index];
        }
        {
            long long *swap = source;
            source = dest;
            dest = swap;
        }
    }
    if (source != items) {
        unsigned int index;
        for (index = 0U; index < count; ++index) items[index] = source[index];
    }
    rt_free(temp);
    return 0;
}

static int v2_build_unique_nodes(PackBuildContext *context) {
    long long *ids;
    unsigned int ref_index;
    unsigned int unique_count = 0U;
    unsigned int write_index = 0U;

    ids = (long long *)rt_malloc(sizeof(*ids) * (size_t)context->ref_count);
    if (ids == 0 && context->ref_count != 0U) return -1;
    for (ref_index = 0U; ref_index < context->ref_count; ++ref_index) ids[ref_index] = context->refs[ref_index];
    if (v2_radix_sort_i64(ids, context->ref_count) != 0) {
        rt_free(ids);
        return -1;
    }
    for (ref_index = 0U; ref_index < context->ref_count; ++ref_index) {
        if (unique_count == 0U || ids[ref_index] != ids[ref_index - 1U]) unique_count += 1U;
    }
    context->nodes = (PackBuildNode *)rt_malloc(sizeof(*context->nodes) * (size_t)unique_count);
    if (context->nodes == 0 && unique_count != 0U) {
        rt_free(ids);
        return -1;
    }
    for (ref_index = 0U; ref_index < context->ref_count; ++ref_index) {
        if (write_index == 0U || ids[ref_index] != context->nodes[write_index - 1U].id) {
            context->nodes[write_index].id = ids[ref_index];
            context->nodes[write_index].lon_nano = 0;
            context->nodes[write_index].lat_nano = 0;
            context->nodes[write_index].found = 0;
            write_index += 1U;
        }
    }
    rt_free(ids);
    context->unique_node_count = unique_count;
    return 0;
}

static int v2_on_way(void *user, const PbfWay *way) {
    V2BuildContext *context = (V2BuildContext *)user;
    int result;
    unsigned int ref_index;
    V2PlaceWay *place_way;

    result = on_pack_way(&context->pack, way);
    if (result != 0) {
        context->failed = 1;
        return result;
    }
    if (!v2_needed_place_way_contains(context, way->id)) return 0;
    if (v2_grow_place_ways(context, context->place_way_count + 1U) != 0 ||
        grow_refs(&context->pack, context->pack.ref_count + way->ref_count) != 0) {
        context->failed = 1;
        return -1;
    }
    for (ref_index = 0U; ref_index < way->ref_count; ++ref_index) context->pack.refs[context->pack.ref_count + ref_index] = way->refs[ref_index];
    place_way = &context->place_ways[context->place_way_count++];
    place_way->id = way->id;
    place_way->ref_offset = context->pack.ref_count;
    place_way->ref_count = way->ref_count;
    context->pack.ref_count += way->ref_count;
    return 0;
}

static int v2_compute_places(V2BuildContext *context) {
    unsigned int place_index;
    if (context->place_way_count != 0U) rt_sort(context->place_ways, context->place_way_count, sizeof(*context->place_ways), compare_place_way);
    for (place_index = 0U; place_index < context->place_count; ++place_index) {
        V2Place *place = &context->places[place_index];
        long long min_lon = 9223372036854775807LL;
        long long min_lat = 9223372036854775807LL;
        long long max_lon = -9223372036854775807LL;
        long long max_lat = -9223372036854775807LL;
        unsigned int member_index;
        unsigned long long resolved_nodes = 0ULL;
        for (member_index = 0U; member_index < place->member_count; ++member_index) {
            long long way_id = context->place_members[place->member_offset + member_index];
            V2PlaceWay *way = v2_find_place_way(context, way_id);
            unsigned int ref_index;
            if (way == 0) continue;
            for (ref_index = 0U; ref_index < way->ref_count; ++ref_index) {
                PackBuildNode *node = find_node(&context->pack, context->pack.refs[way->ref_offset + ref_index]);
                if (node == 0 || !node->found) continue;
                if (node->lon_nano < min_lon) min_lon = node->lon_nano;
                if (node->lon_nano > max_lon) max_lon = node->lon_nano;
                if (node->lat_nano < min_lat) min_lat = node->lat_nano;
                if (node->lat_nano > max_lat) max_lat = node->lat_nano;
                resolved_nodes += 1ULL;
            }
        }
        if (resolved_nodes != 0ULL && min_lon < max_lon && min_lat < max_lat) {
            place->min_lon_nano = min_lon;
            place->min_lat_nano = min_lat;
            place->max_lon_nano = max_lon;
            place->max_lat_nano = max_lat;
            place->have_bbox = 1;
            context->place_relations_with_bbox += 1ULL;
        }
    }
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

static long long v2_tile_x_min_lon(unsigned int x, unsigned int zoom) {
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    return (long long)(((unsigned long long)x * 360000000000ULL) / axis) - 180000000000LL;
}

static long long v2_tile_x_max_lon(unsigned int x, unsigned int zoom) {
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    return (long long)((((unsigned long long)x + 1ULL) * 360000000000ULL) / axis) - 180000000000LL;
}

static long long v2_tile_y_max_lat(unsigned int y, unsigned int zoom) {
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    return 90000000000LL - (long long)(((unsigned long long)y * 180000000000ULL) / axis);
}

static long long v2_tile_y_min_lat(unsigned int y, unsigned int zoom) {
    unsigned long long axis = (unsigned long long)v2_tile_axis(zoom);
    return 90000000000LL - (long long)((((unsigned long long)y + 1ULL) * 180000000000ULL) / axis);
}

static unsigned long long v2_tile_id(unsigned int z, unsigned int x, unsigned int y) {
    return ((unsigned long long)z << 58U) | ((unsigned long long)x << 29U) | (unsigned long long)y;
}

static int v2_materialize_features_and_assign_tiles(V2BuildContext *context) {
    unsigned int feature_index;
    unsigned int axis = v2_tile_axis(context->tile_zoom);
    context->feature_infos = (V2FeatureInfo *)rt_malloc(sizeof(*context->feature_infos) * (size_t)context->pack.feature_count);
    if (context->feature_infos == 0 && context->pack.feature_count != 0U) return -1;
    for (feature_index = 0U; feature_index < context->pack.feature_count; ++feature_index) {
        const PackBuildFeature *feature = &context->pack.features[feature_index];
        V2FeatureInfo *info = &context->feature_infos[feature_index];
        unsigned int ref_index;
        unsigned int min_x;
        unsigned int max_x;
        unsigned int min_y;
        unsigned int max_y;
        unsigned int x;
        unsigned int y;
        int have_bbox = 0;
        rt_memset(info, 0, sizeof(*info));
        for (ref_index = 0U; ref_index < feature->ref_count; ++ref_index) {
            PackBuildNode *node = find_node(&context->pack, context->pack.refs[feature->ref_offset + ref_index]);
            if (node == 0 || !node->found) {
                info->missing = 1;
                break;
            }
            if (!have_bbox) {
                info->min_lon_nano = info->max_lon_nano = node->lon_nano;
                info->min_lat_nano = info->max_lat_nano = node->lat_nano;
                have_bbox = 1;
            } else {
                if (node->lon_nano < info->min_lon_nano) info->min_lon_nano = node->lon_nano;
                if (node->lon_nano > info->max_lon_nano) info->max_lon_nano = node->lon_nano;
                if (node->lat_nano < info->min_lat_nano) info->min_lat_nano = node->lat_nano;
                if (node->lat_nano > info->max_lat_nano) info->max_lat_nano = node->lat_nano;
            }
        }
        if (info->missing || !have_bbox) continue;
        min_x = v2_lon_to_tile_x(info->min_lon_nano, context->tile_zoom);
        max_x = v2_lon_to_tile_x(info->max_lon_nano, context->tile_zoom);
        min_y = v2_lat_to_tile_y(info->max_lat_nano, context->tile_zoom);
        max_y = v2_lat_to_tile_y(info->min_lat_nano, context->tile_zoom);
        if (min_x >= axis || max_x >= axis || min_y >= axis || max_y >= axis) continue;
        for (y = min_y; y <= max_y; ++y) {
            for (x = min_x; x <= max_x; ++x) {
                V2TileAssignment *assignment;
                if (v2_grow_assignments(context, context->assignment_count + 1U) != 0) return -1;
                assignment = &context->assignments[context->assignment_count++];
                assignment->tile_id = v2_tile_id(context->tile_zoom, x, y);
                assignment->x = x;
                assignment->y = y;
                assignment->feature_index = feature_index;
            }
        }
    }
    if (context->assignment_count != 0U) rt_sort(context->assignments, context->assignment_count, sizeof(*context->assignments), compare_assignment);
    context->duplicated_tile_features = context->assignment_count;
    return 0;
}

static void v2_write_u32(unsigned char *out, unsigned int value) {
    write_u32_le(out, value);
}

static void v2_write_u64(unsigned char *out, unsigned long long value) {
    write_u64_le(out, value);
}

static void v2_write_i64(unsigned char *out, long long value) {
    write_i64_le(out, value);
}

static int v2_write_header_fd(int fd, unsigned int tile_zoom, unsigned int tile_halo, unsigned long long place_count, unsigned long long tile_count,
                              unsigned long long place_offset, unsigned long long place_size, unsigned long long tile_offset, unsigned long long tile_size,
                              unsigned long long payload_offset, unsigned long long payload_size, unsigned long long string_offset, unsigned long long string_size,
                              const V2BuildContext *context) {
    unsigned char data[OSMRPACK_V2_HEADER_SIZE];
    rt_memset(data, 0, sizeof(data));
    memcpy(data, osmrpack_v2_magic, sizeof(osmrpack_v2_magic));
    v2_write_u32(data + 8U, OSMRPACK_V2_VERSION);
    v2_write_u32(data + 12U, OSMRPACK_V2_HEADER_SIZE);
    v2_write_u32(data + 16U, 0U);
    v2_write_u32(data + 20U, tile_zoom);
    v2_write_u32(data + 24U, tile_halo);
    v2_write_u32(data + 28U, PACK_STYLE_COUNT);
    v2_write_u32(data + 32U, OSMRPACK_V2_PLACE_RECORD_SIZE);
    v2_write_u32(data + 36U, OSMRPACK_V2_TILE_RECORD_SIZE);
    v2_write_u32(data + 40U, OSMRPACK_FEATURE_HEADER_SIZE);
    v2_write_u64(data + 48U, place_count);
    v2_write_u64(data + 56U, tile_count);
    v2_write_u64(data + 64U, place_offset);
    v2_write_u64(data + 72U, place_size);
    v2_write_u64(data + 80U, tile_offset);
    v2_write_u64(data + 88U, tile_size);
    v2_write_u64(data + 96U, 0ULL);
    v2_write_u64(data + 104U, 0ULL);
    v2_write_u64(data + 112U, payload_offset);
    v2_write_u64(data + 120U, payload_size);
    v2_write_u64(data + 128U, string_offset);
    v2_write_u64(data + 136U, string_size);
    v2_write_u64(data + 144U, context->pack.source_nodes);
    v2_write_u64(data + 152U, context->pack.source_ways);
    v2_write_u64(data + 160U, context->source_relations);
    return rt_write_all(fd, data, sizeof(data));
}

static int v2_write_place_record_fd(int fd, const V2Place *place, unsigned long long string_table_offset) {
    unsigned char data[OSMRPACK_V2_PLACE_RECORD_SIZE];
    rt_memset(data, 0, sizeof(data));
    v2_write_i64(data + 0U, place->id);
    v2_write_u32(data + 8U, place->source_type);
    v2_write_u32(data + 12U, place->place_kind);
    v2_write_u32(data + 16U, place->admin_level);
    v2_write_u32(data + 20U, place->rank_score);
    v2_write_i64(data + 24U, place->min_lon_nano);
    v2_write_i64(data + 32U, place->min_lat_nano);
    v2_write_i64(data + 40U, place->max_lon_nano);
    v2_write_i64(data + 48U, place->max_lat_nano);
    v2_write_u64(data + 56U, string_table_offset + place->name_offset);
    v2_write_u32(data + 64U, place->name_size);
    v2_write_u32(data + 92U, place->flags);
    return rt_write_all(fd, data, sizeof(data));
}

static int v2_write_tile_record_fd(int fd, const V2TileRecord *record) {
    unsigned char data[OSMRPACK_V2_TILE_RECORD_SIZE];
    rt_memset(data, 0, sizeof(data));
    v2_write_u64(data + 0U, record->tile_id);
    v2_write_u32(data + 8U, record->z);
    v2_write_u32(data + 12U, record->x);
    v2_write_u32(data + 16U, record->y);
    v2_write_u32(data + 20U, record->feature_count);
    v2_write_u32(data + 24U, record->layer_mask);
    v2_write_u64(data + 32U, record->payload_offset);
    v2_write_u64(data + 40U, record->payload_size);
    v2_write_i64(data + 48U, record->min_lon_nano);
    v2_write_i64(data + 56U, record->min_lat_nano);
    v2_write_i64(data + 64U, record->max_lon_nano);
    v2_write_i64(data + 72U, record->max_lat_nano);
    return rt_write_all(fd, data, sizeof(data));
}

static int v2_write_feature_points_from_refs(int fd, V2BuildContext *context, const PackBuildFeature *feature) {
    unsigned char data[16];
    unsigned int ref_index;
    for (ref_index = 0U; ref_index < feature->ref_count; ++ref_index) {
        PackBuildNode *node = find_node(&context->pack, context->pack.refs[feature->ref_offset + ref_index]);
        if (node == 0 || !node->found) return -1;
        write_i64_le(data + 0U, node->lon_nano);
        write_i64_le(data + 8U, node->lat_nano);
        if (rt_write_all(fd, data, sizeof(data)) != 0) return -1;
    }
    return 0;
}

static unsigned int v2_count_valid_places(V2BuildContext *context) {
    unsigned int count = 0U;
    unsigned int index;
    for (index = 0U; index < context->place_count; ++index) {
        if (context->places[index].have_bbox) count += 1U;
    }
    return count;
}

static int v2_prepare_tile_records(V2BuildContext *context) {
    unsigned int index = 0U;
    while (index < context->assignment_count) {
        unsigned int start = index;
        V2TileAssignment *assignment = &context->assignments[index];
        V2TileRecord *record;
        unsigned int layer_mask = 0U;
        while (index < context->assignment_count && context->assignments[index].tile_id == assignment->tile_id) {
            unsigned int feature_index = context->assignments[index].feature_index;
            layer_mask |= 1U << context->pack.features[feature_index].style_id;
            index += 1U;
        }
        if (v2_grow_tile_records(context, context->tile_record_count + 1U) != 0) return -1;
        record = &context->tile_records[context->tile_record_count++];
        rt_memset(record, 0, sizeof(*record));
        record->tile_id = assignment->tile_id;
        record->z = context->tile_zoom;
        record->x = assignment->x;
        record->y = assignment->y;
        record->feature_count = index - start;
        record->layer_mask = layer_mask;
        record->min_lon_nano = v2_tile_x_min_lon(assignment->x, context->tile_zoom);
        record->max_lon_nano = v2_tile_x_max_lon(assignment->x, context->tile_zoom);
        record->min_lat_nano = v2_tile_y_min_lat(assignment->y, context->tile_zoom);
        record->max_lat_nano = v2_tile_y_max_lat(assignment->y, context->tile_zoom);
    }
    return 0;
}

static int v2_write_pack(const char *path, V2BuildContext *context, unsigned long long elapsed_ms) {
    unsigned int valid_place_count = v2_count_valid_places(context);
    unsigned long long place_offset = OSMRPACK_V2_HEADER_SIZE;
    unsigned long long place_size = (unsigned long long)valid_place_count * OSMRPACK_V2_PLACE_RECORD_SIZE;
    unsigned long long tile_offset = place_offset + place_size;
    unsigned long long tile_size;
    unsigned long long payload_offset;
    unsigned long long payload_start;
    unsigned long long payload_end;
    unsigned long long string_offset;
    int fd;
    unsigned int tile_index;
    unsigned int assignment_index = 0U;
    unsigned int place_index;

    if (v2_prepare_tile_records(context) != 0) return -1;
    tile_size = (unsigned long long)context->tile_record_count * OSMRPACK_V2_TILE_RECORD_SIZE;
    payload_offset = tile_offset + tile_size;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) return -1;
    if (v2_write_header_fd(fd, context->tile_zoom, context->tile_halo, valid_place_count, context->tile_record_count,
                           place_offset, place_size, tile_offset, tile_size, payload_offset, 0ULL, payload_offset, 0ULL, context) != 0) goto fail;
    if (platform_seek(fd, (long long)payload_offset, PLATFORM_SEEK_SET) < 0) goto fail;
    payload_start = (unsigned long long)platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    for (tile_index = 0U; tile_index < context->tile_record_count; ++tile_index) {
        V2TileRecord *record = &context->tile_records[tile_index];
        unsigned int feature_in_tile;
        unsigned long long tile_start = (unsigned long long)platform_seek(fd, 0, PLATFORM_SEEK_CUR);
        unsigned char count_data[8];
        record->payload_offset = tile_start;
        write_u64_le(count_data, record->feature_count);
        if (rt_write_all(fd, count_data, sizeof(count_data)) != 0) goto fail;
        for (feature_in_tile = 0U; feature_in_tile < record->feature_count; ++feature_in_tile) {
            unsigned int feature_index = context->assignments[assignment_index++].feature_index;
            PackBuildFeature *feature = &context->pack.features[feature_index];
            V2FeatureInfo *info = &context->feature_infos[feature_index];
            if (write_feature_header(fd, feature, feature->ref_count, info->min_lon_nano, info->min_lat_nano, info->max_lon_nano, info->max_lat_nano) != 0 ||
                v2_write_feature_points_from_refs(fd, context, feature) != 0) goto fail;
        }
        record->payload_size = (unsigned long long)platform_seek(fd, 0, PLATFORM_SEEK_CUR) - tile_start;
    }
    payload_end = (unsigned long long)platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    string_offset = payload_end;
    if (context->string_size != 0U && rt_write_all(fd, context->strings, context->string_size) != 0) goto fail;
    if (platform_seek(fd, (long long)place_offset, PLATFORM_SEEK_SET) < 0) goto fail;
    for (place_index = 0U; place_index < context->place_count; ++place_index) {
        if (context->places[place_index].have_bbox && v2_write_place_record_fd(fd, &context->places[place_index], string_offset) != 0) goto fail;
    }
    if (platform_seek(fd, (long long)tile_offset, PLATFORM_SEEK_SET) < 0) goto fail;
    for (tile_index = 0U; tile_index < context->tile_record_count; ++tile_index) {
        if (v2_write_tile_record_fd(fd, &context->tile_records[tile_index]) != 0) goto fail;
    }
    if (platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) goto fail;
    if (v2_write_header_fd(fd, context->tile_zoom, context->tile_halo, valid_place_count, context->tile_record_count,
                           place_offset, place_size, tile_offset, tile_size, payload_start, payload_end - payload_start,
                           string_offset, context->string_size, context) != 0) goto fail;
    if (platform_close(fd) != 0) return -1;
    rt_write_cstr(1, "rpack_v2_written: ");
    rt_write_cstr(1, path);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_zoom: ");
    rt_write_uint(1, context->tile_zoom);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_halo: ");
    rt_write_uint(1, context->tile_halo);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "places_written: ");
    rt_write_uint(1, valid_place_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tiles_written: ");
    rt_write_uint(1, context->tile_record_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "tile_feature_copies: ");
    rt_write_uint(1, context->duplicated_tile_features);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "classified_ways: ");
    rt_write_uint(1, context->pack.classified_ways);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "unique_nodes_needed: ");
    rt_write_uint(1, context->pack.unique_node_count);
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
    const char *program = argc > 0 ? argv[0] : "osmrenderpackv2";
    const char *pbf_path;
    const char *pack_path;
    int argi = 1;
    PbfStreamCallbacks callbacks;
    V2BuildContext context;
    char error[PBF_ERROR_CAPACITY];
    unsigned long long start_ns;
    unsigned long long elapsed_ms;

    rt_memset(&context, 0, sizeof(context));
    context.tile_zoom = OSMRPACK_DEFAULT_TILE_ZOOM;
    context.tile_halo = OSMRPACK_V2_DEFAULT_TILE_HALO;
    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "--tile-zoom") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.tile_zoom) != 0 || context.tile_zoom > OSMRPACK_MAX_TILE_ZOOM) {
                v2_write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--tile-halo") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.tile_halo) != 0 || context.tile_halo > 8U) {
                v2_write_usage(program);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--buildings") == 0) {
            context.pack.include_buildings = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            v2_write_usage(program);
            return 0;
        } else {
            v2_write_usage(program);
            return 1;
        }
    }
    if (argc - argi != 2) {
        v2_write_usage(program);
        return 1;
    }
    pbf_path = argv[argi];
    pack_path = argv[argi + 1];
    start_ns = platform_get_monotonic_time_ns();

    rt_write_cstr(1, "phase: collect_places\n");
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.relation_tags = v2_on_relation_tags;
    callbacks.relation = v2_on_relation;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
        rt_write_cstr(2, "osmrenderpackv2: ");
        rt_write_cstr(2, context.failed ? "out of memory while collecting places" : (error[0] == '\0' ? "could not collect places" : error));
        rt_write_char(2, '\n');
        return 1;
    }
    if (v2_prepare_needed_place_way_ids(&context) != 0) return 1;

    rt_write_cstr(1, "phase: collect_ways\n");
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.way = v2_on_way;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed || context.pack.failed) {
        rt_write_cstr(2, "osmrenderpackv2: ");
        rt_write_cstr(2, (context.failed || context.pack.failed) ? "out of memory while collecting ways" : (error[0] == '\0' ? "could not collect ways" : error));
        rt_write_char(2, '\n');
        return 1;
    }
    rt_write_cstr(1, "phase: build_node_lookup\n");
    if (v2_build_unique_nodes(&context.pack) != 0) {
        rt_write_cstr(2, "osmrenderpackv2: out of memory while building node lookup\n");
        return 1;
    }

    rt_write_cstr(1, "phase: collect_nodes\n");
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS | PBF_STREAM_SKIP_RELATION_ROLES;
    callbacks.node = on_pack_node;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context.pack, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmrenderpackv2: ");
        rt_write_cstr(2, error[0] == '\0' ? "could not collect node coordinates" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    rt_write_cstr(1, "phase: assign_tiles\n");
    if (v2_compute_places(&context) != 0 || v2_materialize_features_and_assign_tiles(&context) != 0) {
        rt_write_cstr(2, "osmrenderpackv2: could not materialize v2 tile data\n");
        return 1;
    }
    elapsed_ms = (platform_get_monotonic_time_ns() - start_ns) / 1000000ULL;
    rt_write_cstr(1, "phase: write_pack\n");
    if (v2_write_pack(pack_path, &context, elapsed_ms) != 0) {
        rt_write_cstr(2, "osmrenderpackv2: could not write rpack v2\n");
        return 1;
    }
    rt_free(context.places);
    rt_free(context.place_members);
    rt_free(context.needed_place_way_ids);
    rt_free(context.place_ways);
    rt_free(context.strings);
    rt_free(context.feature_infos);
    rt_free(context.assignments);
    rt_free(context.tile_records);
    rt_free(context.pack.features);
    rt_free(context.pack.refs);
    rt_free(context.pack.nodes);
    return 0;
}
