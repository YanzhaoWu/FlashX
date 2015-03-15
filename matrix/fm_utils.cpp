/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fm_utils.h"
#include "factor.h"
#include "generic_type.h"

namespace fm
{

/*
 * This applies to a vector of values corresponding to the same key,
 * and generates an adjacency list.
 */
class adj_apply_operate: public gr_apply_operate<data_frame>
{
public:
	void run(const void *key, const data_frame &val, mem_vector &out) const;

	const scalar_type &get_key_type() const {
		return get_scalar_type<fg::vertex_id_t>();
	}

	const scalar_type &get_output_type() const {
		return get_scalar_type<char>();
	}

	size_t get_num_out_eles() const {
		return 1;
	}
};

void adj_apply_operate::run(const void *key, const data_frame &val,
		mem_vector &out) const
{
	fg::vertex_id_t vid = *(const fg::vertex_id_t *) key;
	if (vid == fg::INVALID_VERTEX_ID) {
		out.resize(0);
		return;
	}

	// Right now, we just assume there aren't attributes.
	size_t edge_data_size = 0;
	assert(val.get_num_vecs() == 2);

	assert(out.is_type<char>());
	// The data frame is sorted based on the first vector and now we need
	// to access the entries in the second vector.
	const type_mem_vector<fg::vertex_id_t> &vec
		= (const type_mem_vector<fg::vertex_id_t> &) val.get_vec_ref(1);

	// I added an invalid edge for each vertex.
	// The invalid edge is the maximal integer.
	fg::vsize_t num_edges = vec.get_length() - 1;
	// TODO we actually don't need to alloate memory multiple times.
	std::unique_ptr<fg::vertex_id_t[]> edge_buf
		= std::unique_ptr<fg::vertex_id_t[]>(new fg::vertex_id_t[num_edges]);
	size_t edge_idx = 0;
	for (size_t i = 0; i < vec.get_length(); i++) {
		if (vec.get(i) == fg::INVALID_VERTEX_ID)
			continue;
		edge_buf[edge_idx++] = vec.get(i);
	}
	assert(edge_idx == num_edges);
	std::sort(edge_buf.get(), edge_buf.get() + num_edges);
	size_t size = fg::ext_mem_undirected_vertex::num_edges2vsize(num_edges,
			edge_data_size);
	out.resize(size);

	// Even if we generate a directed, we still can use undirected vertex to
	// store one type of edges of a vertex.
	fg::in_mem_undirected_vertex<> v(vid, edge_data_size > 0);
	for (size_t i = 0; i < num_edges; i++)
		v.add_edge(fg::edge<>(vid, edge_buf[i]));
	fg::ext_mem_undirected_vertex::serialize(v, out.get_raw_arr(), size,
			// The edge type here actually doesn't matter since it's
			// an undirected vertex.
			fg::edge_type::OUT_EDGE);
}

class count_agg_operate: public agg_operate
{
public:
	virtual void run(size_t num_eles, const void *in, void *output) const {
		fg::vsize_t *out = (fg::vsize_t *) output;
		out[0] = num_eles - 1;
	}

	virtual const scalar_type &get_input_type() const {
		return get_scalar_type<fg::vertex_id_t>();
	}

	virtual const scalar_type &get_output_type() const {
		return get_scalar_type<fg::vsize_t>();
	}
};

vector_vector::ptr create_1d_matrix(data_frame::ptr df)
{
	if (df->get_num_vecs() < 2) {
		BOOST_LOG_TRIVIAL(error)
			<< "The data frame needs to contain at least 2 vectors";
		return vector_vector::ptr();
	}

	std::string sort_vec_name = df->get_vec_name(0);
	df->sort(sort_vec_name);
	assert(df->is_sorted(sort_vec_name));
	type_mem_vector<fg::vsize_t>::ptr num_non_zeros
		= type_mem_vector<fg::vsize_t>::cast(df->get_vec(sort_vec_name)->groupby(
			count_agg_operate(), false)->get_vec("agg"));
	adj_apply_operate adj_op;
	return df->groupby(sort_vec_name, adj_op);
}

class set_2d_label_operate: public type_set_operate<factor_value_t>
{
	block_2d_size block_size;
public:
	set_2d_label_operate(const block_2d_size &_size): block_size(_size) {
	}

	virtual void set(factor_value_t *arr, size_t num_eles, off_t row_idx,
			off_t col_idx) const {
		assert(col_idx == 0);
		for (size_t i = 0; i < num_eles; i++)
			arr[i] = (row_idx + i) / block_size.get_num_rows();
	}
};

class part_2d_apply_operate: public gr_apply_operate<sub_vector_vector>
{
	size_t row_len;
	block_2d_size block_size;
public:
	part_2d_apply_operate(const block_2d_size &_size,
			size_t row_len): block_size(_size) {
		this->row_len = row_len;
	}

	void run(const void *key, const sub_vector_vector &val,
			mem_vector &out) const;

	const scalar_type &get_key_type() const {
		return get_scalar_type<factor_value_t>();
	}

	const scalar_type &get_output_type() const {
		return get_scalar_type<char>();
	}

	size_t get_num_out_eles() const {
		return 1;
	}
};

void part_2d_apply_operate::run(const void *key, const sub_vector_vector &val,
		mem_vector &out) const
{
	size_t block_height = block_size.get_num_rows();
	size_t block_width = block_size.get_num_cols();
	size_t num_blocks = ceil(((double) row_len) / block_width);
	factor_value_t block_row_id = *(const factor_value_t *) key;
	size_t tot_num_non_zeros = 0;
	size_t max_row_parts = 0;
	for (size_t i = 0; i < val.get_num_vecs(); i++) {
		const fg::ext_mem_undirected_vertex *v
			= (const fg::ext_mem_undirected_vertex *) val.get_raw_arr(i);
		assert(v->get_id() / block_height == (size_t) block_row_id);
		tot_num_non_zeros += v->get_num_edges();
		// I definitely over estimate the number of row parts.
		// If a row doesn't have many non-zero entries, I assume that
		// the non-zero entries distribute evenly across all row parts.
		max_row_parts += std::min(num_blocks, v->get_num_edges());
	}

	std::vector<size_t> neigh_idxs(val.get_num_vecs());
	// The maximal size of a block.
	size_t max_block_size
		// Even if a block is empty, its header still exists. The size is
		// accurate.
		= sizeof(sparse_block_2d) * num_blocks
		// The size for row part headers is highly over estimated.
		+ sizeof(sparse_row_part) * max_row_parts
		// The size is accurate.
		+ sparse_row_part::get_col_entry_size() * tot_num_non_zeros;
	out.resize(max_block_size);
	size_t curr_size = 0;
	// The maximal size of a row part.
	size_t max_row_size = sparse_row_part::get_size(block_width);
	std::unique_ptr<char[]> buf = std::unique_ptr<char[]>(new char[max_row_size]);
	size_t num_non_zeros = 0;
	for (size_t col_idx = 0; col_idx < row_len; col_idx += block_width) {
		sparse_block_2d *block
			= new (out.get_raw_arr() + curr_size) sparse_block_2d(
					block_row_id, col_idx / block_width);
		for (size_t row_idx = 0; row_idx < val.get_num_vecs(); row_idx++) {
			const fg::ext_mem_undirected_vertex *v
				= (const fg::ext_mem_undirected_vertex *) val.get_raw_arr(row_idx);
			// If the vertex has no more edges left.
			if (neigh_idxs[row_idx] >= v->get_num_edges())
				continue;
			assert(v->get_neighbor(neigh_idxs[row_idx]) >= col_idx);
			// If the vertex has no edges that fall in the range.
			if (v->get_neighbor(neigh_idxs[row_idx]) >= col_idx + block_width)
				continue;

			sparse_row_part *part = new (buf.get()) sparse_row_part(row_idx);
			size_t idx = neigh_idxs[row_idx];
			for (; idx < v->get_num_edges()
					&& v->get_neighbor(idx) < col_idx + block_width; idx++)
				part->add(block_size, v->get_neighbor(idx));
			assert(part->get_size() <= max_row_size);
			neigh_idxs[row_idx] = idx;
			num_non_zeros += part->get_num_non_zeros();
			assert(block->get_size() + part->get_size()
					<= max_block_size - curr_size);
			block->append(*part);
		}
		// Only the non-empty blocks exist in a block row.
		if (!block->is_empty()) {
			curr_size += block->get_size();
			block->verify(block_size);
		}
	}
	out.resize(curr_size);
}

std::pair<SpM_2d_index::ptr, SpM_2d_storage::ptr> create_2d_matrix(
		vector_vector::ptr adjs, const block_2d_size &block_size)
{
	size_t num_rows = adjs->get_num_vecs();
	factor f(ceil(((double) num_rows) / block_size.get_num_rows()));
	factor_vector::ptr labels = factor_vector::create(f, num_rows);
	labels->set_data(set_2d_label_operate(block_size));
	vector_vector::ptr res = adjs->groupby(*labels,
			part_2d_apply_operate(block_size, num_rows));

	matrix_header mheader(matrix_type::SPARSE, 0, num_rows, num_rows,
			matrix_layout_t::L_ROW_2D, prim_type::P_BOOL, block_size);

	// Construct the index file of the adjacency matrix.
	std::vector<off_t> offsets(res->get_num_vecs() + 1);
	off_t off = sizeof(mheader);
	for (size_t i = 0; i < res->get_num_vecs(); i++) {
		offsets[i] = off;
		off += res->get_length(i);
	}
	offsets[res->get_num_vecs()] = off;
	SpM_2d_index::ptr idx = SpM_2d_index::create(mheader, offsets);

	return std::pair<SpM_2d_index::ptr, SpM_2d_storage::ptr>(
			idx, SpM_2d_storage::create(mheader, *res, idx));
}

std::pair<SpM_2d_index::ptr, SpM_2d_storage::ptr> create_2d_matrix(
		data_frame::ptr df, const block_2d_size &block_size)
{
	vector_vector::ptr vv = create_1d_matrix(df);
	if (vv == NULL)
		return std::pair<SpM_2d_index::ptr, SpM_2d_storage::ptr>();
	else
		return create_2d_matrix(vv, block_size);
}

void export_2d_matrix(vector_vector::ptr adjs, const block_2d_size &block_size,
		const std::string &mat_file, const std::string &mat_idx_file)
{
	size_t num_rows = adjs->get_num_vecs();
	factor f(ceil(((double) num_rows) / block_size.get_num_rows()));
	factor_vector::ptr labels = factor_vector::create(f, num_rows);
	labels->set_data(set_2d_label_operate(block_size));
	vector_vector::ptr res = adjs->groupby(*labels,
			part_2d_apply_operate(block_size, num_rows));

	matrix_header mheader(matrix_type::SPARSE, 0, num_rows, num_rows,
			matrix_layout_t::L_ROW_2D, prim_type::P_BOOL, block_size);
	FILE *f_2d = fopen(mat_file.c_str(), "w");
	if (f_2d == NULL) {
		BOOST_LOG_TRIVIAL(error) << boost::format("open %1%: %2%")
			% mat_file % strerror(errno);
		return;
	}
	fwrite(&mheader, sizeof(mheader), 1, f_2d);
	bool ret = mem_vector::cast(res->cat())->export2(f_2d);
	assert(ret);
	fclose(f_2d);

	// Construct the index file of the adjacency matrix.
	std::vector<off_t> offsets(res->get_num_vecs() + 1);
	off_t off = sizeof(mheader);
	for (size_t i = 0; i < res->get_num_vecs(); i++) {
		offsets[i] = off;
		off += res->get_length(i);
	}
	offsets[res->get_num_vecs()] = off;
	SpM_2d_index::ptr mindex = SpM_2d_index::create(mheader, offsets);
	mindex->dump(mat_idx_file);
}

}