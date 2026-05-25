#include "osm_index.h"

#include "pbf.h"
#include "platform.h"
#include "runtime.h"

#define OSM_NODE_INDEX_HEADER_SIZE 32U
#define OSM_NODE_INDEX_RECORD_SIZE 24U
#define OSM_WAY_INDEX_HEADER_SIZE 40U
#define OSM_WAY_INDEX_RECORD_SIZE 24U

static const unsigned char osm_node_index_magic[8] = { 'O', 'S', 'M', 'N', 'I', 'D', 'X', '1' };
static const unsigned char osm_way_index_magic[8] = { 'O', 'S', 'M', 'W', 'I', 'D', 'X', '1' };

typedef struct {
    int fd;
    unsigned long long count;
    long long last_id;
    int has_last_id;
    int failed;
    char *error;
    size_t error_capacity;
} OsmNodeIndexBuildContext;

typedef struct {
    int record_fd;
    int ref_fd;
    unsigned long long count;
    unsigned long long ref_count;
    long long last_id;
    int has_last_id;
    int failed;
    char *error;
    size_t error_capacity;
} OsmWayIndexBuildContext;

static void osm_index_set_error(char *error, size_t error_capacity, const char *message) {
    if (error != 0 && error_capacity != 0U) {
        rt_copy_string(error, error_capacity, message);
    }
}

static void osm_index_write_u32(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8U) & 0xffU);
    out[2] = (unsigned char)((value >> 16U) & 0xffU);
    out[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void osm_index_write_u64(unsigned char *out, unsigned long long value) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        out[index] = (unsigned char)((value >> (index * 8U)) & 0xffU);
    }
}

static unsigned int osm_index_read_u32(const unsigned char *in) {
    return (unsigned int)in[0] |
           ((unsigned int)in[1] << 8U) |
           ((unsigned int)in[2] << 16U) |
           ((unsigned int)in[3] << 24U);
}

static unsigned long long osm_index_read_u64(const unsigned char *in) {
    unsigned long long value = 0ULL;
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        value |= ((unsigned long long)in[index]) << (index * 8U);
    }
    return value;
}

static int osm_index_read_exact(int fd, void *buffer, size_t count) {
    unsigned char *cursor = (unsigned char *)buffer;
    size_t offset = 0U;

    while (offset < count) {
        long bytes = platform_read(fd, cursor + offset, count - offset);
        if (bytes <= 0) {
            return -1;
        }
        offset += (size_t)bytes;
    }
    return 0;
}

static void osm_node_index_make_header(unsigned char header[OSM_NODE_INDEX_HEADER_SIZE], unsigned long long count) {
    rt_memset(header, 0, OSM_NODE_INDEX_HEADER_SIZE);
    memcpy(header, osm_node_index_magic, sizeof(osm_node_index_magic));
    osm_index_write_u32(header + 8U, 1U);
    osm_index_write_u32(header + 12U, OSM_NODE_INDEX_RECORD_SIZE);
    osm_index_write_u64(header + 16U, count);
}

static void osm_way_index_make_header(unsigned char header[OSM_WAY_INDEX_HEADER_SIZE], unsigned long long count, unsigned long long ref_count) {
    rt_memset(header, 0, OSM_WAY_INDEX_HEADER_SIZE);
    memcpy(header, osm_way_index_magic, sizeof(osm_way_index_magic));
    osm_index_write_u32(header + 8U, 1U);
    osm_index_write_u32(header + 12U, OSM_WAY_INDEX_RECORD_SIZE);
    osm_index_write_u64(header + 16U, count);
    osm_index_write_u64(header + 24U, ref_count);
}

static int osm_way_index_write_header(int fd, unsigned long long count, unsigned long long ref_count) {
    unsigned char header[OSM_WAY_INDEX_HEADER_SIZE];

    osm_way_index_make_header(header, count, ref_count);
    return rt_write_all(fd, header, sizeof(header));
}

static int osm_way_index_write_record(int fd, const OsmWayIndexRecord *record) {
    unsigned char data[OSM_WAY_INDEX_RECORD_SIZE];

    rt_memset(data, 0, sizeof(data));
    osm_index_write_u64(data, (unsigned long long)record->id);
    osm_index_write_u64(data + 8U, record->ref_offset);
    osm_index_write_u32(data + 16U, record->ref_count);
    return rt_write_all(fd, data, sizeof(data));
}

static void osm_way_index_read_record_data(const unsigned char data[OSM_WAY_INDEX_RECORD_SIZE], OsmWayIndexRecord *record) {
    record->id = (long long)osm_index_read_u64(data);
    record->ref_offset = osm_index_read_u64(data + 8U);
    record->ref_count = osm_index_read_u32(data + 16U);
}

static int osm_way_index_write_ref(int fd, long long ref) {
    unsigned char data[8];

    osm_index_write_u64(data, (unsigned long long)ref);
    return rt_write_all(fd, data, sizeof(data));
}

static int osm_node_index_write_header(int fd, unsigned long long count) {
    unsigned char header[OSM_NODE_INDEX_HEADER_SIZE];

    osm_node_index_make_header(header, count);
    return rt_write_all(fd, header, sizeof(header));
}

static int osm_node_index_write_record(int fd, const OsmNodeIndexRecord *record) {
    unsigned char data[OSM_NODE_INDEX_RECORD_SIZE];

    osm_index_write_u64(data, (unsigned long long)record->id);
    osm_index_write_u64(data + 8U, (unsigned long long)record->lat_nano);
    osm_index_write_u64(data + 16U, (unsigned long long)record->lon_nano);
    return rt_write_all(fd, data, sizeof(data));
}

static void osm_node_index_read_record_data(const unsigned char data[OSM_NODE_INDEX_RECORD_SIZE], OsmNodeIndexRecord *record) {
    record->id = (long long)osm_index_read_u64(data);
    record->lat_nano = (long long)osm_index_read_u64(data + 8U);
    record->lon_nano = (long long)osm_index_read_u64(data + 16U);
}

static int osm_node_index_on_node(void *user, const PbfNode *node) {
    OsmNodeIndexBuildContext *context = (OsmNodeIndexBuildContext *)user;
    OsmNodeIndexRecord record;

    if (context->has_last_id && node->id < context->last_id) {
        context->failed = 1;
        osm_index_set_error(context->error, context->error_capacity, "nodes are not sorted by ID");
        return 1;
    }
    record.id = node->id;
    record.lat_nano = node->lat_nano;
    record.lon_nano = node->lon_nano;
    if (osm_node_index_write_record(context->fd, &record) != 0) {
        context->failed = 1;
        osm_index_set_error(context->error, context->error_capacity, "could not write node index record");
        return 1;
    }
    context->last_id = node->id;
    context->has_last_id = 1;
    context->count += 1ULL;
    return 0;
}

static int osm_way_index_on_way(void *user, const PbfWay *way) {
    OsmWayIndexBuildContext *context = (OsmWayIndexBuildContext *)user;
    OsmWayIndexRecord record;
    unsigned int index;

    if (context->has_last_id && way->id < context->last_id) {
        context->failed = 1;
        osm_index_set_error(context->error, context->error_capacity, "ways are not sorted by ID");
        return 1;
    }
    if (way->ref_count > 0xffffffffU || context->ref_count > 0xffffffffffffffffULL - (unsigned long long)way->ref_count) {
        context->failed = 1;
        osm_index_set_error(context->error, context->error_capacity, "way index is too large");
        return 1;
    }
    record.id = way->id;
    record.ref_offset = context->ref_count;
    record.ref_count = way->ref_count;
    if (osm_way_index_write_record(context->record_fd, &record) != 0) {
        context->failed = 1;
        osm_index_set_error(context->error, context->error_capacity, "could not write way index record");
        return 1;
    }
    for (index = 0U; index < way->ref_count; ++index) {
        if (osm_way_index_write_ref(context->ref_fd, way->refs[index]) != 0) {
            context->failed = 1;
            osm_index_set_error(context->error, context->error_capacity, "could not write way index refs");
            return 1;
        }
    }
    context->last_id = way->id;
    context->has_last_id = 1;
    context->count += 1ULL;
    context->ref_count += (unsigned long long)way->ref_count;
    return 0;
}

int osm_node_index_build(const char *pbf_path, const char *index_path, unsigned long long *count_out, char *error, size_t error_capacity) {
    OsmNodeIndexBuildContext context;
    PbfStreamCallbacks callbacks;
    int stream_result;

    rt_memset(&context, 0, sizeof(context));
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS;
    context.error = error;
    context.error_capacity = error_capacity;
    context.fd = platform_open_write(index_path, 0644U);
    if (context.fd < 0) {
        osm_index_set_error(error, error_capacity, "could not open index output file");
        return -1;
    }
    if (osm_node_index_write_header(context.fd, 0ULL) != 0) {
        (void)platform_close(context.fd);
        osm_index_set_error(error, error_capacity, "could not write node index header");
        return -1;
    }
    callbacks.node = osm_node_index_on_node;
    stream_result = pbf_stream_entities(pbf_path, &callbacks, &context, error, error_capacity);
    if (stream_result != 0 || context.failed) {
        (void)platform_close(context.fd);
        return -1;
    }
    if (platform_seek(context.fd, 0, PLATFORM_SEEK_SET) != 0 || osm_node_index_write_header(context.fd, context.count) != 0) {
        (void)platform_close(context.fd);
        osm_index_set_error(error, error_capacity, "could not finalize node index header");
        return -1;
    }
    if (platform_close(context.fd) != 0) {
        osm_index_set_error(error, error_capacity, "could not close node index");
        return -1;
    }
    if (count_out != 0) {
        *count_out = context.count;
    }
    return 0;
}

static int osm_way_index_append_refs(int index_fd, const char *ref_temp_path, char *error, size_t error_capacity) {
    int ref_read_fd;
    unsigned char buffer[65536U];

    ref_read_fd = platform_open_read(ref_temp_path);
    if (ref_read_fd < 0) {
        osm_index_set_error(error, error_capacity, "could not reopen way ref data");
        return -1;
    }
    for (;;) {
        long bytes = platform_read(ref_read_fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            (void)platform_close(ref_read_fd);
            osm_index_set_error(error, error_capacity, "could not read way ref data");
            return -1;
        }
        if (bytes == 0) break;
        if (rt_write_all(index_fd, buffer, (size_t)bytes) != 0) {
            (void)platform_close(ref_read_fd);
            osm_index_set_error(error, error_capacity, "could not append way ref data");
            return -1;
        }
    }
    if (platform_close(ref_read_fd) != 0) {
        osm_index_set_error(error, error_capacity, "could not close way ref data");
        return -1;
    }
    return 0;
}

int osm_way_index_build(const char *pbf_path, const char *index_path, unsigned long long *way_count_out, unsigned long long *ref_count_out, char *error, size_t error_capacity) {
    OsmWayIndexBuildContext context;
    PbfStreamCallbacks callbacks;
    static const char ref_temp_suffix[] = ".refs.tmp";
    char *ref_temp_path;
    size_t index_path_size;
    size_t ref_temp_suffix_size;
    int stream_result;

    rt_memset(&context, 0, sizeof(context));
    rt_memset(&callbacks, 0, sizeof(callbacks));
    index_path_size = rt_strlen(index_path);
    ref_temp_suffix_size = sizeof(ref_temp_suffix) - 1U;
    ref_temp_path = (char *)rt_malloc(index_path_size + ref_temp_suffix_size + 1U);
    if (ref_temp_path == 0) {
        osm_index_set_error(error, error_capacity, "out of memory while preparing way index");
        return -1;
    }
    memcpy(ref_temp_path, index_path, index_path_size);
    memcpy(ref_temp_path + index_path_size, ref_temp_suffix, ref_temp_suffix_size + 1U);
    context.record_fd = -1;
    context.ref_fd = -1;
    context.error = error;
    context.error_capacity = error_capacity;
    context.record_fd = platform_open_write(index_path, 0644U);
    if (context.record_fd < 0) {
        rt_free(ref_temp_path);
        osm_index_set_error(error, error_capacity, "could not open way index output file");
        return -1;
    }
    context.ref_fd = platform_open_write(ref_temp_path, 0600U);
    if (context.ref_fd < 0) {
        (void)platform_close(context.record_fd);
        rt_free(ref_temp_path);
        osm_index_set_error(error, error_capacity, "could not create temporary way ref file");
        return -1;
    }
    if (osm_way_index_write_header(context.record_fd, 0ULL, 0ULL) != 0) {
        (void)platform_close(context.record_fd);
        (void)platform_close(context.ref_fd);
        (void)platform_remove_file(ref_temp_path);
        rt_free(ref_temp_path);
        osm_index_set_error(error, error_capacity, "could not write way index header");
        return -1;
    }
    callbacks.flags = PBF_STREAM_SKIP_NODE_TAGS | PBF_STREAM_SKIP_WAY_TAGS;
    callbacks.way = osm_way_index_on_way;
    stream_result = pbf_stream_entities(pbf_path, &callbacks, &context, error, error_capacity);
    if (platform_close(context.ref_fd) != 0) {
        (void)platform_close(context.record_fd);
        (void)platform_remove_file(ref_temp_path);
        rt_free(ref_temp_path);
        osm_index_set_error(error, error_capacity, "could not close temporary way ref file");
        return -1;
    }
    context.ref_fd = -1;
    if (stream_result != 0 || context.failed) {
        (void)platform_close(context.record_fd);
        (void)platform_remove_file(ref_temp_path);
        rt_free(ref_temp_path);
        return -1;
    }
    if (osm_way_index_append_refs(context.record_fd, ref_temp_path, error, error_capacity) != 0) {
        (void)platform_close(context.record_fd);
        (void)platform_remove_file(ref_temp_path);
        rt_free(ref_temp_path);
        return -1;
    }
    (void)platform_remove_file(ref_temp_path);
    rt_free(ref_temp_path);
    if (platform_seek(context.record_fd, 0, PLATFORM_SEEK_SET) != 0 || osm_way_index_write_header(context.record_fd, context.count, context.ref_count) != 0) {
        (void)platform_close(context.record_fd);
        osm_index_set_error(error, error_capacity, "could not finalize way index header");
        return -1;
    }
    if (platform_close(context.record_fd) != 0) {
        osm_index_set_error(error, error_capacity, "could not close way index");
        return -1;
    }
    if (way_count_out != 0) *way_count_out = context.count;
    if (ref_count_out != 0) *ref_count_out = context.ref_count;
    return 0;
}

int osm_node_index_open(OsmNodeIndex *index, const char *path, char *error, size_t error_capacity) {
    unsigned char header[OSM_NODE_INDEX_HEADER_SIZE];
    unsigned int version;
    unsigned int record_size;

    rt_memset(index, 0, sizeof(*index));
    index->fd = -1;
    index->fd = platform_open_read(path);
    if (index->fd < 0) {
        osm_index_set_error(error, error_capacity, "could not open node index");
        return -1;
    }
    if (osm_index_read_exact(index->fd, header, sizeof(header)) != 0 || memcmp(header, osm_node_index_magic, sizeof(osm_node_index_magic)) != 0) {
        osm_node_index_close(index);
        osm_index_set_error(error, error_capacity, "invalid node index header");
        return -1;
    }
    version = osm_index_read_u32(header + 8U);
    record_size = osm_index_read_u32(header + 12U);
    if (version != 1U || record_size != OSM_NODE_INDEX_RECORD_SIZE) {
        osm_node_index_close(index);
        osm_index_set_error(error, error_capacity, "unsupported node index version");
        return -1;
    }
    index->count = osm_index_read_u64(header + 16U);
    return 0;
}

void osm_node_index_close(OsmNodeIndex *index) {
    if (index != 0 && index->fd >= 0) {
        (void)platform_close(index->fd);
        index->fd = -1;
        index->count = 0ULL;
    }
}

int osm_way_index_open(OsmWayIndex *index, const char *path, char *error, size_t error_capacity) {
    unsigned char header[OSM_WAY_INDEX_HEADER_SIZE];
    unsigned int version;
    unsigned int record_size;

    rt_memset(index, 0, sizeof(*index));
    index->fd = -1;
    index->fd = platform_open_read(path);
    if (index->fd < 0) {
        osm_index_set_error(error, error_capacity, "could not open way index");
        return -1;
    }
    if (osm_index_read_exact(index->fd, header, sizeof(header)) != 0 || memcmp(header, osm_way_index_magic, sizeof(osm_way_index_magic)) != 0) {
        osm_way_index_close(index);
        osm_index_set_error(error, error_capacity, "invalid way index header");
        return -1;
    }
    version = osm_index_read_u32(header + 8U);
    record_size = osm_index_read_u32(header + 12U);
    if (version != 1U || record_size != OSM_WAY_INDEX_RECORD_SIZE) {
        osm_way_index_close(index);
        osm_index_set_error(error, error_capacity, "unsupported way index version");
        return -1;
    }
    index->count = osm_index_read_u64(header + 16U);
    index->ref_count = osm_index_read_u64(header + 24U);
    return 0;
}

void osm_way_index_close(OsmWayIndex *index) {
    if (index != 0 && index->fd >= 0) {
        (void)platform_close(index->fd);
        index->fd = -1;
        index->count = 0ULL;
        index->ref_count = 0ULL;
    }
}

int osm_node_index_find(OsmNodeIndex *index, long long id, OsmNodeIndexRecord *record_out, char *error, size_t error_capacity) {
    unsigned long long left = 0ULL;
    unsigned long long right = index->count;
    unsigned char data[OSM_NODE_INDEX_RECORD_SIZE];

    while (left < right) {
        unsigned long long mid = left + (right - left) / 2ULL;
        long long offset = (long long)OSM_NODE_INDEX_HEADER_SIZE + (long long)(mid * OSM_NODE_INDEX_RECORD_SIZE);
        OsmNodeIndexRecord record;

        if (platform_seek(index->fd, offset, PLATFORM_SEEK_SET) != offset || osm_index_read_exact(index->fd, data, sizeof(data)) != 0) {
            osm_index_set_error(error, error_capacity, "could not read node index record");
            return -1;
        }
        osm_node_index_read_record_data(data, &record);
        if (record.id == id) {
            if (record_out != 0) {
                *record_out = record;
            }
            return 1;
        }
        if (record.id < id) {
            left = mid + 1ULL;
        } else {
            right = mid;
        }
    }
    return 0;
}

int osm_way_index_find(OsmWayIndex *index, long long id, OsmWayIndexRecord *record_out, char *error, size_t error_capacity) {
    unsigned long long left = 0ULL;
    unsigned long long right = index->count;
    unsigned char data[OSM_WAY_INDEX_RECORD_SIZE];

    while (left < right) {
        unsigned long long mid = left + (right - left) / 2ULL;
        long long offset = (long long)OSM_WAY_INDEX_HEADER_SIZE + (long long)(mid * OSM_WAY_INDEX_RECORD_SIZE);
        OsmWayIndexRecord record;

        if (platform_seek(index->fd, offset, PLATFORM_SEEK_SET) != offset || osm_index_read_exact(index->fd, data, sizeof(data)) != 0) {
            osm_index_set_error(error, error_capacity, "could not read way index record");
            return -1;
        }
        osm_way_index_read_record_data(data, &record);
        if (record.id == id) {
            if (record_out != 0) *record_out = record;
            return 1;
        }
        if (record.id < id) left = mid + 1ULL;
        else right = mid;
    }
    return 0;
}

int osm_way_index_read_refs(OsmWayIndex *index, const OsmWayIndexRecord *record, long long **refs_out, char *error, size_t error_capacity) {
    unsigned char *data;
    long long *refs;
    size_t data_size;
    unsigned int ref_index;
    long long offset;

    *refs_out = 0;
    if (record->ref_count == 0U) return 0;
    if (record->ref_offset > index->ref_count || (unsigned long long)record->ref_count > index->ref_count - record->ref_offset) {
        osm_index_set_error(error, error_capacity, "way ref range is outside index");
        return -1;
    }
    if (record->ref_count > (unsigned int)(((size_t)-1) / sizeof(long long)) || record->ref_count > (unsigned int)(((size_t)-1) / 8U)) {
        osm_index_set_error(error, error_capacity, "way has too many refs");
        return -1;
    }
    refs = (long long *)rt_malloc(sizeof(long long) * (size_t)record->ref_count);
    data_size = (size_t)record->ref_count * 8U;
    data = (unsigned char *)rt_malloc(data_size);
    if (refs == 0 || data == 0) {
        rt_free(refs);
        rt_free(data);
        osm_index_set_error(error, error_capacity, "out of memory while reading way refs");
        return -1;
    }
    offset = (long long)OSM_WAY_INDEX_HEADER_SIZE + (long long)(index->count * OSM_WAY_INDEX_RECORD_SIZE) + (long long)(record->ref_offset * 8ULL);
    if (platform_seek(index->fd, offset, PLATFORM_SEEK_SET) != offset || osm_index_read_exact(index->fd, data, data_size) != 0) {
        rt_free(refs);
        rt_free(data);
        osm_index_set_error(error, error_capacity, "could not read way refs");
        return -1;
    }
    for (ref_index = 0U; ref_index < record->ref_count; ++ref_index) {
        refs[ref_index] = (long long)osm_index_read_u64(data + (size_t)ref_index * 8U);
    }
    rt_free(data);
    *refs_out = refs;
    return 0;
}

int osm_node_index_load_records(OsmNodeIndex *index, OsmNodeIndexRecord **records_out, unsigned long long *count_out, char *error, size_t error_capacity) {
    OsmNodeIndexRecord *records;
    unsigned char data[OSM_NODE_INDEX_RECORD_SIZE * 4096U];
    unsigned long long record_index;

    *records_out = 0;
    if (count_out != 0) *count_out = 0ULL;
    if (index->count > (unsigned long long)(((size_t)-1) / sizeof(OsmNodeIndexRecord))) {
        osm_index_set_error(error, error_capacity, "node index is too large to load");
        return -1;
    }
    records = (OsmNodeIndexRecord *)rt_malloc(sizeof(OsmNodeIndexRecord) * (size_t)index->count);
    if (records == 0 && index->count != 0ULL) {
        osm_index_set_error(error, error_capacity, "out of memory while loading node index");
        return -1;
    }
    if (platform_seek(index->fd, OSM_NODE_INDEX_HEADER_SIZE, PLATFORM_SEEK_SET) != (long long)OSM_NODE_INDEX_HEADER_SIZE) {
        rt_free(records);
        osm_index_set_error(error, error_capacity, "could not seek node index records");
        return -1;
    }
    record_index = 0ULL;
    while (record_index < index->count) {
        unsigned int batch_count = 0U;
        unsigned int batch_index;
        unsigned long long remaining = index->count - record_index;
        size_t batch_bytes;

        batch_count = remaining > 4096ULL ? 4096U : (unsigned int)remaining;
        batch_bytes = (size_t)batch_count * OSM_NODE_INDEX_RECORD_SIZE;
        if (osm_index_read_exact(index->fd, data, batch_bytes) != 0) {
            rt_free(records);
            osm_index_set_error(error, error_capacity, "could not read node index records");
            return -1;
        }
        for (batch_index = 0U; batch_index < batch_count; ++batch_index) {
            osm_node_index_read_record_data(data + (size_t)batch_index * OSM_NODE_INDEX_RECORD_SIZE, &records[record_index + (unsigned long long)batch_index]);
        }
        record_index += (unsigned long long)batch_count;
    }
    *records_out = records;
    if (count_out != 0) *count_out = index->count;
    return 0;
}