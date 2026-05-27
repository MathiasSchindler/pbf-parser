#include "pbf.h"
#include "platform.h"
#include "runtime.h"

#define POTSDAM_BBOX "12.88,52.32,13.18,52.50"
#define INF_DISTANCE 0xffffffffffffffffULL
#define ROUTE_LABEL_CAPACITY 96U
#define GTFS_LINE_CAPACITY 4096U
#define GTFS_KEY_CAPACITY 160U

typedef struct { long long id; long long lat_nano; long long lon_nano; int graph_index; } RouteCoord;
typedef struct { long long id; unsigned int index; int used; } RouteCoordSlot;
typedef struct { RouteCoord *items; RouteCoordSlot *slots; unsigned int count; unsigned int capacity; unsigned int slot_capacity; } RouteCoordStore;
typedef struct { long long id; long long lat_nano; long long lon_nano; int first_edge; unsigned long long distance; int previous; int previous_edge; int settled; } RouteNode;
typedef struct { int to; int next; unsigned int meters; char label[ROUTE_LABEL_CAPACITY]; } RouteEdge;
typedef struct { RouteNode *nodes; RouteEdge *edges; unsigned int node_count; unsigned int node_capacity; unsigned int edge_count; unsigned int edge_capacity; } RouteGraph;
typedef struct { unsigned int left_coord_index; unsigned int right_coord_index; char label[ROUTE_LABEL_CAPACITY]; } WaySegment;
typedef struct { WaySegment *items; unsigned int count; unsigned int capacity; } WaySegmentStore;
typedef struct { int node; unsigned long long distance; } RouteHeapItem;
typedef struct { RouteHeapItem *items; unsigned int count; unsigned int capacity; } RouteHeap;
typedef struct { PbfText street; PbfText house; PbfText city; int has_street; int has_house; int has_city; } AddressTags;
typedef struct { const char *input; char street[160]; char house[48]; long long lat_nano; long long lon_nano; long long osm_id; const char *source_type; int found; } AddressQuery;
typedef struct { const char *data; size_t size; } CsvField;

typedef struct {
    int fd;
    unsigned char buffer[65536];
    size_t position;
    size_t used;
} GtfsLineReader;

typedef struct {
    char *key;
    size_t key_size;
    unsigned int value;
    int used;
} GtfsStringMapEntry;

typedef struct {
    GtfsStringMapEntry *entries;
    unsigned int capacity;
    unsigned int count;
} GtfsStringMap;

typedef struct {
    char *id;
    char *name;
    long long lat_nano;
    long long lon_nano;
    unsigned int walk_from_m;
    unsigned int walk_to_m;
    unsigned int walk_from_sec;
    unsigned int walk_to_sec;
    int origin_ok;
    int dest_ok;
} GtfsStop;

typedef struct {
    GtfsStop *items;
    unsigned int count;
    unsigned int capacity;
} GtfsStopList;

typedef struct {
    char *short_name;
    char *long_name;
    unsigned int mode;
} GtfsRoute;

typedef struct {
    GtfsRoute *items;
    unsigned int count;
    unsigned int capacity;
} GtfsRouteList;

typedef struct {
    int found;
    unsigned int mode;
    const char *line_short;
    const char *line_long;
    const char *board_stop;
    const char *alight_stop;
    unsigned int board_departure_sec;
    unsigned int alight_arrival_sec;
    unsigned int walk_to_stop_m;
    unsigned int walk_from_stop_m;
    unsigned int total_sec;
    unsigned int debug_active_services;
    unsigned int debug_candidate_origins;
    unsigned int debug_candidate_destinations;
    unsigned int debug_active_trips;
    unsigned long long debug_stop_times_scanned;
    unsigned long long debug_stop_times_trip_hits;
    unsigned long long debug_board_candidates;
} TransitPlan;

static unsigned int distance_m(long long lat_a, long long lon_a, long long lat_b, long long lon_b);

typedef struct {
    const char *pbf_path;
    const char *city_name;
    long long min_lon_nano;
    long long min_lat_nano;
    long long max_lon_nano;
    long long max_lat_nano;
    AddressQuery from;
    AddressQuery to;
    RouteCoordStore coords;
    RouteGraph graph;
    WaySegmentStore way_segments;
    unsigned long long bbox_nodes;
    unsigned long long ways_seen;
    unsigned long long walkable_ways;
    unsigned long long address_nodes;
    unsigned long long address_ways;
    unsigned long long speed_m_per_hour;
    const char *gtfs_path;
    int have_depart;
    int have_arrive;
    unsigned int depart_date;
    unsigned int depart_seconds;
    unsigned int arrive_date;
    unsigned int arrive_seconds;
    unsigned int thread_count;
    int show_geometry;
    int use_color;
} RouteContext;

typedef struct {
    const RouteContext *shared;
    WaySegmentStore segments;
    AddressQuery from;
    AddressQuery to;
    unsigned long long ways_seen;
    unsigned long long walkable_ways;
    unsigned long long address_ways;
} WayWorkerContext;

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf FROM_ADDRESS TO_ADDRESS [--city Potsdam] [--bbox MINLON,MINLAT,MAXLON,MAXLAT] [--speed-kmh N] [--threads N] [--gtfs DIR] [--depart YYYY-MM-DDTHH:MM[:SS]] [--arrive YYYY-MM-DDTHH:MM[:SS]] [--geometry] [--color] [--no-color]\n");
}

static void write_color(const RouteContext *context, const char *code) {
    if (context->use_color) rt_write_cstr(1, code);
}

static void write_color_reset(const RouteContext *context) {
    if (context->use_color) rt_write_cstr(1, "\033[0m");
}

static void write_colored_cstr(const RouteContext *context, const char *code, const char *text) {
    write_color(context, code);
    rt_write_cstr(1, text);
    write_color_reset(context);
}

static void write_colored_uint(const RouteContext *context, const char *code, unsigned long long value) {
    write_color(context, code);
    rt_write_uint(1, value);
    write_color_reset(context);
}

static int parse_two_digits(const char *text, unsigned int *value_out) {
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return -1;
    *value_out = (unsigned int)(text[0] - '0') * 10U + (unsigned int)(text[1] - '0');
    return 0;
}

static int parse_four_digits(const char *text, unsigned int *value_out) {
    unsigned int d0;
    unsigned int d1;
    unsigned int d2;
    unsigned int d3;
    if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9' || text[2] < '0' || text[2] > '9' || text[3] < '0' || text[3] > '9') return -1;
    d0 = (unsigned int)(text[0] - '0');
    d1 = (unsigned int)(text[1] - '0');
    d2 = (unsigned int)(text[2] - '0');
    d3 = (unsigned int)(text[3] - '0');
    *value_out = d0 * 1000U + d1 * 100U + d2 * 10U + d3;
    return 0;
}

static int parse_datetime_arg(const char *text, unsigned int *date_out, unsigned int *seconds_out) {
    size_t length = rt_strlen(text);
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second = 0U;
    if (!(length == 16U || length == 19U)) return -1;
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') return -1;
    if (length == 19U && (text[16] != ':')) return -1;
    if (parse_four_digits(text, &year) != 0 || parse_two_digits(text + 5U, &month) != 0 || parse_two_digits(text + 8U, &day) != 0 ||
        parse_two_digits(text + 11U, &hour) != 0 || parse_two_digits(text + 14U, &minute) != 0) return -1;
    if (length == 19U && parse_two_digits(text + 17U, &second) != 0) return -1;
    if (year < 2000U || year > 2100U) return -1;
    if (month < 1U || month > 12U || day < 1U || day > 31U) return -1;
    if (hour > 23U || minute > 59U || second > 59U) return -1;
    *date_out = year * 10000U + month * 100U + day;
    *seconds_out = hour * 3600U + minute * 60U + second;
    return 0;
}

static int parse_threads(const char *text, unsigned int *threads_out) {
    unsigned long long value;
    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 64ULL) return -1;
    *threads_out = (unsigned int)value;
    return 0;
}

static unsigned int gtfs_hash_bytes(const char *data, size_t size) {
    unsigned int hash = 2166136261U;
    size_t index;
    for (index = 0U; index < size; ++index) {
        hash ^= (unsigned char)data[index];
        hash *= 16777619U;
    }
    return hash;
}

static int csv_field_equals_cstr(CsvField field, const char *text) {
    size_t size = rt_strlen(text);
    return field.size == size && memcmp(field.data, text, size) == 0;
}

static char *csv_field_copy(CsvField field) {
    char *copy = (char *)rt_malloc(field.size + 1U);
    if (copy == 0) return 0;
    if (field.size != 0U) memcpy(copy, field.data, field.size);
    copy[field.size] = '\0';
    return copy;
}

static int gtfs_open_file(const char *dir_path, const char *name) {
    char path[1024];
    if (rt_join_path(dir_path, name, path, sizeof(path)) != 0) return -1;
    return platform_open_read(path);
}

static void gtfs_line_reader_init(GtfsLineReader *reader, int fd) {
    reader->fd = fd;
    reader->position = 0U;
    reader->used = 0U;
}

static int gtfs_read_line(GtfsLineReader *reader, char *line, size_t line_capacity) {
    size_t line_size = 0U;
    if (line_capacity == 0U) return -1;
    for (;;) {
        if (reader->position >= reader->used) {
            long amount = platform_read(reader->fd, reader->buffer, sizeof(reader->buffer));
            if (amount < 0) return -1;
            if (amount == 0) {
                if (line_size == 0U) return 0;
                line[line_size] = '\0';
                return 1;
            }
            reader->position = 0U;
            reader->used = (size_t)amount;
        }
        while (reader->position < reader->used) {
            char ch = (char)reader->buffer[reader->position++];
            if (line_size + 1U >= line_capacity) return -1;
            if (ch == '\n') {
                if (line_size > 0U && line[line_size - 1U] == '\r') line_size -= 1U;
                line[line_size] = '\0';
                return 1;
            }
            line[line_size++] = ch;
        }
    }
}

static int csv_get_fields(const char *line, const unsigned int *indexes, unsigned int index_count, CsvField *fields) {
    unsigned int field_index = 0U;
    size_t offset = 0U;
    unsigned int index;
    for (index = 0U; index < index_count; ++index) {
        fields[index].data = "";
        fields[index].size = 0U;
    }
    while (line[offset] != '\0') {
        size_t start;
        size_t end;
        int quoted = 0;
        if (line[offset] == '"') {
            quoted = 1;
            offset += 1U;
            start = offset;
            while (line[offset] != '\0') {
                if (line[offset] == '"') {
                    if (line[offset + 1U] == '"') {
                        offset += 2U;
                    } else {
                        break;
                    }
                } else {
                    offset += 1U;
                }
            }
            end = offset;
            if (line[offset] == '"') offset += 1U;
            while (line[offset] != '\0' && line[offset] != ',') offset += 1U;
        } else {
            start = offset;
            while (line[offset] != '\0' && line[offset] != ',') offset += 1U;
            end = offset;
            while (end > start && (line[end - 1U] == '\r' || line[end - 1U] == '\n')) end -= 1U;
            while (end > start && line[end - 1U] == ' ') end -= 1U;
            while (start < end && line[start] == ' ') start += 1U;
        }
        for (index = 0U; index < index_count; ++index) {
            if (indexes[index] == field_index) {
                fields[index].data = line + start;
                fields[index].size = end - start;
                break;
            }
        }
        if (line[offset] == ',') offset += 1U;
        field_index += 1U;
    }
    return 0;
}

static int csv_find_header_indexes(const char *line, const char **names, unsigned int count, unsigned int *indexes) {
    unsigned int field_index = 0U;
    size_t offset = 0U;
    unsigned int found = 0U;
    unsigned int i;
    for (i = 0U; i < count; ++i) indexes[i] = 0xffffffffU;
    while (line[offset] != '\0') {
        size_t start = offset;
        size_t end;
        CsvField field;
        if (line[offset] == '"') {
            offset += 1U;
            start = offset;
            while (line[offset] != '\0' && line[offset] != '"') offset += 1U;
            end = offset;
            if (line[offset] == '"') offset += 1U;
            while (line[offset] != '\0' && line[offset] != ',') offset += 1U;
        } else {
            while (line[offset] != '\0' && line[offset] != ',') offset += 1U;
            end = offset;
            while (end > start && (line[end - 1U] == '\r' || line[end - 1U] == '\n')) end -= 1U;
        }
        field.data = line + start;
        field.size = end - start;
        for (i = 0U; i < count; ++i) {
            if (indexes[i] == 0xffffffffU && csv_field_equals_cstr(field, names[i])) {
                indexes[i] = field_index;
                found += 1U;
                break;
            }
        }
        if (line[offset] == ',') offset += 1U;
        field_index += 1U;
    }
    return found == count ? 0 : -1;
}

static int gtfs_string_map_grow(GtfsStringMap *map, unsigned int needed) {
    unsigned int capacity = map->capacity == 0U ? 2048U : map->capacity;
    GtfsStringMapEntry *entries;
    unsigned int old_capacity = map->capacity;
    unsigned int index;
    while (capacity * 7U / 10U < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == map->capacity) return 0;
    entries = (GtfsStringMapEntry *)rt_malloc(sizeof(*entries) * (size_t)capacity);
    if (entries == 0) return -1;
    rt_memset(entries, 0, sizeof(*entries) * (size_t)capacity);
    for (index = 0U; index < old_capacity; ++index) {
        if (map->entries[index].used) {
            unsigned int slot = gtfs_hash_bytes(map->entries[index].key, map->entries[index].key_size) & (capacity - 1U);
            while (entries[slot].used) slot = (slot + 1U) & (capacity - 1U);
            entries[slot] = map->entries[index];
        }
    }
    rt_free(map->entries);
    map->entries = entries;
    map->capacity = capacity;
    return 0;
}

static int gtfs_string_map_put_raw(GtfsStringMap *map, const char *key, size_t key_size, unsigned int value, int merge_or) {
    unsigned int slot;
    if (key_size == 0U) return 0;
    if (gtfs_string_map_grow(map, map->count + 1U) != 0) return -1;
    slot = gtfs_hash_bytes(key, key_size) & (map->capacity - 1U);
    for (;;) {
        if (!map->entries[slot].used) {
            map->entries[slot].key = (char *)rt_malloc(key_size + 1U);
            if (map->entries[slot].key == 0) return -1;
            memcpy(map->entries[slot].key, key, key_size);
            map->entries[slot].key[key_size] = '\0';
            map->entries[slot].key_size = key_size;
            map->entries[slot].value = value;
            map->entries[slot].used = 1;
            map->count += 1U;
            return 0;
        }
        if (map->entries[slot].key_size == key_size && memcmp(map->entries[slot].key, key, key_size) == 0) {
            if (merge_or) map->entries[slot].value |= value;
            else map->entries[slot].value = value;
            return 0;
        }
        slot = (slot + 1U) & (map->capacity - 1U);
    }
}

static int gtfs_string_map_put(GtfsStringMap *map, CsvField key, unsigned int value, int merge_or) {
    return gtfs_string_map_put_raw(map, key.data, key.size, value, merge_or);
}

static unsigned int gtfs_string_map_get(const GtfsStringMap *map, CsvField key) {
    unsigned int slot;
    if (map->capacity == 0U || key.size == 0U) return 0U;
    slot = gtfs_hash_bytes(key.data, key.size) & (map->capacity - 1U);
    for (;;) {
        if (!map->entries[slot].used) return 0U;
        if (map->entries[slot].key_size == key.size && memcmp(map->entries[slot].key, key.data, key.size) == 0) return map->entries[slot].value;
        slot = (slot + 1U) & (map->capacity - 1U);
    }
}

static void gtfs_string_map_destroy(GtfsStringMap *map) {
    unsigned int index;
    for (index = 0U; index < map->capacity; ++index) {
        if (map->entries[index].used) rt_free(map->entries[index].key);
    }
    rt_free(map->entries);
    rt_memset(map, 0, sizeof(*map));
}

static int gtfs_parse_uint_field(CsvField field, unsigned int *value_out) {
    unsigned int value = 0U;
    size_t index;
    if (field.size == 0U) return -1;
    for (index = 0U; index < field.size; ++index) {
        char ch = field.data[index];
        if (ch < '0' || ch > '9') return -1;
        value = value * 10U + (unsigned int)(ch - '0');
    }
    *value_out = value;
    return 0;
}

static int parse_gtfs_coord_field(CsvField field, long long *value_out) {
    size_t index = 0U;
    unsigned long long whole = 0ULL;
    unsigned long long fraction = 0ULL;
    unsigned int fraction_digits = 0U;
    int negative = 0;
    if (field.size == 0U) return -1;
    if (field.data[index] == '-') {
        negative = 1;
        index += 1U;
    }
    if (index >= field.size || field.data[index] < '0' || field.data[index] > '9') return -1;
    while (index < field.size && field.data[index] >= '0' && field.data[index] <= '9') {
        whole = whole * 10ULL + (unsigned long long)(field.data[index] - '0');
        if (whole > 180ULL) return -1;
        index += 1U;
    }
    if (index < field.size && field.data[index] == '.') {
        index += 1U;
        while (index < field.size && field.data[index] >= '0' && field.data[index] <= '9') {
            if (fraction_digits < 9U) {
                fraction = fraction * 10ULL + (unsigned long long)(field.data[index] - '0');
                fraction_digits += 1U;
            }
            index += 1U;
        }
    }
    if (index != field.size) return -1;
    while (fraction_digits < 9U) {
        fraction *= 10ULL;
        fraction_digits += 1U;
    }
    *value_out = (long long)(whole * 1000000000ULL + fraction);
    if (negative) *value_out = -*value_out;
    return 0;
}

static int gtfs_parse_time_field(CsvField field, unsigned int *seconds_out) {
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;
    size_t index = 0U;
    if (field.size < 7U) return -1;
    while (index < field.size && field.data[index] >= '0' && field.data[index] <= '9') {
        hour = hour * 10U + (unsigned int)(field.data[index] - '0');
        index += 1U;
    }
    if (index == 0U || index + 6U != field.size || field.data[index] != ':' || field.data[index + 3U] != ':') return -1;
    minute = (unsigned int)(field.data[index + 1U] - '0') * 10U + (unsigned int)(field.data[index + 2U] - '0');
    second = (unsigned int)(field.data[index + 4U] - '0') * 10U + (unsigned int)(field.data[index + 5U] - '0');
    if (field.data[index + 1U] < '0' || field.data[index + 1U] > '9' || field.data[index + 2U] < '0' || field.data[index + 2U] > '9' ||
        field.data[index + 4U] < '0' || field.data[index + 4U] > '9' || field.data[index + 5U] < '0' || field.data[index + 5U] > '9') return -1;
    if (minute > 59U || second > 59U || hour > 71U) return -1;
    *seconds_out = hour * 3600U + minute * 60U + second;
    return 0;
}

static int is_leap_year(unsigned int year) {
    return ((year % 4U) == 0U && (year % 100U) != 0U) || ((year % 400U) == 0U);
}

static int day_of_week_monday0(unsigned int yyyymmdd) {
    static const int table[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    unsigned int year = yyyymmdd / 10000U;
    unsigned int month = (yyyymmdd / 100U) % 100U;
    unsigned int day = yyyymmdd % 100U;
    unsigned int year_adj = year;
    int sunday0;
    if (month < 3U) year_adj -= 1U;
    sunday0 = (int)((year_adj + year_adj / 4U - year_adj / 100U + year_adj / 400U + (unsigned int)table[month - 1U] + day) % 7U);
    return (sunday0 + 6) % 7;
}

static unsigned int gtfs_route_type_to_mode(unsigned int route_type) {
    if (route_type == 0U || (route_type >= 900U && route_type < 1000U)) return 1U;
    if (route_type == 1U || (route_type >= 400U && route_type < 500U)) return 2U;
    if (route_type == 2U || (route_type >= 100U && route_type < 200U)) return 3U;
    if (route_type == 3U || (route_type >= 700U && route_type < 800U)) return 4U;
    return 5U;
}

static const char *gtfs_mode_name(unsigned int mode) {
    if (mode == 1U) return "tram";
    if (mode == 2U) return "subway";
    if (mode == 3U) return "rail";
    if (mode == 4U) return "bus";
    return "transit";
}

static int stop_list_reserve(GtfsStopList *list, unsigned int needed) {
    unsigned int capacity = list->capacity == 0U ? 2048U : list->capacity;
    GtfsStop *items;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == list->capacity) return 0;
    items = (GtfsStop *)rt_realloc(list->items, sizeof(*items) * (size_t)capacity);
    if (items == 0) return -1;
    list->items = items;
    list->capacity = capacity;
    return 0;
}

static void stop_list_destroy(GtfsStopList *list) {
    unsigned int index;
    for (index = 0U; index < list->count; ++index) {
        rt_free(list->items[index].id);
        rt_free(list->items[index].name);
    }
    rt_free(list->items);
    rt_memset(list, 0, sizeof(*list));
}

static int route_list_reserve(GtfsRouteList *list, unsigned int needed) {
    unsigned int capacity = list->capacity == 0U ? 512U : list->capacity;
    GtfsRoute *items;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == list->capacity) return 0;
    items = (GtfsRoute *)rt_realloc(list->items, sizeof(*items) * (size_t)capacity);
    if (items == 0) return -1;
    list->items = items;
    list->capacity = capacity;
    return 0;
}

static void route_list_destroy(GtfsRouteList *list) {
    unsigned int index;
    for (index = 0U; index < list->count; ++index) {
        rt_free(list->items[index].short_name);
        rt_free(list->items[index].long_name);
    }
    rt_free(list->items);
    rt_memset(list, 0, sizeof(*list));
}

static int load_gtfs_active_services(const RouteContext *context, GtfsStringMap *services) {
    const char *names[] = { "service_id", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday", "start_date", "end_date" };
    unsigned int indexes[10];
    unsigned int weekday = (unsigned int)day_of_week_monday0(context->depart_date);
    int fd = gtfs_open_file(context->gtfs_path, "calendar.txt");
    GtfsLineReader reader;
    char line[GTFS_LINE_CAPACITY];
    int read_result;
    if (fd < 0) return -1;
    gtfs_line_reader_init(&reader, fd);
    read_result = gtfs_read_line(&reader, line, sizeof(line));
    if (read_result <= 0 || csv_find_header_indexes(line, names, 10U, indexes) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (;;) {
        CsvField fields[10];
        unsigned int active_flag;
        unsigned int start_date;
        unsigned int end_date;
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result <= 0) break;
        csv_get_fields(line, indexes, 10U, fields);
        if (gtfs_parse_uint_field(fields[1U + weekday], &active_flag) != 0 || gtfs_parse_uint_field(fields[8], &start_date) != 0 || gtfs_parse_uint_field(fields[9], &end_date) != 0) continue;
        if (active_flag == 0U) continue;
        if (context->depart_date < start_date || context->depart_date > end_date) continue;
        if (gtfs_string_map_put(services, fields[0], 1U, 0) != 0) {
            (void)platform_close(fd);
            return -1;
        }
    }
    (void)platform_close(fd);

    fd = gtfs_open_file(context->gtfs_path, "calendar_dates.txt");
    if (fd >= 0) {
        const char *extra_names[] = { "service_id", "date", "exception_type" };
        unsigned int extra_indexes[3];
        gtfs_line_reader_init(&reader, fd);
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result > 0 && csv_find_header_indexes(line, extra_names, 3U, extra_indexes) == 0) {
            for (;;) {
                CsvField fields[3];
                unsigned int date_value;
                unsigned int exception_type;
                read_result = gtfs_read_line(&reader, line, sizeof(line));
                if (read_result <= 0) break;
                csv_get_fields(line, extra_indexes, 3U, fields);
                if (gtfs_parse_uint_field(fields[1], &date_value) != 0 || date_value != context->depart_date || gtfs_parse_uint_field(fields[2], &exception_type) != 0) continue;
                if (gtfs_string_map_put(services, fields[0], exception_type == 1U ? 1U : 0U, 0) != 0) {
                    (void)platform_close(fd);
                    return -1;
                }
            }
        }
        (void)platform_close(fd);
    }
    return 0;
}

static int load_gtfs_stops(const RouteContext *context, GtfsStopList *stops, GtfsStringMap *stop_index_map) {
    const char *names[] = { "stop_id", "stop_name", "stop_lat", "stop_lon" };
    unsigned int indexes[4];
    int fd = gtfs_open_file(context->gtfs_path, "stops.txt");
    GtfsLineReader reader;
    char line[GTFS_LINE_CAPACITY];
    int read_result;
    if (fd < 0) return -1;
    gtfs_line_reader_init(&reader, fd);
    read_result = gtfs_read_line(&reader, line, sizeof(line));
    if (read_result <= 0 || csv_find_header_indexes(line, names, 4U, indexes) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (;;) {
        CsvField fields[4];
        GtfsStop *stop;
        long long lat_nano;
        long long lon_nano;
        unsigned int from_m;
        unsigned int to_m;
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result <= 0) break;
        csv_get_fields(line, indexes, 4U, fields);
        if (fields[0].size == 0U) continue;
        if (parse_gtfs_coord_field(fields[2], &lat_nano) != 0 || parse_gtfs_coord_field(fields[3], &lon_nano) != 0) continue;
        if (stop_list_reserve(stops, stops->count + 1U) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        stop = &stops->items[stops->count];
        rt_memset(stop, 0, sizeof(*stop));
        stop->id = csv_field_copy(fields[0]);
        stop->name = fields[1].size != 0U ? csv_field_copy(fields[1]) : csv_field_copy(fields[0]);
        if (stop->id == 0 || stop->name == 0) {
            (void)platform_close(fd);
            return -1;
        }
        stop->lat_nano = lat_nano;
        stop->lon_nano = lon_nano;
        from_m = distance_m(context->from.lat_nano, context->from.lon_nano, lat_nano, lon_nano);
        to_m = distance_m(context->to.lat_nano, context->to.lon_nano, lat_nano, lon_nano);
        stop->walk_from_m = from_m;
        stop->walk_to_m = to_m;
        stop->walk_from_sec = (unsigned int)(((unsigned long long)from_m * 3600ULL + context->speed_m_per_hour - 1ULL) / context->speed_m_per_hour);
        stop->walk_to_sec = (unsigned int)(((unsigned long long)to_m * 3600ULL + context->speed_m_per_hour - 1ULL) / context->speed_m_per_hour);
        stop->origin_ok = from_m <= 5000U;
        stop->dest_ok = to_m <= 5000U;
        if (gtfs_string_map_put_raw(stop_index_map, stop->id, rt_strlen(stop->id), stops->count + 1U, 0) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        stops->count += 1U;
    }
    (void)platform_close(fd);
    return 0;
}

static int load_gtfs_routes(const RouteContext *context, GtfsRouteList *routes, GtfsStringMap *route_index_map) {
    const char *names[] = { "route_id", "route_short_name", "route_long_name", "route_type" };
    unsigned int indexes[4];
    int fd = gtfs_open_file(context->gtfs_path, "routes.txt");
    GtfsLineReader reader;
    char line[GTFS_LINE_CAPACITY];
    int read_result;
    if (fd < 0) return -1;
    gtfs_line_reader_init(&reader, fd);
    read_result = gtfs_read_line(&reader, line, sizeof(line));
    if (read_result <= 0 || csv_find_header_indexes(line, names, 4U, indexes) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (;;) {
        CsvField fields[4];
        GtfsRoute *route;
        unsigned int route_type;
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result <= 0) break;
        csv_get_fields(line, indexes, 4U, fields);
        if (fields[0].size == 0U || gtfs_parse_uint_field(fields[3], &route_type) != 0) continue;
        if (route_list_reserve(routes, routes->count + 1U) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        route = &routes->items[routes->count];
        route->short_name = fields[1].size != 0U ? csv_field_copy(fields[1]) : csv_field_copy(fields[0]);
        route->long_name = fields[2].size != 0U ? csv_field_copy(fields[2]) : csv_field_copy(fields[0]);
        route->mode = gtfs_route_type_to_mode(route_type);
        if (route->short_name == 0 || route->long_name == 0) {
            (void)platform_close(fd);
            return -1;
        }
        if (gtfs_string_map_put(route_index_map, fields[0], routes->count + 1U, 0) != 0) {
            (void)platform_close(fd);
            return -1;
        }
        routes->count += 1U;
    }
    (void)platform_close(fd);
    return 0;
}

static int load_gtfs_active_trips(const RouteContext *context, const GtfsStringMap *active_services, const GtfsStringMap *route_index_map, GtfsStringMap *trip_route_map) {
    const char *names[] = { "route_id", "service_id", "trip_id" };
    unsigned int indexes[3];
    int fd = gtfs_open_file(context->gtfs_path, "trips.txt");
    GtfsLineReader reader;
    char line[GTFS_LINE_CAPACITY];
    int read_result;
    if (fd < 0) return -1;
    gtfs_line_reader_init(&reader, fd);
    read_result = gtfs_read_line(&reader, line, sizeof(line));
    if (read_result <= 0 || csv_find_header_indexes(line, names, 3U, indexes) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    for (;;) {
        CsvField fields[3];
        unsigned int service_state;
        unsigned int route_index;
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result <= 0) break;
        csv_get_fields(line, indexes, 3U, fields);
        if (fields[2].size == 0U) continue;
        service_state = gtfs_string_map_get(active_services, fields[1]);
        if (service_state == 0U) continue;
        route_index = gtfs_string_map_get(route_index_map, fields[0]);
        if (route_index == 0U) continue;
        if (gtfs_string_map_put(trip_route_map, fields[2], route_index, 0) != 0) {
            (void)platform_close(fd);
            return -1;
        }
    }
    (void)platform_close(fd);
    return 0;
}

static int field_equals_saved_key(const CsvField *field, const char *saved, size_t saved_size) {
    return field->size == saved_size && memcmp(field->data, saved, saved_size) == 0;
}

static void write_hhmm(unsigned int seconds) {
    unsigned int day = seconds / 86400U;
    unsigned int in_day = seconds % 86400U;
    unsigned int hour = in_day / 3600U;
    unsigned int minute = (in_day / 60U) % 60U;
    if (hour < 10U) rt_write_char(1, '0');
    rt_write_uint(1, hour);
    rt_write_char(1, ':');
    if (minute < 10U) rt_write_char(1, '0');
    rt_write_uint(1, minute);
    if (day > 0U) {
        rt_write_cstr(1, " (+");
        rt_write_uint(1, day);
        rt_write_cstr(1, "d)");
    }
}

static int evaluate_gtfs_depart(const RouteContext *context, TransitPlan *plan) {
    const char *names[] = { "trip_id", "arrival_time", "departure_time", "stop_id", "stop_sequence" };
    unsigned int indexes[5];
    GtfsStringMap active_services;
    GtfsStringMap stop_index_map;
    GtfsStringMap route_index_map;
    GtfsStringMap trip_route_map;
    GtfsStopList stops;
    GtfsRouteList routes;
    int fd;
    GtfsLineReader reader;
    char line[GTFS_LINE_CAPACITY];
    int read_result;
    char current_trip_id[GTFS_KEY_CAPACITY];
    size_t current_trip_size = 0U;
    unsigned int current_route_index = 0U;
    int board_found = 0;
    unsigned int board_departure = 0U;
    unsigned int board_sequence = 0U;
    unsigned int board_stop_index = 0U;

    rt_memset(plan, 0, sizeof(*plan));
    rt_memset(&active_services, 0, sizeof(active_services));
    rt_memset(&stop_index_map, 0, sizeof(stop_index_map));
    rt_memset(&route_index_map, 0, sizeof(route_index_map));
    rt_memset(&trip_route_map, 0, sizeof(trip_route_map));
    rt_memset(&stops, 0, sizeof(stops));
    rt_memset(&routes, 0, sizeof(routes));

    if (load_gtfs_active_services(context, &active_services) != 0) goto fail;
    plan->debug_active_services = active_services.count;
    if (load_gtfs_stops(context, &stops, &stop_index_map) != 0) goto fail;
    {
        unsigned int idx;
        for (idx = 0U; idx < stops.count; ++idx) {
            if (stops.items[idx].origin_ok) plan->debug_candidate_origins += 1U;
            if (stops.items[idx].dest_ok) plan->debug_candidate_destinations += 1U;
        }
    }
    if (load_gtfs_routes(context, &routes, &route_index_map) != 0) goto fail;
    if (load_gtfs_active_trips(context, &active_services, &route_index_map, &trip_route_map) != 0) goto fail;
    plan->debug_active_trips = trip_route_map.count;

    fd = gtfs_open_file(context->gtfs_path, "stop_times.txt");
    if (fd < 0) goto fail;
    gtfs_line_reader_init(&reader, fd);
    read_result = gtfs_read_line(&reader, line, sizeof(line));
    if (read_result <= 0 || csv_find_header_indexes(line, names, 5U, indexes) != 0) {
        (void)platform_close(fd);
        goto fail;
    }
    for (;;) {
        CsvField fields[5];
        unsigned int route_index;
        unsigned int stop_index;
        unsigned int arrival_sec;
        unsigned int departure_sec;
        unsigned int sequence;
        read_result = gtfs_read_line(&reader, line, sizeof(line));
        if (read_result <= 0) break;
        csv_get_fields(line, indexes, 5U, fields);
        plan->debug_stop_times_scanned += 1ULL;
        if (fields[0].size == 0U || fields[3].size == 0U) continue;

        if (!field_equals_saved_key(&fields[0], current_trip_id, current_trip_size)) {
            board_found = 0;
            board_departure = 0U;
            board_sequence = 0U;
            board_stop_index = 0U;
            current_trip_size = fields[0].size;
            if (current_trip_size >= sizeof(current_trip_id)) current_trip_size = 0U;
            if (current_trip_size != 0U) memcpy(current_trip_id, fields[0].data, current_trip_size);
            route_index = gtfs_string_map_get(&trip_route_map, fields[0]);
            current_route_index = route_index;
        }
        if (current_route_index == 0U) continue;
        plan->debug_stop_times_trip_hits += 1ULL;
        stop_index = gtfs_string_map_get(&stop_index_map, fields[3]);
        if (stop_index == 0U) continue;
        if (gtfs_parse_time_field(fields[1], &arrival_sec) != 0 || gtfs_parse_time_field(fields[2], &departure_sec) != 0 || gtfs_parse_uint_field(fields[4], &sequence) != 0) continue;

        {
            GtfsStop *stop = &stops.items[stop_index - 1U];
            unsigned int earliest_board = context->depart_seconds + stop->walk_from_sec;
            if (stop->origin_ok && departure_sec >= earliest_board) {
                plan->debug_board_candidates += 1ULL;
                if (!board_found || departure_sec < board_departure) {
                    board_found = 1;
                    board_departure = departure_sec;
                    board_sequence = sequence;
                    board_stop_index = stop_index;
                }
            }
            if (board_found && stop->dest_ok && sequence > board_sequence && arrival_sec >= board_departure) {
                unsigned int total_arrival = arrival_sec + stop->walk_to_sec;
                if (!plan->found || total_arrival < plan->total_sec + context->depart_seconds) {
                    GtfsStop *board_stop = &stops.items[board_stop_index - 1U];
                    GtfsRoute *route = &routes.items[current_route_index - 1U];
                    plan->found = 1;
                    plan->mode = route->mode;
                    plan->line_short = route->short_name;
                    plan->line_long = route->long_name;
                    plan->board_stop = board_stop->name;
                    plan->alight_stop = stop->name;
                    plan->board_departure_sec = board_departure;
                    plan->alight_arrival_sec = arrival_sec;
                    plan->walk_to_stop_m = board_stop->walk_from_m;
                    plan->walk_from_stop_m = stop->walk_to_m;
                    plan->total_sec = total_arrival - context->depart_seconds;
                }
            }
        }
    }
    (void)platform_close(fd);

    gtfs_string_map_destroy(&trip_route_map);
    gtfs_string_map_destroy(&route_index_map);
    gtfs_string_map_destroy(&stop_index_map);
    gtfs_string_map_destroy(&active_services);
    route_list_destroy(&routes);
    stop_list_destroy(&stops);
    return 0;

fail:
    gtfs_string_map_destroy(&trip_route_map);
    gtfs_string_map_destroy(&route_index_map);
    gtfs_string_map_destroy(&stop_index_map);
    gtfs_string_map_destroy(&active_services);
    route_list_destroy(&routes);
    stop_list_destroy(&stops);
    return -1;
}

static int lower_ascii(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }

static int cstr_eq_ci(const char *left, const char *right) {
    size_t index = 0U;
    while (left[index] != '\0' && right[index] != '\0') {
        if (lower_ascii((unsigned char)left[index]) != lower_ascii((unsigned char)right[index])) return 0;
        index += 1U;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static void copy_cstr(char *out, size_t capacity, const char *text) {
    size_t index = 0U;
    if (capacity == 0U) return;
    while (index + 1U < capacity && text[index] != '\0') {
        out[index] = text[index];
        index += 1U;
    }
    out[index] = '\0';
}

static void copy_text(char *out, size_t capacity, PbfText text) {
    size_t size = text.size;
    if (capacity == 0U) return;
    if (size >= capacity) size = capacity - 1U;
    if (size != 0U) memcpy(out, text.data, size);
    out[size] = '\0';
}

static int text_eq(PbfText text, const char *value) {
    size_t size = rt_strlen(value);
    return text.size == size && memcmp(text.data, value, size) == 0;
}

static int text_eq_ci(PbfText text, const char *value) {
    size_t index;
    if (text.size != rt_strlen(value)) return 0;
    for (index = 0U; index < text.size; ++index) if (lower_ascii((unsigned char)text.data[index]) != lower_ascii((unsigned char)value[index])) return 0;
    return 1;
}

static int parse_coord_part(const char *text, size_t size, long long *value_out) {
    size_t index = 0U;
    unsigned long long whole = 0ULL;
    unsigned long long fraction = 0ULL;
    unsigned int fraction_digits = 0U;
    int negative = 0;

    if (size == 0U) return -1;
    if (text[index] == '-') { negative = 1; index += 1U; }
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
    while (fraction_digits < 9U) { fraction *= 10ULL; fraction_digits += 1U; }
    *value_out = (long long)(whole * 1000000000ULL + fraction);
    if (negative) *value_out = -*value_out;
    return 0;
}

static int parse_bbox(const char *text, RouteContext *context) {
    const char *part[4];
    size_t size[4];
    size_t start = 0U;
    size_t index = 0U;
    unsigned int count = 0U;

    for (;;) {
        if (text[index] == ',' || text[index] == '\0') {
            if (count >= 4U) return -1;
            part[count] = text + start;
            size[count] = index - start;
            count += 1U;
            if (text[index] == '\0') break;
            start = index + 1U;
        }
        index += 1U;
    }
    if (count != 4U) return -1;
    if (parse_coord_part(part[0], size[0], &context->min_lon_nano) != 0 || parse_coord_part(part[1], size[1], &context->min_lat_nano) != 0 ||
        parse_coord_part(part[2], size[2], &context->max_lon_nano) != 0 || parse_coord_part(part[3], size[3], &context->max_lat_nano) != 0) return -1;
    return context->min_lon_nano < context->max_lon_nano && context->min_lat_nano < context->max_lat_nano ? 0 : -1;
}

static int parse_speed(const char *text, unsigned long long *speed_out) {
    unsigned long long kmh;
    if (rt_parse_uint(text, &kmh) != 0 || kmh == 0ULL || kmh > 20ULL) return -1;
    *speed_out = kmh * 1000ULL;
    return 0;
}

static int parse_address(const char *text, AddressQuery *query) {
    size_t length = rt_strlen(text);
    size_t split;
    while (length > 0U && text[length - 1U] == ' ') length -= 1U;
    split = length;
    while (split > 0U && text[split - 1U] != ' ') split -= 1U;
    if (split == 0U || split >= length || split >= sizeof(query->street) || length - split >= sizeof(query->house)) return -1;
    memcpy(query->street, text, split - 1U);
    query->street[split - 1U] = '\0';
    memcpy(query->house, text + split, length - split);
    query->house[length - split] = '\0';
    query->input = text;
    return query->street[0] != '\0' && query->house[0] != '\0' ? 0 : -1;
}

static unsigned int hash_i64(long long value) {
    unsigned long long key = (unsigned long long)value;
    key ^= key >> 33U; key *= 0xff51afd7ed558ccdULL; key ^= key >> 33U; key *= 0xc4ceb9fe1a85ec53ULL; key ^= key >> 33U;
    return (unsigned int)key;
}

static int coord_store_reserve(RouteCoordStore *store, unsigned int needed) {
    unsigned int capacity = store->capacity == 0U ? 4096U : store->capacity;
    RouteCoord *items;
    while (capacity < needed) { if (capacity > 0x40000000U) return -1; capacity *= 2U; }
    if (capacity == store->capacity) return 0;
    items = (RouteCoord *)rt_realloc(store->items, sizeof(*items) * (size_t)capacity);
    if (items == 0) return -1;
    store->items = items;
    store->capacity = capacity;
    return 0;
}

static int coord_slots_reserve(RouteCoordStore *store, unsigned int needed) {
    unsigned int capacity = store->slot_capacity == 0U ? 8192U : store->slot_capacity;
    RouteCoordSlot *slots;
    unsigned int old_capacity = store->slot_capacity;
    unsigned int index;

    while (capacity * 7U / 10U < needed) { if (capacity > 0x40000000U) return -1; capacity *= 2U; }
    if (capacity == store->slot_capacity) return 0;
    slots = (RouteCoordSlot *)rt_malloc(sizeof(*slots) * (size_t)capacity);
    if (slots == 0) return -1;
    rt_memset(slots, 0, sizeof(*slots) * (size_t)capacity);
    for (index = 0U; index < old_capacity; ++index) {
        if (store->slots[index].used) {
            unsigned int slot = hash_i64(store->slots[index].id) & (capacity - 1U);
            while (slots[slot].used) slot = (slot + 1U) & (capacity - 1U);
            slots[slot] = store->slots[index];
        }
    }
    rt_free(store->slots);
    store->slots = slots;
    store->slot_capacity = capacity;
    return 0;
}

static RouteCoord *coord_find(const RouteCoordStore *store, long long id) {
    unsigned int slot;
    if (store->slot_capacity == 0U) return 0;
    slot = hash_i64(id) & (store->slot_capacity - 1U);
    for (;;) {
        if (!store->slots[slot].used) return 0;
        if (store->slots[slot].id == id) return (RouteCoord *)&store->items[store->slots[slot].index];
        slot = (slot + 1U) & (store->slot_capacity - 1U);
    }
}

static int coord_add(RouteCoordStore *store, long long id, long long lat_nano, long long lon_nano) {
    unsigned int slot;
    if (coord_find(store, id) != 0) return 0;
    if (coord_store_reserve(store, store->count + 1U) != 0 || coord_slots_reserve(store, store->count + 1U) != 0) return -1;
    store->items[store->count].id = id;
    store->items[store->count].lat_nano = lat_nano;
    store->items[store->count].lon_nano = lon_nano;
    store->items[store->count].graph_index = -1;
    slot = hash_i64(id) & (store->slot_capacity - 1U);
    while (store->slots[slot].used) slot = (slot + 1U) & (store->slot_capacity - 1U);
    store->slots[slot].id = id;
    store->slots[slot].index = store->count;
    store->slots[slot].used = 1;
    store->count += 1U;
    return 0;
}

static int graph_reserve_nodes(RouteGraph *graph, unsigned int needed) {
    unsigned int capacity = graph->node_capacity == 0U ? 4096U : graph->node_capacity;
    RouteNode *nodes;
    while (capacity < needed) { if (capacity > 0x40000000U) return -1; capacity *= 2U; }
    if (capacity == graph->node_capacity) return 0;
    nodes = (RouteNode *)rt_realloc(graph->nodes, sizeof(*nodes) * (size_t)capacity);
    if (nodes == 0) return -1;
    graph->nodes = nodes;
    graph->node_capacity = capacity;
    return 0;
}

static int graph_reserve_edges(RouteGraph *graph, unsigned int needed) {
    unsigned int capacity = graph->edge_capacity == 0U ? 8192U : graph->edge_capacity;
    RouteEdge *edges;
    while (capacity < needed) { if (capacity > 0x40000000U) return -1; capacity *= 2U; }
    if (capacity == graph->edge_capacity) return 0;
    edges = (RouteEdge *)rt_realloc(graph->edges, sizeof(*edges) * (size_t)capacity);
    if (edges == 0) return -1;
    graph->edges = edges;
    graph->edge_capacity = capacity;
    return 0;
}

static int graph_node_for_coord(RouteGraph *graph, RouteCoord *coord) {
    unsigned int index;
    if (coord->graph_index >= 0) return coord->graph_index;
    if (graph_reserve_nodes(graph, graph->node_count + 1U) != 0) return -1;
    index = graph->node_count++;
    graph->nodes[index].id = coord->id;
    graph->nodes[index].lat_nano = coord->lat_nano;
    graph->nodes[index].lon_nano = coord->lon_nano;
    graph->nodes[index].first_edge = -1;
    graph->nodes[index].distance = INF_DISTANCE;
    graph->nodes[index].previous = -1;
    graph->nodes[index].previous_edge = -1;
    graph->nodes[index].settled = 0;
    coord->graph_index = (int)index;
    return (int)index;
}

static unsigned long long abs_i64(long long value) { return value < 0 ? (unsigned long long)(-value) : (unsigned long long)value; }

static unsigned long long isqrt_u64(unsigned long long value) {
    unsigned long long result = 0ULL;
    unsigned long long bit = 1ULL << 62U;
    while (bit > value) bit >>= 2U;
    while (bit != 0ULL) {
        if (value >= result + bit) { value -= result + bit; result = (result >> 1U) + bit; } else { result >>= 1U; }
        bit >>= 2U;
    }
    return result;
}

static unsigned int distance_m(long long lat_a, long long lon_a, long long lat_b, long long lon_b) {
    unsigned long long dx = (abs_i64(lon_a - lon_b) * 68000ULL + 500000000ULL) / 1000000000ULL;
    unsigned long long dy = (abs_i64(lat_a - lat_b) * 111320ULL + 500000000ULL) / 1000000000ULL;
    unsigned long long distance = isqrt_u64(dx * dx + dy * dy);
    if (distance == 0ULL) distance = 1ULL;
    return distance > 0xffffffffULL ? 0xffffffffU : (unsigned int)distance;
}

static int graph_add_directed(RouteGraph *graph, int from, int to, unsigned int meters, const char *label) {
    unsigned int index;
    if (from == to) return 0;
    if (graph_reserve_edges(graph, graph->edge_count + 1U) != 0) return -1;
    index = graph->edge_count++;
    graph->edges[index].to = to;
    graph->edges[index].meters = meters;
    copy_cstr(graph->edges[index].label, sizeof(graph->edges[index].label), label);
    graph->edges[index].next = graph->nodes[from].first_edge;
    graph->nodes[from].first_edge = (int)index;
    return 0;
}

static int graph_add_edge(RouteGraph *graph, int from, int to, const char *label) {
    unsigned int meters = distance_m(graph->nodes[from].lat_nano, graph->nodes[from].lon_nano, graph->nodes[to].lat_nano, graph->nodes[to].lon_nano);
    return graph_add_directed(graph, from, to, meters, label) != 0 || graph_add_directed(graph, to, from, meters, label) != 0 ? -1 : 0;
}

static int way_segment_reserve(WaySegmentStore *store, unsigned int needed) {
    unsigned int capacity = store->capacity == 0U ? 8192U : store->capacity;
    WaySegment *items;
    while (capacity < needed) {
        if (capacity > 0x40000000U) return -1;
        capacity *= 2U;
    }
    if (capacity == store->capacity) return 0;
    items = (WaySegment *)rt_realloc(store->items, sizeof(*items) * (size_t)capacity);
    if (items == 0) return -1;
    store->items = items;
    store->capacity = capacity;
    return 0;
}

static int way_segment_add(WaySegmentStore *store, unsigned int left_coord_index, unsigned int right_coord_index, const char *label) {
    WaySegment *segment;
    if (left_coord_index == right_coord_index) return 0;
    if (way_segment_reserve(store, store->count + 1U) != 0) return -1;
    segment = &store->items[store->count++];
    segment->left_coord_index = left_coord_index;
    segment->right_coord_index = right_coord_index;
    copy_cstr(segment->label, sizeof(segment->label), label);
    return 0;
}

static int way_segment_store_append(WaySegmentStore *dst, const WaySegmentStore *src) {
    unsigned int index;
    if (src->count == 0U) return 0;
    if (way_segment_reserve(dst, dst->count + src->count) != 0) return -1;
    for (index = 0U; index < src->count; ++index) dst->items[dst->count++] = src->items[index];
    return 0;
}

static int materialize_way_segments(RouteContext *context) {
    unsigned int index;
    if (context->way_segments.count > 0x7fffffffU || graph_reserve_edges(&context->graph, context->way_segments.count * 2U) != 0) return -1;
    for (index = 0U; index < context->way_segments.count; ++index) {
        WaySegment *segment = &context->way_segments.items[index];
        RouteCoord *left_coord = &context->coords.items[segment->left_coord_index];
        RouteCoord *right_coord = &context->coords.items[segment->right_coord_index];
        int left = graph_node_for_coord(&context->graph, left_coord);
        int right = graph_node_for_coord(&context->graph, right_coord);
        if (left < 0 || right < 0 || graph_add_edge(&context->graph, left, right, segment->label) != 0) return -1;
    }
    return 0;
}

static int coord_in_bbox(const RouteContext *context, long long lat_nano, long long lon_nano) {
    return lon_nano >= context->min_lon_nano && lon_nano <= context->max_lon_nano && lat_nano >= context->min_lat_nano && lat_nano <= context->max_lat_nano;
}

static PbfText tag_value(const PbfTag *tags, unsigned int tag_count, const char *key) {
    PbfText empty;
    unsigned int index;
    empty.data = "";
    empty.size = 0U;
    for (index = 0U; index < tag_count; ++index) if (text_eq(tags[index].key, key)) return tags[index].value;
    return empty;
}

static void find_address_tags(const PbfTag *tags, unsigned int tag_count, AddressTags *address) {
    rt_memset(address, 0, sizeof(*address));
    address->street = tag_value(tags, tag_count, "addr:street");
    address->house = tag_value(tags, tag_count, "addr:housenumber");
    address->city = tag_value(tags, tag_count, "addr:city");
    address->has_street = address->street.size != 0U;
    address->has_house = address->house.size != 0U;
    address->has_city = address->city.size != 0U;
}

static int address_matches(const RouteContext *context, const AddressTags *address, const AddressQuery *query) {
    const char *city_name = context->city_name;
    if (!address->has_street || !address->has_house) return 0;
    if (!text_eq_ci(address->street, query->street) || !text_eq_ci(address->house, query->house)) return 0;
    if (address->has_city && city_name != 0 && !text_eq_ci(address->city, city_name)) return 0;
    return 1;
}

static int address_matches_city(const char *city_name, const AddressTags *address, const AddressQuery *query) {
    if (!address->has_street || !address->has_house) return 0;
    if (!text_eq_ci(address->street, query->street) || !text_eq_ci(address->house, query->house)) return 0;
    if (address->has_city && city_name != 0 && !text_eq_ci(address->city, city_name)) return 0;
    return 1;
}

static void store_matching_address(RouteContext *context, const AddressTags *address, const char *source, long long id, long long lat_nano, long long lon_nano) {
    if (!context->from.found && address_matches(context, address, &context->from)) {
        context->from.lat_nano = lat_nano; context->from.lon_nano = lon_nano; context->from.osm_id = id; context->from.source_type = source; context->from.found = 1;
    }
    if (!context->to.found && address_matches(context, address, &context->to)) {
        context->to.lat_nano = lat_nano; context->to.lon_nano = lon_nano; context->to.osm_id = id; context->to.source_type = source; context->to.found = 1;
    }
}

static void copy_query_seed(AddressQuery *dst, const AddressQuery *src) {
    *dst = *src;
    dst->lat_nano = 0;
    dst->lon_nano = 0;
    dst->osm_id = 0;
    dst->source_type = 0;
    dst->found = 0;
}

static int on_node(void *user, const PbfNode *node) {
    RouteContext *context = (RouteContext *)user;
    AddressTags address;
    if (!coord_in_bbox(context, node->lat_nano, node->lon_nano)) return 0;
    context->bbox_nodes += 1ULL;
    if (coord_add(&context->coords, node->id, node->lat_nano, node->lon_nano) != 0) return 1;
    find_address_tags(node->tags, node->tag_count, &address);
    if (address.has_street && address.has_house) { context->address_nodes += 1ULL; store_matching_address(context, &address, "node", node->id, node->lat_nano, node->lon_nano); }
    return 0;
}

static int text_one_of(PbfText text, const char *a, const char *b, const char *c, const char *d) {
    return (a != 0 && text_eq_ci(text, a)) || (b != 0 && text_eq_ci(text, b)) || (c != 0 && text_eq_ci(text, c)) || (d != 0 && text_eq_ci(text, d));
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

static void way_label(const PbfTag *tags, unsigned int tag_count, char *out, size_t capacity) {
    PbfText name = tag_value(tags, tag_count, "name");
    PbfText ref = tag_value(tags, tag_count, "ref");
    PbfText highway = tag_value(tags, tag_count, "highway");
    if (name.size != 0U) {
        copy_text(out, capacity, name);
    } else if (ref.size != 0U) {
        copy_text(out, capacity, ref);
    } else if (text_eq(highway, "service")) {
        copy_cstr(out, capacity, "an unnamed service road");
    } else if (text_eq(highway, "footway")) {
        copy_cstr(out, capacity, "an unnamed footway");
    } else if (text_eq(highway, "path")) {
        copy_cstr(out, capacity, "an unnamed path");
    } else if (text_eq(highway, "steps")) {
        copy_cstr(out, capacity, "unnamed steps");
    } else if (text_eq(highway, "track")) {
        copy_cstr(out, capacity, "an unnamed track");
    } else if (highway.size != 0U) {
        copy_cstr(out, capacity, "an unnamed ");
        copy_text(out + rt_strlen(out), capacity - rt_strlen(out), highway);
        copy_cstr(out + rt_strlen(out), capacity - rt_strlen(out), " road");
    } else {
        copy_cstr(out, capacity, "an unnamed walkable way");
    }
}

static int should_print_snap(unsigned int meters) {
    return meters > 10U;
}

static void maybe_way_address(RouteContext *context, const PbfWay *way, const AddressTags *address) {
    long long lat_sum = 0;
    long long lon_sum = 0;
    unsigned int count = 0U;
    unsigned int index;
    if (!address->has_street || !address->has_house) return;
    context->address_ways += 1ULL;
    if ((context->from.found || !address_matches(context, address, &context->from)) && (context->to.found || !address_matches(context, address, &context->to))) return;
    for (index = 0U; index < way->ref_count; ++index) {
        RouteCoord *coord = coord_find(&context->coords, way->refs[index]);
        if (coord != 0) { lat_sum += coord->lat_nano; lon_sum += coord->lon_nano; count += 1U; }
    }
    if (count != 0U) store_matching_address(context, address, "way", way->id, lat_sum / (long long)count, lon_sum / (long long)count);
}

static void maybe_way_address_worker(WayWorkerContext *worker, const PbfWay *way, const AddressTags *address) {
    long long lat_sum = 0;
    long long lon_sum = 0;
    unsigned int count = 0U;
    unsigned int index;
    int from_match;
    int to_match;
    if (!address->has_street || !address->has_house) return;
    worker->address_ways += 1ULL;
    from_match = !worker->shared->from.found && !worker->from.found && address_matches_city(worker->shared->city_name, address, &worker->from);
    to_match = !worker->shared->to.found && !worker->to.found && address_matches_city(worker->shared->city_name, address, &worker->to);
    if (!from_match && !to_match) return;
    for (index = 0U; index < way->ref_count; ++index) {
        RouteCoord *coord = coord_find(&worker->shared->coords, way->refs[index]);
        if (coord != 0) {
            lat_sum += coord->lat_nano;
            lon_sum += coord->lon_nano;
            count += 1U;
        }
    }
    if (count == 0U) return;
    if (from_match) {
        worker->from.lat_nano = lat_sum / (long long)count;
        worker->from.lon_nano = lon_sum / (long long)count;
        worker->from.osm_id = way->id;
        worker->from.source_type = "way";
        worker->from.found = 1;
    }
    if (to_match) {
        worker->to.lat_nano = lat_sum / (long long)count;
        worker->to.lon_nano = lon_sum / (long long)count;
        worker->to.osm_id = way->id;
        worker->to.source_type = "way";
        worker->to.found = 1;
    }
}

static int on_way_worker(void *user, const PbfWay *way) {
    WayWorkerContext *worker = (WayWorkerContext *)user;
    AddressTags address;
    char label[ROUTE_LABEL_CAPACITY];
    unsigned int index;
    worker->ways_seen += 1ULL;
    find_address_tags(way->tags, way->tag_count, &address);
    maybe_way_address_worker(worker, way, &address);
    if (!way_walkable(way->tags, way->tag_count)) return 0;
    worker->walkable_ways += 1ULL;
    way_label(way->tags, way->tag_count, label, sizeof(label));
    for (index = 0U; index + 1U < way->ref_count; ++index) {
        RouteCoord *left_coord = coord_find(&worker->shared->coords, way->refs[index]);
        RouteCoord *right_coord = coord_find(&worker->shared->coords, way->refs[index + 1U]);
        unsigned int left_index;
        unsigned int right_index;
        if (left_coord == 0 || right_coord == 0) continue;
        left_index = (unsigned int)(left_coord - worker->shared->coords.items);
        right_index = (unsigned int)(right_coord - worker->shared->coords.items);
        if (way_segment_add(&worker->segments, left_index, right_index, label) != 0) return 1;
    }
    return 0;
}

static int way_worker_init(void *worker_user, unsigned int worker_index, void *shared_user) {
    WayWorkerContext *worker = (WayWorkerContext *)worker_user;
    RouteContext *context = (RouteContext *)shared_user;
    (void)worker_index;
    rt_memset(worker, 0, sizeof(*worker));
    worker->shared = context;
    copy_query_seed(&worker->from, &context->from);
    copy_query_seed(&worker->to, &context->to);
    return 0;
}

static int way_worker_merge(void *shared_user, void *worker_user) {
    RouteContext *context = (RouteContext *)shared_user;
    WayWorkerContext *worker = (WayWorkerContext *)worker_user;
    context->ways_seen += worker->ways_seen;
    context->walkable_ways += worker->walkable_ways;
    context->address_ways += worker->address_ways;
    if (way_segment_store_append(&context->way_segments, &worker->segments) != 0) return -1;
    if (!context->from.found && worker->from.found) {
        context->from.lat_nano = worker->from.lat_nano;
        context->from.lon_nano = worker->from.lon_nano;
        context->from.osm_id = worker->from.osm_id;
        context->from.source_type = worker->from.source_type;
        context->from.found = 1;
    }
    if (!context->to.found && worker->to.found) {
        context->to.lat_nano = worker->to.lat_nano;
        context->to.lon_nano = worker->to.lon_nano;
        context->to.osm_id = worker->to.osm_id;
        context->to.source_type = worker->to.source_type;
        context->to.found = 1;
    }
    return 0;
}

static void way_worker_destroy(void *worker_user) {
    WayWorkerContext *worker = (WayWorkerContext *)worker_user;
    rt_free(worker->segments.items);
    worker->segments.items = 0;
}

static int on_way(void *user, const PbfWay *way) {
    RouteContext *context = (RouteContext *)user;
    AddressTags address;
    char label[ROUTE_LABEL_CAPACITY];
    unsigned int index;
    context->ways_seen += 1ULL;
    find_address_tags(way->tags, way->tag_count, &address);
    maybe_way_address(context, way, &address);
    if (!way_walkable(way->tags, way->tag_count)) return 0;
    context->walkable_ways += 1ULL;
    way_label(way->tags, way->tag_count, label, sizeof(label));
    for (index = 0U; index + 1U < way->ref_count; ++index) {
        RouteCoord *left_coord = coord_find(&context->coords, way->refs[index]);
        RouteCoord *right_coord = coord_find(&context->coords, way->refs[index + 1U]);
        int left;
        int right;
        if (left_coord == 0 || right_coord == 0) continue;
        left = graph_node_for_coord(&context->graph, left_coord);
        right = graph_node_for_coord(&context->graph, right_coord);
        if (left < 0 || right < 0 || graph_add_edge(&context->graph, left, right, label) != 0) return 1;
    }
    return 0;
}

static int heap_reserve(RouteHeap *heap, unsigned int needed) {
    unsigned int capacity = heap->capacity == 0U ? 1024U : heap->capacity;
    RouteHeapItem *items;
    while (capacity < needed) { if (capacity > 0x40000000U) return -1; capacity *= 2U; }
    if (capacity == heap->capacity) return 0;
    items = (RouteHeapItem *)rt_realloc(heap->items, sizeof(*items) * (size_t)capacity);
    if (items == 0) return -1;
    heap->items = items;
    heap->capacity = capacity;
    return 0;
}

static int heap_push(RouteHeap *heap, int node, unsigned long long distance) {
    unsigned int index;
    if (heap_reserve(heap, heap->count + 1U) != 0) return -1;
    index = heap->count++;
    while (index > 0U) {
        unsigned int parent = (index - 1U) / 2U;
        if (heap->items[parent].distance <= distance) break;
        heap->items[index] = heap->items[parent];
        index = parent;
    }
    heap->items[index].node = node;
    heap->items[index].distance = distance;
    return 0;
}

static int heap_pop(RouteHeap *heap, RouteHeapItem *out) {
    RouteHeapItem tail;
    unsigned int index = 0U;
    if (heap->count == 0U) return 0;
    *out = heap->items[0];
    tail = heap->items[--heap->count];
    while (index * 2U + 1U < heap->count) {
        unsigned int child = index * 2U + 1U;
        if (child + 1U < heap->count && heap->items[child + 1U].distance < heap->items[child].distance) child += 1U;
        if (heap->items[child].distance >= tail.distance) break;
        heap->items[index] = heap->items[child];
        index = child;
    }
    if (heap->count != 0U) heap->items[index] = tail;
    return 1;
}

static int nearest_node(RouteGraph *graph, long long lat_nano, long long lon_nano, unsigned int *snap_m_out) {
    unsigned int index;
    int best = -1;
    unsigned int best_m = 0xffffffffU;
    for (index = 0U; index < graph->node_count; ++index) {
        unsigned int meters = distance_m(lat_nano, lon_nano, graph->nodes[index].lat_nano, graph->nodes[index].lon_nano);
        if (meters < best_m) { best_m = meters; best = (int)index; }
    }
    *snap_m_out = best_m;
    return best;
}

static int dijkstra(RouteGraph *graph, int start, int target) {
    RouteHeap heap;
    RouteHeapItem item;
    unsigned int index;
    rt_memset(&heap, 0, sizeof(heap));
    for (index = 0U; index < graph->node_count; ++index) { graph->nodes[index].distance = INF_DISTANCE; graph->nodes[index].previous = -1; graph->nodes[index].previous_edge = -1; graph->nodes[index].settled = 0; }
    graph->nodes[start].distance = 0ULL;
    if (heap_push(&heap, start, 0ULL) != 0) return -1;
    while (heap_pop(&heap, &item)) {
        int edge_index;
        if (graph->nodes[item.node].settled) continue;
        graph->nodes[item.node].settled = 1;
        if (item.node == target) { rt_free(heap.items); return 1; }
        for (edge_index = graph->nodes[item.node].first_edge; edge_index >= 0; edge_index = graph->edges[edge_index].next) {
            RouteEdge *edge = &graph->edges[edge_index];
            unsigned long long next_distance = graph->nodes[item.node].distance + (unsigned long long)edge->meters;
            if (next_distance < graph->nodes[edge->to].distance) {
                graph->nodes[edge->to].distance = next_distance;
                graph->nodes[edge->to].previous = item.node;
                graph->nodes[edge->to].previous_edge = edge_index;
                if (heap_push(&heap, edge->to, next_distance) != 0) { rt_free(heap.items); return -1; }
            }
        }
    }
    rt_free(heap.items);
    return 0;
}

static void write_coord(long long nano) {
    unsigned long long value;
    unsigned long long whole;
    unsigned long long fraction;
    unsigned long long divisor = 100000000ULL;
    if (nano < 0) { rt_write_char(1, '-'); value = (unsigned long long)(-nano); } else { value = (unsigned long long)nano; }
    whole = value / 1000000000ULL;
    fraction = value % 1000000000ULL;
    rt_write_uint(1, whole);
    rt_write_char(1, '.');
    while (divisor != 0ULL) { rt_write_char(1, (char)('0' + (fraction / divisor) % 10ULL)); divisor /= 10ULL; }
}

static void write_address(const char *label, const AddressQuery *query) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, query->input);
    rt_write_cstr(1, " source=");
    rt_write_cstr(1, query->source_type == 0 ? "unknown" : query->source_type);
    rt_write_cstr(1, " id=");
    if (query->osm_id < 0) { rt_write_char(1, '-'); rt_write_uint(1, (unsigned long long)(-query->osm_id)); } else { rt_write_uint(1, (unsigned long long)query->osm_id); }
    rt_write_cstr(1, " lat=");
    write_coord(query->lat_nano);
    rt_write_cstr(1, " lon=");
    write_coord(query->lon_nano);
    rt_write_char(1, '\n');
}

static void write_address_colored(const RouteContext *context, const char *label, const AddressQuery *query) {
    rt_write_cstr(1, label);
    write_colored_cstr(context, "\033[1;36m", query->input);
    rt_write_cstr(1, " source=");
    rt_write_cstr(1, query->source_type == 0 ? "unknown" : query->source_type);
    rt_write_cstr(1, " id=");
    if (query->osm_id < 0) { rt_write_char(1, '-'); rt_write_uint(1, (unsigned long long)(-query->osm_id)); } else { rt_write_uint(1, (unsigned long long)query->osm_id); }
    rt_write_cstr(1, " lat=");
    write_coord(query->lat_nano);
    rt_write_cstr(1, " lon=");
    write_coord(query->lon_nano);
    rt_write_char(1, '\n');
}

static int write_geometry(RouteGraph *graph, int target) {
    int *route;
    unsigned int count = 0U;
    unsigned int index;
    int node = target;
    while (node >= 0) { count += 1U; node = graph->nodes[node].previous; }
    route = (int *)rt_malloc(sizeof(*route) * (size_t)count);
    if (route == 0) return -1;
    node = target;
    for (index = 0U; index < count; ++index) { route[index] = node; node = graph->nodes[node].previous; }
    rt_write_cstr(1, "geometry:\n");
    for (index = count; index > 0U; --index) { RouteNode *point = &graph->nodes[route[index - 1U]]; write_coord(point->lat_nano); rt_write_char(1, ','); write_coord(point->lon_nano); rt_write_char(1, '\n'); }
    rt_free(route);
    return 0;
}

static void write_step_prefix(unsigned int *step) {
    rt_write_uint(1, *step);
    rt_write_cstr(1, ". ");
    *step += 1U;
}

static void write_step_prefix_colored(const RouteContext *context, unsigned int *step) {
    write_color(context, "\033[1;34m");
    rt_write_uint(1, *step);
    rt_write_cstr(1, ". ");
    write_color_reset(context);
    *step += 1U;
}

static void write_route_distance(unsigned long long meters) {
    rt_write_uint(1, meters);
    rt_write_cstr(1, " m");
}

static int write_plain_route(RouteContext *context, int target_node, unsigned int start_snap_m, unsigned int target_snap_m) {
    RouteGraph *graph = &context->graph;
    int *route;
    unsigned int count = 0U;
    unsigned int index;
    unsigned int step = 1U;
    int node = target_node;

    while (node >= 0) { count += 1U; node = graph->nodes[node].previous; }
    route = (int *)rt_malloc(sizeof(*route) * (size_t)count);
    if (route == 0) return -1;
    node = target_node;
    for (index = 0U; index < count; ++index) { route[index] = node; node = graph->nodes[node].previous; }

    write_colored_cstr(context, "\033[1;35m", "directions");
    rt_write_cstr(1, ":\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Start at ");
    write_colored_cstr(context, "\033[1;36m", context->from.input);
    rt_write_cstr(1, ".\n");
    if (should_print_snap(start_snap_m)) {
        write_step_prefix_colored(context, &step);
        rt_write_cstr(1, "Walk ");
        write_route_distance(start_snap_m);
        rt_write_cstr(1, " to the nearest walkable connection.\n");
    }
    index = count - 1U;
    while (index > 0U) {
        RouteEdge *edge = &graph->edges[graph->nodes[route[index - 1U]].previous_edge];
        const char *label = edge->label;
        unsigned long long meters = (unsigned long long)edge->meters;
        index -= 1U;
        while (index > 0U) {
            RouteEdge *next_edge = &graph->edges[graph->nodes[route[index - 1U]].previous_edge];
            if (rt_strcmp(label, next_edge->label) != 0) break;
            meters += (unsigned long long)next_edge->meters;
            index -= 1U;
        }
        write_step_prefix_colored(context, &step);
        rt_write_cstr(1, "Follow ");
        write_colored_cstr(context, "\033[36m", label);
        rt_write_cstr(1, " for ");
        write_route_distance(meters);
        rt_write_cstr(1, ".\n");
    }
    if (should_print_snap(target_snap_m)) {
        write_step_prefix_colored(context, &step);
        rt_write_cstr(1, "Walk ");
        write_route_distance(target_snap_m);
        rt_write_cstr(1, " from the walking network to ");
        write_colored_cstr(context, "\033[1;36m", context->to.input);
        rt_write_cstr(1, ".\n");
    }
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Arrive at ");
    write_colored_cstr(context, "\033[1;36m", context->to.input);
    rt_write_cstr(1, ".\n");
    rt_free(route);
    return 0;
}

static void write_transit_line_name(const TransitPlan *plan) {
    rt_write_cstr(1, gtfs_mode_name(plan->mode));
    if (plan->line_short != 0 && plan->line_short[0] != '\0') {
        rt_write_char(1, ' ');
        rt_write_cstr(1, plan->line_short);
    }
}

static void write_transit_line_name_colored(const RouteContext *context, const TransitPlan *plan) {
    write_color(context, "\033[1;32m");
    write_transit_line_name(plan);
    write_color_reset(context);
}

static int write_transit_route(RouteContext *context, const TransitPlan *plan) {
    unsigned int step = 1U;
    write_colored_cstr(context, "\033[1;35m", "directions");
    rt_write_cstr(1, ":\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Start at ");
    write_colored_cstr(context, "\033[1;36m", context->from.input);
    rt_write_cstr(1, ".\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Walk about ");
    write_route_distance(plan->walk_to_stop_m);
    rt_write_cstr(1, " to ");
    write_colored_cstr(context, "\033[1;36m", plan->board_stop == 0 ? "the departure stop" : plan->board_stop);
    rt_write_cstr(1, ".\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Take ");
    write_transit_line_name_colored(context, plan);
    rt_write_cstr(1, " from ");
    write_colored_cstr(context, "\033[1;36m", plan->board_stop == 0 ? "the departure stop" : plan->board_stop);
    rt_write_cstr(1, " at ");
    write_color(context, "\033[1;33m");
    write_hhmm(plan->board_departure_sec);
    write_color_reset(context);
    rt_write_cstr(1, " to ");
    write_colored_cstr(context, "\033[1;36m", plan->alight_stop == 0 ? "the arrival stop" : plan->alight_stop);
    rt_write_cstr(1, ", arriving at ");
    write_color(context, "\033[1;33m");
    write_hhmm(plan->alight_arrival_sec);
    write_color_reset(context);
    rt_write_cstr(1, ".\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Walk about ");
    write_route_distance(plan->walk_from_stop_m);
    rt_write_cstr(1, " from ");
    write_colored_cstr(context, "\033[1;36m", plan->alight_stop == 0 ? "the arrival stop" : plan->alight_stop);
    rt_write_cstr(1, " to ");
    write_colored_cstr(context, "\033[1;36m", context->to.input);
    rt_write_cstr(1, ".\n");
    write_step_prefix_colored(context, &step);
    rt_write_cstr(1, "Arrive at ");
    write_colored_cstr(context, "\033[1;36m", context->to.input);
    rt_write_cstr(1, ".\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmwalkroute";
    RouteContext context;
    PbfStreamCallbacks callbacks;
    char error[PBF_ERROR_CAPACITY];
    int argi;
    int start_node;
    int target_node;
    unsigned int start_snap_m;
    unsigned int target_snap_m;
    unsigned long long total_m;
    unsigned long long seconds;
    int route_result;
    TransitPlan transit_plan;

    if (argc < 4 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) { write_usage(program); return argc == 2 ? 0 : 1; }
    rt_memset(&context, 0, sizeof(context));
    context.pbf_path = argv[1];
    context.city_name = "Potsdam";
    context.speed_m_per_hour = 4800ULL;
    context.thread_count = 1U;
    context.use_color = 1;
    if (parse_address(argv[2], &context.from) != 0 || parse_address(argv[3], &context.to) != 0 || parse_bbox(POTSDAM_BBOX, &context) != 0) { write_usage(program); return 1; }
    argi = 4;
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--city") == 0) {
            argi += 1;
            if (argi >= argc || !cstr_eq_ci(argv[argi], "Potsdam") || parse_bbox(POTSDAM_BBOX, &context) != 0) { write_usage(program); return 1; }
            context.city_name = "Potsdam";
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--bbox") == 0) {
            argi += 1;
            if (argi >= argc || parse_bbox(argv[argi], &context) != 0) { write_usage(program); return 1; }
            context.city_name = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--speed-kmh") == 0) {
            argi += 1;
            if (argi >= argc || parse_speed(argv[argi], &context.speed_m_per_hour) != 0) { write_usage(program); return 1; }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--threads") == 0) {
            argi += 1;
            if (argi >= argc || parse_threads(argv[argi], &context.thread_count) != 0) { write_usage(program); return 1; }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--gtfs") == 0) {
            argi += 1;
            if (argi >= argc) { write_usage(program); return 1; }
            context.gtfs_path = argv[argi];
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--depart") == 0) {
            argi += 1;
            if (argi >= argc || parse_datetime_arg(argv[argi], &context.depart_date, &context.depart_seconds) != 0) { write_usage(program); return 1; }
            context.have_depart = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--arrive") == 0) {
            argi += 1;
            if (argi >= argc || parse_datetime_arg(argv[argi], &context.arrive_date, &context.arrive_seconds) != 0) { write_usage(program); return 1; }
            context.have_arrive = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--geometry") == 0) {
            context.show_geometry = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--color") == 0) {
            context.use_color = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-color") == 0) {
            context.use_color = 0;
            argi += 1;
        } else {
            write_usage(program);
            return 1;
        }
    }
    if (context.have_depart && context.have_arrive) {
        rt_write_cstr(2, "osmwalkroute: use only one of --depart or --arrive\n");
        return 1;
    }

    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.node = on_node;
    error[0] = '\0';
    if (pbf_stream_entities(context.pbf_path, &callbacks, &context, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmwalkroute: failed while reading nodes\n");
        return 1;
    }
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.way = on_way;
    error[0] = '\0';
    if (context.thread_count > 1U) {
        PbfStreamCallbacks way_callbacks;
        PbfStreamParallelOptions options;
        rt_memset(&way_callbacks, 0, sizeof(way_callbacks));
        way_callbacks.way = on_way_worker;
        rt_memset(&options, 0, sizeof(options));
        options.callbacks = &way_callbacks;
        options.worker_user_size = sizeof(WayWorkerContext);
        options.init_worker = way_worker_init;
        options.merge_worker = way_worker_merge;
        options.destroy_worker = way_worker_destroy;
        options.shared_user = &context;
        if (pbf_stream_entities_parallel(context.pbf_path, context.thread_count, &options, error, sizeof(error)) != 0) {
            rt_write_cstr(2, "osmwalkroute: failed while reading ways\n");
            return 1;
        }
        if (materialize_way_segments(&context) != 0) {
            rt_write_cstr(2, "osmwalkroute: out of memory while building graph\n");
            return 1;
        }
        rt_free(context.way_segments.items);
        context.way_segments.items = 0;
        context.way_segments.count = 0U;
        context.way_segments.capacity = 0U;
    } else {
        if (pbf_stream_entities(context.pbf_path, &callbacks, &context, error, sizeof(error)) != 0) {
            rt_write_cstr(2, "osmwalkroute: failed while reading ways\n");
            return 1;
        }
    }
    if (!context.from.found || !context.to.found) {
        rt_write_cstr(2, "osmwalkroute: address not found: ");
        if (!context.from.found) rt_write_cstr(2, context.from.input);
        if (!context.from.found && !context.to.found) rt_write_cstr(2, ", ");
        if (!context.to.found) rt_write_cstr(2, context.to.input);
        rt_write_char(2, '\n');
        return 2;
    }
    start_node = nearest_node(&context.graph, context.from.lat_nano, context.from.lon_nano, &start_snap_m);
    target_node = nearest_node(&context.graph, context.to.lat_nano, context.to.lon_nano, &target_snap_m);
    if (start_node < 0 || target_node < 0) { rt_write_cstr(2, "osmwalkroute: could not snap addresses to walk graph\n"); return 2; }
    route_result = dijkstra(&context.graph, start_node, target_node);
    if (route_result < 0) { rt_write_cstr(2, "osmwalkroute: out of memory while routing\n"); return 1; }
    if (route_result == 0) { rt_write_cstr(2, "osmwalkroute: no walking route found\n"); return 2; }
    total_m = context.graph.nodes[target_node].distance + (unsigned long long)start_snap_m + (unsigned long long)target_snap_m;
    seconds = (total_m * 3600ULL + context.speed_m_per_hour / 2ULL) / context.speed_m_per_hour;
    rt_write_cstr(1, "route_found: "); write_colored_cstr(&context, "\033[1;32m", "yes"); rt_write_char(1, '\n');
    write_address_colored(&context, "from: ", &context.from);
    write_address_colored(&context, "to: ", &context.to);
    rt_write_cstr(1, "walk_distance_m: "); rt_write_uint(1, total_m); rt_write_char(1, '\n');
    rt_write_cstr(1, "graph_distance_m: "); rt_write_uint(1, context.graph.nodes[target_node].distance); rt_write_char(1, '\n');
    rt_write_cstr(1, "connector_distance_m: "); rt_write_uint(1, (unsigned long long)start_snap_m + (unsigned long long)target_snap_m); rt_write_char(1, '\n');
    rt_write_cstr(1, "walk_time_min: "); rt_write_uint(1, (seconds + 30ULL) / 60ULL); rt_write_char(1, '\n');
    rt_write_cstr(1, "graph_nodes: "); rt_write_uint(1, context.graph.node_count); rt_write_char(1, '\n');
    rt_write_cstr(1, "graph_edges: "); rt_write_uint(1, context.graph.edge_count); rt_write_char(1, '\n');
    rt_write_cstr(1, "bbox_nodes: "); rt_write_uint(1, context.bbox_nodes); rt_write_char(1, '\n');
    rt_write_cstr(1, "walkable_ways: "); rt_write_uint(1, context.walkable_ways); rt_write_char(1, '\n');
    rt_write_cstr(1, "address_nodes: "); rt_write_uint(1, context.address_nodes); rt_write_char(1, '\n');
    rt_write_cstr(1, "address_ways: "); rt_write_uint(1, context.address_ways); rt_write_char(1, '\n');
    rt_memset(&transit_plan, 0, sizeof(transit_plan));
    if (context.gtfs_path == 0) {
        if (context.have_depart || context.have_arrive) {
            rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[33m", "unavailable"); rt_write_char(1, '\n');
            rt_write_cstr(1, "transit_note: --depart/--arrive was provided without --gtfs\n");
        }
    } else if (context.have_arrive) {
        rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[33m", "unsupported"); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_note: --arrive is parsed, but only --depart routing is implemented currently\n");
    } else if (!context.have_depart) {
        rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[33m", "unavailable"); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_note: provide --depart YYYY-MM-DDTHH:MM with --gtfs\n");
    } else if (evaluate_gtfs_depart(&context, &transit_plan) != 0) {
        rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[1;31m", "failed"); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_note: GTFS files could not be evaluated for this query\n");
    } else if (!transit_plan.found) {
        rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[33m", "no_option"); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_note: no matching single-leg transit option found near the addresses/time\n");
    } else {
        unsigned long long transit_minutes = ((unsigned long long)transit_plan.total_sec + 30ULL) / 60ULL;
        rt_write_cstr(1, "transit_evaluation: "); write_colored_cstr(&context, "\033[1;32m", "found"); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_mode: ");
        write_colored_cstr(&context, "\033[1;32m", gtfs_mode_name(transit_plan.mode));
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_line: ");
        write_colored_cstr(&context, "\033[1;32m", transit_plan.line_short);
        if (transit_plan.line_long != 0 && transit_plan.line_long[0] != '\0' && rt_strcmp(transit_plan.line_long, transit_plan.line_short) != 0) {
            rt_write_cstr(1, " (");
            rt_write_cstr(1, transit_plan.line_long);
            rt_write_cstr(1, ")");
        }
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_board_stop: ");
        rt_write_cstr(1, transit_plan.board_stop);
        rt_write_cstr(1, " at ");
        write_hhmm(transit_plan.board_departure_sec);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_alight_stop: ");
        rt_write_cstr(1, transit_plan.alight_stop);
        rt_write_cstr(1, " at ");
        write_hhmm(transit_plan.alight_arrival_sec);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_total_min: ");
        rt_write_uint(1, transit_minutes);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_walk_to_stop_m: ");
        rt_write_uint(1, transit_plan.walk_to_stop_m);
        rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_walk_from_stop_m: ");
        rt_write_uint(1, transit_plan.walk_from_stop_m);
        rt_write_char(1, '\n');
        if (transit_minutes < ((seconds + 30ULL) / 60ULL)) {
            rt_write_cstr(1, "transit_beats_walking: "); write_colored_cstr(&context, "\033[1;32m", "yes"); rt_write_char(1, '\n');
            rt_write_cstr(1, "transit_time_saved_min: ");
            write_colored_uint(&context, "\033[1;32m", ((seconds + 30ULL) / 60ULL) - transit_minutes);
            rt_write_char(1, '\n');
        } else {
            rt_write_cstr(1, "transit_beats_walking: "); write_colored_cstr(&context, "\033[33m", "no"); rt_write_char(1, '\n');
        }
        rt_write_cstr(1, "transit_debug_active_services: "); rt_write_uint(1, transit_plan.debug_active_services); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_origin_candidates: "); rt_write_uint(1, transit_plan.debug_candidate_origins); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_destination_candidates: "); rt_write_uint(1, transit_plan.debug_candidate_destinations); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_active_trips: "); rt_write_uint(1, transit_plan.debug_active_trips); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_stop_times_scanned: "); rt_write_uint(1, transit_plan.debug_stop_times_scanned); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_stop_times_trip_hits: "); rt_write_uint(1, transit_plan.debug_stop_times_trip_hits); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_board_candidates: "); rt_write_uint(1, transit_plan.debug_board_candidates); rt_write_char(1, '\n');
    }
    if (context.gtfs_path != 0 && context.have_depart && !transit_plan.found) {
        rt_write_cstr(1, "transit_debug_active_services: "); rt_write_uint(1, transit_plan.debug_active_services); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_origin_candidates: "); rt_write_uint(1, transit_plan.debug_candidate_origins); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_destination_candidates: "); rt_write_uint(1, transit_plan.debug_candidate_destinations); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_active_trips: "); rt_write_uint(1, transit_plan.debug_active_trips); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_stop_times_scanned: "); rt_write_uint(1, transit_plan.debug_stop_times_scanned); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_stop_times_trip_hits: "); rt_write_uint(1, transit_plan.debug_stop_times_trip_hits); rt_write_char(1, '\n');
        rt_write_cstr(1, "transit_debug_board_candidates: "); rt_write_uint(1, transit_plan.debug_board_candidates); rt_write_char(1, '\n');
    }
    if (transit_plan.found && (unsigned long long)transit_plan.total_sec < seconds) {
        if (write_transit_route(&context, &transit_plan) != 0) return 1;
    } else if (write_plain_route(&context, target_node, start_snap_m, target_snap_m) != 0) return 1;
    if (context.show_geometry && write_geometry(&context.graph, target_node) != 0) return 1;
    return 0;
}
