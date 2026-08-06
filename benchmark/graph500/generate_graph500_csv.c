#include "graph_generator.h"
#include "utils.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message, const char *path) {
	if (path) {
		fprintf(stderr, "%s %s: %s\n", message, path, strerror(errno));
	} else {
		fprintf(stderr, "%s\n", message);
	}
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
	if (argc != 5) {
		fprintf(stderr, "usage: %s SCALE EDGE_FACTOR NODES_CSV EDGES_CSV\n", argv[0]);
		return EXIT_FAILURE;
	}

	const int scale = atoi(argv[1]);
	const int edge_factor = atoi(argv[2]);
	if (scale < 1 || scale > 40 || edge_factor < 1) {
		fail("SCALE must be in [1, 40] and EDGE_FACTOR must be positive", NULL);
	}

	const int64_t vertex_count = INT64_C(1) << scale;
	const int64_t requested_edge_count = vertex_count * edge_factor;
	uint_fast32_t seed[5];
	/* Seeds used by the Graph500 sample driver. */
	make_mrg_seed(2, 3, seed);

	FILE *nodes = fopen(argv[3], "w");
	if (!nodes) {
		fail("cannot open", argv[3]);
	}
	if (fprintf(nodes, ":ID(Graph500),:LABEL,external_id:long\n") < 0) {
		fail("cannot write", argv[3]);
	}
	for (int64_t vertex = 0; vertex < vertex_count; vertex++) {
		if (fprintf(nodes, "%" PRId64 ",node,%" PRId64 "\n", vertex, vertex) < 0) {
			fail("cannot write", argv[3]);
		}
	}
	if (fclose(nodes) != 0) {
		fail("cannot close", argv[3]);
	}

	FILE *edge_file = fopen(argv[4], "w");
	if (!edge_file) {
		fail("cannot open", argv[4]);
	}
	if (fprintf(edge_file, "edge_id:long,:START_ID(Graph500),:END_ID(Graph500),:TYPE\n") < 0) {
		fail("cannot write", argv[4]);
	}
	const int64_t chunk_capacity = INT64_C(1) << 20;
	packed_edge *edges = malloc((size_t)chunk_capacity * sizeof(packed_edge));
	if (!edges) {
		fail("cannot allocate the Graph500 generation buffer", NULL);
	}
	for (int64_t chunk_start = 0; chunk_start < requested_edge_count; chunk_start += chunk_capacity) {
		const int64_t chunk_end = chunk_start + chunk_capacity < requested_edge_count
		                              ? chunk_start + chunk_capacity
		                              : requested_edge_count;
		generate_kronecker_range(seed, scale, chunk_start, chunk_end, edges);
		for (int64_t edge = chunk_start; edge < chunk_end; edge++) {
			const int64_t local_edge = edge - chunk_start;
			const int64_t source = get_v0_from_edge(&edges[local_edge]);
			const int64_t target = get_v1_from_edge(&edges[local_edge]);
			if (fprintf(edge_file, "%" PRId64 ",%" PRId64 ",%" PRId64 ",link\n", edge, source, target) < 0) {
				fail("cannot write", argv[4]);
			}
		}
	}
	if (fclose(edge_file) != 0) {
		fail("cannot close", argv[4]);
	}

	free(edges);
	fprintf(stderr, "wrote scale=%d vertices=%" PRId64 " edges=%" PRId64 "\n", scale, vertex_count,
	        requested_edge_count);
	return EXIT_SUCCESS;
}
