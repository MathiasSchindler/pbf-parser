#include "osmrpack.h"

#include "runtime.h"

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " FILE.osmrpack\n");
}

int main(int argc, char **argv) {
    const char *program = argc > 0 ? argv[0] : "osmrpackinfo";
    OsmrPackHeader header;
    char error[OSMRPACK_ERROR_CAPACITY];

    if (argc == 2 && (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0)) {
        write_usage(program);
        return 0;
    }
    if (argc != 2) {
        write_usage(program);
        return 1;
    }
    error[0] = '\0';
    if (osmrpack_read_header(argv[1], &header, error, sizeof(error)) != 0) {
        rt_write_cstr(2, "osmrpackinfo: ");
        rt_write_cstr(2, error[0] == '\0' ? "could not read pack" : error);
        rt_write_char(2, '\n');
        return 1;
    }
    osmrpack_write_header_text(1, &header);
    return 0;
}
