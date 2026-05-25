#include "osm_index.h"
#include "runtime.h"

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osm.pbf INDEX.osmnidx\n");
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmnodeindex";
    unsigned long long count = 0ULL;
    char error[OSM_INDEX_ERROR_CAPACITY];

    if (argc != 3 || rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) {
        write_usage(program);
        return argc == 2 ? 0 : 1;
    }
    error[0] = '\0';
    if (osm_node_index_build(argv[1], argv[2], &count, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmnodeindex: ");
        rt_write_cstr(2, error[0] == '\0' ? "failed to build node index" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    rt_write_cstr(1, "nodes_indexed: ");
    rt_write_uint(1, count);
    rt_write_char(1, '\n');
    return 0;
}