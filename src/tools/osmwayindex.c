#include "osm_index.h"
#include "runtime.h"

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf INDEX.osmwidx\n");
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmwayindex";
    unsigned long long way_count = 0ULL;
    unsigned long long ref_count = 0ULL;
    char error[OSM_INDEX_ERROR_CAPACITY];

    if (argc != 3 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) {
        write_usage(program);
        return argc == 2 ? 0 : 1;
    }
    error[0] = '\0';
    if (osm_way_index_build(argv[1], argv[2], &way_count, &ref_count, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmwayindex: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to build way index" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    rt_write_cstr(1, "ways_indexed: ");
    rt_write_uint(1, way_count);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "refs_indexed: ");
    rt_write_uint(1, ref_count);
    rt_write_char(1, '\n');
    return 0;
}