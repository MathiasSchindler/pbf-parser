#include "osm_index.h"
#include "runtime.h"

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--progress] FILE.osm.pbf INDEX.osmnidx INDEX.osmwidx\n");
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmindex";
    unsigned long long node_count = 0ULL;
    unsigned long long way_count = 0ULL;
    unsigned long long ref_count = 0ULL;
    unsigned int flags = 0U;
    int argi = 1;
    char error[OSM_INDEX_ERROR_CAPACITY];

    if (argc >= 2 && (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0)) {
        write_usage(program);
        return 0;
    }
    if (argi < argc && rt_strcmp(argv[argi], "--progress") == 0) {
        flags |= OSM_INDEX_BUILD_PROGRESS;
        argi += 1;
    }
    if (argc - argi != 3) {
        write_usage(program);
        return 1;
    }
    error[0] = '\0';
    if (osm_index_build_ex(argv[argi], argv[argi + 1], argv[argi + 2], &node_count, &way_count, &ref_count, flags, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmindex: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to build indexes" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    rt_write_cstr(1, "nodes_indexed: ");
    rt_write_uint(1, node_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "ways_indexed: ");
    rt_write_uint(1, way_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "refs_indexed: ");
    rt_write_uint(1, ref_count);
    rt_write_char(1, '\n');
    return 0;
}