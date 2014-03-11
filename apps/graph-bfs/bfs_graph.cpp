/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <vector>

#include "thread.h"
#include "io_interface.h"
#include "container.h"
#include "concurrency.h"

#include "vertex_index.h"
#include "graph_engine.h"
#include "graph_config.h"

class bfs_vertex: public compute_vertex
{
	enum {
		VISITED,
	};

	atomic_flags<int> flags;
public:
	bfs_vertex() {
	}

	bfs_vertex(vertex_id_t id, const vertex_index *index): compute_vertex(
			id, index) {
	}

	bool has_visited() const {
		return flags.test_flag(VISITED);
	}

	bool set_visited(bool visited) {
		if (visited)
			return flags.set_flag(VISITED);
		else
			return flags.clear_flag(VISITED);
	}

	void run(graph_engine &graph) {
		if (!has_visited()) {
			vertex_id_t id = get_id();
			graph.request_vertices(*this, &id, 1);
		}
	}

	void run(graph_engine &graph, const page_vertex &vertex);

	virtual void run_on_messages(graph_engine &,
			const vertex_message *msgs[], int num) {
	}
};

void bfs_vertex::run(graph_engine &graph, const page_vertex &vertex)
{
	vertex_id_t max_id = graph.get_max_vertex_id();
	vertex_id_t min_id = graph.get_min_vertex_id();

	assert(!has_visited());
	set_visited(true);

	// We need to add the neighbors of the vertex to the queue of
	// the next level.
	page_byte_array::const_iterator<vertex_id_t> end_it
		= vertex.get_neigh_end(OUT_EDGE);
	stack_array<vertex_id_t, 1024> dest_buf(vertex.get_num_edges(OUT_EDGE));
	int num_dests = 0;
	for (page_byte_array::const_iterator<vertex_id_t> it
			= vertex.get_neigh_begin(OUT_EDGE); it != end_it; ++it) {
		vertex_id_t id = *it;
		assert(id >= min_id && id <= max_id);
		dest_buf[num_dests++] = id;
	}
	graph.activate_vertices(dest_buf.data(), num_dests);
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

void print_usage()
{
	fprintf(stderr,
			"bfs [options] conf_file graph_file index_file start_vertex\n");
	fprintf(stderr, "-c confs: add more configurations to the system\n");
	graph_conf.print_help();
	params.print_help();
}

int main(int argc, char *argv[])
{
	int opt;
	std::string confs;
	int num_opts = 0;
	while ((opt = getopt(argc, argv, "c:")) != -1) {
		num_opts++;
		switch (opt) {
			case 'c':
				confs = optarg;
				num_opts++;
				break;
			default:
				print_usage();
		}
	}
	argv += 1 + num_opts;
	argc -= 1 + num_opts;

	if (argc < 4) {
		print_usage();
		exit(-1);
	}

	std::string conf_file = argv[0];
	std::string graph_file = argv[1];
	std::string index_file = argv[2];
	vertex_id_t start_vertex = atoi(argv[3]);

	config_map configs(conf_file);
	configs.add_options(confs);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	graph_index *index = NUMA_graph_index<bfs_vertex>::create(index_file,
			graph_conf.get_num_threads(), params.get_num_nodes());

	graph_engine *graph = graph_engine::create(graph_conf.get_num_threads(),
			params.get_num_nodes(), graph_file, index);
	printf("BFS starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start(&start_vertex, 1);
	graph->wait4complete();
	gettimeofday(&end, NULL);

	NUMA_graph_index<bfs_vertex>::const_iterator it
		= ((NUMA_graph_index<bfs_vertex> *) index)->begin();
	NUMA_graph_index<bfs_vertex>::const_iterator end_it
		= ((NUMA_graph_index<bfs_vertex> *) index)->end();
	int num_visited = 0;
	for (; it != end_it; ++it) {
		const bfs_vertex &v = (const bfs_vertex &) *it;
		if (v.has_visited())
			num_visited++;
	}

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph_engine::destroy(graph);
	destroy_io_system();
	printf("BFS from vertex %ld visits %d vertices. It takes %f seconds\n",
			(unsigned long) start_vertex, num_visited, time_diff(start, end));
}
