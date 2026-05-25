#include "osm_index.h"

#include "pbf.h"
#include "platform.h"
#include "runtime.h"

#define OSM_NODE_INDEX_HEADER_SIZE 32U
#define OSM_NODE_INDEX_RECORD_SIZE 24U

static const unsigned char osm_node_index_magic[8] = { 'O', 'S', 'M', 'N', 'I', 'D', 'X', '1' };

typedef struct {
    int fd;
    unsigned long long count;
    long long last_id;
    int has_last_id;
    int failed;
    char *error;
    size_t error_capacity;
} OsmNodeIndexBuildContext;

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