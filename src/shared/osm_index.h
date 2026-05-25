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

typedef struct {
    long long id;
    unsigned long long ref_offset;
    unsigned int ref_count;
} OsmWayIndexRecord;

typedef struct {
    int fd;
    unsigned long long count;
    unsigned long long ref_count;
} OsmWayIndex;

int osm_node_index_build(const char *pbf_path, const char *index_path, unsigned long long *count_out, char *error, size_t error_capacity);
int osm_node_index_open(OsmNodeIndex *index, const char *path, char *error, size_t error_capacity);
void osm_node_index_close(OsmNodeIndex *index);
int osm_node_index_find(OsmNodeIndex *index, long long id, OsmNodeIndexRecord *record_out, char *error, size_t error_capacity);
int osm_node_index_load_records(OsmNodeIndex *index, OsmNodeIndexRecord **records_out, unsigned long long *count_out, char *error, size_t error_capacity);

int osm_way_index_build(const char *pbf_path, const char *index_path, unsigned long long *way_count_out, unsigned long long *ref_count_out, char *error, size_t error_capacity);
int osm_way_index_open(OsmWayIndex *index, const char *path, char *error, size_t error_capacity);
void osm_way_index_close(OsmWayIndex *index);
int osm_way_index_find(OsmWayIndex *index, long long id, OsmWayIndexRecord *record_out, char *error, size_t error_capacity);
int osm_way_index_read_refs(OsmWayIndex *index, const OsmWayIndexRecord *record, long long **refs_out, char *error, size_t error_capacity);

#endif