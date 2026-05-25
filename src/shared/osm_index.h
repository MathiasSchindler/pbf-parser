#ifndef NEWOS_OSM_INDEX_H
#define NEWOS_OSM_INDEX_H

#include <stddef.h>

#define OSM_INDEX_ERROR_CAPACITY 160U

typedef struct {
    long long id;
    long long lat_nano;
    long long lon_nano;
} OsmNodeIndexRecord;

typedef struct {
    int fd;
    unsigned long long count;
} OsmNodeIndex;

int osm_node_index_build(const char *pbf_path, const char *index_path, unsigned long long *count_out, char *error, size_t error_capacity);
int osm_node_index_open(OsmNodeIndex *index, const char *path, char *error, size_t error_capacity);
void osm_node_index_close(OsmNodeIndex *index);
int osm_node_index_find(OsmNodeIndex *index, long long id, OsmNodeIndexRecord *record_out, char *error, size_t error_capacity);

#endif