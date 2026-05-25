#include "pbf.h"
#include "platform.h"
#include "runtime.h"

typedef struct {
    PbfText street;
    PbfText housenumber;
    PbfText postcode;
    int has_street;
    int has_housenumber;
    int has_postcode;
} OsmAddressFields;

typedef struct {
    int out_fd;
    int include_incomplete;
    int write_header;
    unsigned long long limit;
    unsigned long long written;
    unsigned long long node_addresses;
    unsigned long long way_addresses;
    unsigned long long relation_addresses;
    int failed;
} OsmAddressesContext;

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf OUT.tsv [--include-incomplete] [--no-header] [--limit N]\n");
}

static int parse_uint_arg(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') return -1;
    while (text[index] != '\0') {
        unsigned long long previous;
        if (text[index] < '0' || text[index] > '9') return -1;
        previous = value;
        value = value * 10ULL + (unsigned long long)(text[index] - '0');
        if (value < previous) return -1;
        index += 1U;
    }
    *value_out = value;
    return 0;
}

static int text_equals_cstr(PbfText text, const char *value) {
    size_t value_size = rt_strlen(value);

    return text.size == value_size && memcmp(text.data, value, value_size) == 0;
}

static void find_address_fields(const PbfTag *tags, unsigned int tag_count, OsmAddressFields *fields) {
    unsigned int index;

    rt_memset(fields, 0, sizeof(*fields));
    for (index = 0U; index < tag_count; ++index) {
        if (!fields->has_street && text_equals_cstr(tags[index].key, "addr:street")) {
            fields->street = tags[index].value;
            fields->has_street = 1;
        } else if (!fields->has_housenumber && text_equals_cstr(tags[index].key, "addr:housenumber")) {
            fields->housenumber = tags[index].value;
            fields->has_housenumber = 1;
        } else if (!fields->has_postcode && text_equals_cstr(tags[index].key, "addr:postcode")) {
            fields->postcode = tags[index].value;
            fields->has_postcode = 1;
        }
    }
}

static int address_fields_match(const OsmAddressesContext *context, const OsmAddressFields *fields) {
    if (context->include_incomplete) {
        return fields->has_street || fields->has_housenumber || fields->has_postcode;
    }
    return fields->has_street && fields->has_housenumber && fields->has_postcode;
}

static int write_all_or_fail(int fd, const void *data, size_t size) {
    return size == 0U ? 0 : rt_write_all(fd, data, size);
}

static int write_tsv_text(int fd, PbfText text) {
    size_t start = 0U;
    size_t index;

    for (index = 0U; index < text.size; ++index) {
        char ch = text.data[index];
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            if (write_all_or_fail(fd, text.data + start, index - start) != 0 || rt_write_char(fd, ' ') != 0) return -1;
            start = index + 1U;
        }
    }
    return write_all_or_fail(fd, text.data + start, text.size - start);
}

static int write_coord(int fd, long long nano) {
    unsigned long long value;
    unsigned long long whole;
    unsigned long long fraction;
    unsigned long long divisor = 100000000ULL;

    if (nano < 0) {
        if (rt_write_char(fd, '-') != 0) return -1;
        value = (unsigned long long)(-nano);
    } else {
        value = (unsigned long long)nano;
    }
    whole = value / 1000000000ULL;
    fraction = value % 1000000000ULL;
    if (rt_write_uint(fd, whole) != 0 || rt_write_char(fd, '.') != 0) return -1;
    while (divisor != 0ULL) {
        if (rt_write_char(fd, (char)('0' + (fraction / divisor) % 10ULL)) != 0) return -1;
        divisor /= 10ULL;
    }
    return 0;
}

static int write_entity_id(int fd, long long id) {
    if (id < 0) {
        return rt_write_char(fd, '-') != 0 || rt_write_uint(fd, (unsigned long long)(-id)) != 0 ? -1 : 0;
    }
    return rt_write_uint(fd, (unsigned long long)id) != 0 ? -1 : 0;
}

static int write_header(int fd) {
    return rt_write_cstr(fd, "type\tid\tlat\tlon\tstreet\thousenumber\tpostcode\n");
}

static int emit_address(OsmAddressesContext *context, const char *type, long long id, int has_coord, long long lat_nano, long long lon_nano, const OsmAddressFields *fields) {
    int fd = context->out_fd;

    if (!address_fields_match(context, fields)) return 0;
    if (context->limit != 0ULL && context->written >= context->limit) return 1;
    if (rt_write_cstr(fd, type) != 0 || rt_write_char(fd, '\t') != 0 || write_entity_id(fd, id) != 0 || rt_write_char(fd, '\t') != 0) goto fail;
    if (has_coord && write_coord(fd, lat_nano) != 0) goto fail;
    if (rt_write_char(fd, '\t') != 0) goto fail;
    if (has_coord && write_coord(fd, lon_nano) != 0) goto fail;
    if (rt_write_char(fd, '\t') != 0 || write_tsv_text(fd, fields->street) != 0) goto fail;
    if (rt_write_char(fd, '\t') != 0 || write_tsv_text(fd, fields->housenumber) != 0) goto fail;
    if (rt_write_char(fd, '\t') != 0 || write_tsv_text(fd, fields->postcode) != 0 || rt_write_char(fd, '\n') != 0) goto fail;
    context->written += 1ULL;
    return context->limit != 0ULL && context->written >= context->limit;

fail:
    context->failed = 1;
    return 1;
}

static int on_node(void *user, const PbfNode *node) {
    OsmAddressesContext *context = (OsmAddressesContext *)user;
    OsmAddressFields fields;
    unsigned long long previous = context->written;
    int stop;

    find_address_fields(node->tags, node->tag_count, &fields);
    stop = emit_address(context, "node", node->id, 1, node->lat_nano, node->lon_nano, &fields);
    if (context->written != previous) context->node_addresses += 1ULL;
    return stop;
}

static int on_way_tags(void *user, long long id, const PbfTag *tags, unsigned int tag_count) {
    OsmAddressesContext *context = (OsmAddressesContext *)user;
    OsmAddressFields fields;
    unsigned long long previous = context->written;

    find_address_fields(tags, tag_count, &fields);
    (void)emit_address(context, "way", id, 0, 0, 0, &fields);
    if (context->written != previous) context->way_addresses += 1ULL;
    return 0;
}

static int on_relation(void *user, const PbfRelation *relation) {
    OsmAddressesContext *context = (OsmAddressesContext *)user;
    OsmAddressFields fields;
    unsigned long long previous = context->written;
    int stop;

    find_address_fields(relation->tags, relation->tag_count, &fields);
    stop = emit_address(context, "relation", relation->id, 0, 0, 0, &fields);
    if (context->written != previous) context->relation_addresses += 1ULL;
    return stop;
}

static int output_is_stdout(const char *path) {
    return path[0] == '-' && path[1] == '\0';
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmaddresses";
    const char *pbf_path;
    const char *out_path;
    OsmAddressesContext context;
    PbfStreamCallbacks callbacks;
    char error[PBF_ERROR_CAPACITY];
    int argi;
    int stats_fd;

    if (argc < 3 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) {
        write_usage(program);
        return argc == 2 ? 0 : 1;
    }
    pbf_path = argv[1];
    out_path = argv[2];
    rt_memset(&context, 0, sizeof(context));
    context.write_header = 1;
    argi = 3;
    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--include-incomplete") == 0) {
            context.include_incomplete = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--no-header") == 0) {
            context.write_header = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--limit") == 0) {
            argi += 1;
            if (argi >= argc || parse_uint_arg(argv[argi], &context.limit) != 0) {
                write_usage(program);
                return 1;
            }
            argi += 1;
        } else {
            write_usage(program);
            return 1;
        }
    }
    context.out_fd = output_is_stdout(out_path) ? 1 : platform_open_write(out_path, 0644U);
    if (context.out_fd < 0) {
        rt_write_cstr(2, "osmaddresses: could not open output file\n");
        return 1;
    }
    if (context.write_header && write_header(context.out_fd) != 0) {
        rt_write_cstr(2, "osmaddresses: could not write output header\n");
        if (context.out_fd != 1) (void)platform_close(context.out_fd);
        return 1;
    }
    rt_memset(&callbacks, 0, sizeof(callbacks));
    callbacks.node = on_node;
    callbacks.way_tags = on_way_tags;
    callbacks.relation = on_relation;
    error[0] = '\0';
    if (pbf_stream_entities(pbf_path, &callbacks, &context, error, sizeof(error)) != 0 || context.failed) {
        rt_write_cstr(2, "osmaddresses: ");
        rt_write_cstr(2, context.failed ? "could not write output" : (error[0] == '\0' ? "failed to parse PBF" : error));
        rt_write_char(2, '\n');
        if (context.out_fd != 1) (void)platform_close(context.out_fd);
        return 1;
    }
    if (context.out_fd != 1 && platform_close(context.out_fd) != 0) {
        rt_write_cstr(2, "osmaddresses: could not close output file\n");
        return 1;
    }
    stats_fd = context.out_fd == 1 ? 2 : 1;
    rt_write_cstr(stats_fd, "addresses_written: ");
    rt_write_uint(stats_fd, context.written);
    rt_write_char(stats_fd, '\n');
    rt_write_cstr(stats_fd, "node_addresses: ");
    rt_write_uint(stats_fd, context.node_addresses);
    rt_write_char(stats_fd, '\n');
    rt_write_cstr(stats_fd, "way_addresses: ");
    rt_write_uint(stats_fd, context.way_addresses);
    rt_write_char(stats_fd, '\n');
    rt_write_cstr(stats_fd, "relation_addresses: ");
    rt_write_uint(stats_fd, context.relation_addresses);
    rt_write_char(stats_fd, '\n');
    return 0;
}