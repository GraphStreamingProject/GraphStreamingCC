#include "cc_sketch_alg.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <omp.h>

CCSketchAlg::CCSketchAlg(node_id_t num_nodes, CCAlgConfiguration config)
    : num_nodes(num_nodes), dsu(num_nodes), config(config) {
  representatives = new std::set<node_id_t>();
  sketches = new Sketch *[num_nodes];
  seed = std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::high_resolution_clock::now().time_since_epoch())
             .count();
  std::mt19937_64 r(seed);
  seed = r();

  vec_t sketch_vec_len = Sketch::calc_vector_length(num_nodes);
  size_t sketch_num_samples = Sketch::calc_cc_samples(num_nodes);
  for (node_id_t i = 0; i < num_nodes; ++i) {
    representatives->insert(i);
    sketches[i] = new Sketch(sketch_vec_len, seed, sketch_num_samples);
  }

  spanning_forest = new std::unordered_set<node_id_t>[num_nodes];
  spanning_forest_mtx = new std::mutex[num_nodes];
  dsu_valid = true;
  shared_dsu_valid = true;
}

CCSketchAlg *CCSketchAlg::construct_from_serialized_data(const std::string &input_file,
                                                        CCAlgConfiguration config) {
  double sketches_factor;
  auto binary_in = std::ifstream(input_file, std::ios::binary);
  size_t seed;
  node_id_t num_nodes;
  binary_in.read((char *)&seed, sizeof(seed));
  binary_in.read((char *)&num_nodes, sizeof(num_nodes));
  binary_in.read((char *)&sketches_factor, sizeof(sketches_factor));

  config.sketches_factor(sketches_factor);

  return new CCSketchAlg(num_nodes, seed, binary_in, config);
}

CCSketchAlg::CCSketchAlg(node_id_t num_nodes, size_t seed, std::ifstream &binary_stream,
                         CCAlgConfiguration config)
    : num_nodes(num_nodes), seed(seed), dsu(num_nodes), config(config) {
  representatives = new std::set<node_id_t>();
  sketches = new Sketch *[num_nodes];

  vec_t sketch_vec_len = Sketch::calc_vector_length(num_nodes);
  size_t sketch_num_samples = Sketch::calc_cc_samples(num_nodes);
  for (node_id_t i = 0; i < num_nodes; ++i) {
    representatives->insert(i);
    sketches[i] = new Sketch(sketch_vec_len, seed, binary_stream, sketch_num_samples);
  }
  binary_stream.close();

  spanning_forest = new std::unordered_set<node_id_t>[num_nodes];
  spanning_forest_mtx = new std::mutex[num_nodes];
  dsu_valid = false;
  shared_dsu_valid = false;
}

CCSketchAlg::~CCSketchAlg() {
  for (size_t i = 0; i < num_nodes; ++i) delete sketches[i];
  delete[] sketches;
  if (delta_sketches != nullptr) {
    for (size_t i = 0; i < num_delta_sketches; i++) delete delta_sketches[i];
    delete[] delta_sketches;
  }

  delete representatives;
  delete[] spanning_forest;
  delete[] spanning_forest_mtx;
}

void CCSketchAlg::pre_insert(GraphUpdate upd, int /* thr_id */) {
#ifdef NO_EAGER_DSU
  (void)upd;
  // reason we have an if statement: avoiding cache coherency issues
  unlikely_if(dsu_valid) {
    dsu_valid = false;
    shared_dsu_valid = false;
  }
#else
  if (dsu_valid) {
    Edge edge = upd.edge;
    auto src = std::min(edge.src, edge.dst);
    auto dst = std::max(edge.src, edge.dst);
    std::lock_guard<std::mutex> sflock(spanning_forest_mtx[src]);
    if (spanning_forest[src].find(dst) != spanning_forest[src].end()) {
      dsu_valid = false;
      shared_dsu_valid = false;
    } else {
      spanning_forest[src].insert(dst);
      dsu.merge(src, dst);
    }
  }
#endif  // NO_EAGER_DSU
}

void CCSketchAlg::apply_update_batch(int thr_id, node_id_t src_vertex,
                                     const std::vector<node_id_t> &dst_vertices) {
  if (update_locked) throw UpdateLockedException();
  Sketch &delta_sketch = *delta_sketches[thr_id];
  delta_sketch.zero_contents();

  for (const auto &dst : dst_vertices) {
    delta_sketch.update(static_cast<vec_t>(concat_pairing_fn(src_vertex, dst)));
  }

  std::unique_lock<std::mutex>(sketches[src_vertex]->mutex);
  sketches[src_vertex]->merge(delta_sketch);
}

void CCSketchAlg::apply_raw_buckets_update(node_id_t src_vertex, Bucket *raw_buckets) {
  std::unique_lock<std::mutex>(sketches[src_vertex]->mutex);
  sketches[src_vertex]->merge_raw_bucket_buffer(raw_buckets);
}

// Note: for performance reasons route updates through the driver instead of calling this function
//       whenever possible.
void CCSketchAlg::update(GraphUpdate upd) {
  pre_insert(upd, 0);
  Edge edge = upd.edge;

  sketches[edge.src]->update(static_cast<vec_t>(concat_pairing_fn(edge.src, edge.dst)));
  sketches[edge.dst]->update(static_cast<vec_t>(concat_pairing_fn(edge.src, edge.dst)));
}

// sample from a sketch that represents a supernode of vertices
// that is, 1 or more vertices merged together during Boruvka
inline bool CCSketchAlg::sample_supernode(Sketch &skt) {
  bool modified = false;
  SketchSample sample = skt.sample();

  Edge e = inv_concat_pairing_fn(sample.idx);
  SampleResult result_type = sample.result;

  // std::cout << "Sample: " << result_type << " e:" << e.src << " " << e.dst << std::endl;

  if (result_type == FAIL) {
    modified = true;
  } else if (result_type == GOOD) {
    DSUMergeRet<node_id_t> m_ret = dsu.merge(e.src, e.dst);
    if (m_ret.merged) {
#ifdef VERIFY_SAMPLES_F
      verifier->verify_edge(e);
#endif
      modified = true;
      // Update spanning forest
      auto src = std::min(e.src, e.dst);
      auto dst = std::max(e.src, e.dst);
      {
        std::unique_lock<std::mutex> lk(spanning_forest_mtx[src]);
        spanning_forest[src].insert(dst);
      }
    }
  }

  return modified;
}

/*
 * Returns the ith half-open range in the division of [0, length] into divisions segments.
 */
inline std::pair<node_id_t, node_id_t> get_ith_partition(node_id_t length, size_t i,
                                                         size_t divisions) {
  double div_factor = (double)length / divisions;
  return {ceil(div_factor * i), ceil(div_factor * (i + 1))};
}

/*
 * Returns the half-open range idx that contains idx
 * Inverse of get_ith_partition
 */
inline size_t get_partition_idx(node_id_t length, node_id_t idx, size_t divisions) {
  double div_factor = (double)length / divisions;
  return idx / div_factor;
}

inline node_id_t find_last_partition_of_root(const std::vector<MergeInstr> &merge_instr,
                                             const node_id_t root, node_id_t min_hint,
                                             size_t num_threads) {
  node_id_t max = merge_instr.size() - 1;
  node_id_t min = min_hint;
  MergeInstr target = {root, (node_id_t) -1};

  while (min < max) {
    node_id_t mid = min + (max - min) / 2;

    if (merge_instr[mid] < target) {
      min = mid + 1;
    } else {
      max = mid;
    }
  }

  if (merge_instr[min].root != root)
    min = min - 1;

  assert(merge_instr[min].root == root);
  assert(min == merge_instr.size() - 1 || merge_instr[min + 1].root > root);
  return get_partition_idx(merge_instr.size(), min, num_threads);
}

// merge the global and return if it is safe to query now
inline bool merge_global(const size_t cur_round, const Sketch &local_sketch,
                         GlobalMergeData &global) {
  std::unique_lock<std::mutex> lk(global.mtx);
  global.sketch.range_merge(local_sketch, cur_round, 1);
  ++global.num_merge_done;
  assert(global.num_merge_done <= global.num_merge_needed);

  return global.num_merge_done >= global.num_merge_needed;
}

bool CCSketchAlg::perform_boruvka_round(const size_t cur_round,
                                        const std::vector<MergeInstr> &merge_instr,
                                        std::vector<GlobalMergeData> &global_merges) {
  bool modified = false;
  bool except = false;
  std::exception_ptr err;
  for (size_t i = 0; i < global_merges.size(); i++) {
    global_merges[i].sketch.zero_contents();
    global_merges[i].num_merge_needed = -1;
    global_merges[i].num_merge_done = 0;
  }

  std::atomic<size_t> num_query;
  num_query = 0;

#pragma omp parallel default(shared) num_threads(8)
  {
    // some thread local variables
    Sketch local_sketch(Sketch::calc_vector_length(num_nodes), seed,
                        Sketch::calc_cc_samples(num_nodes));

    size_t thr_id = omp_get_thread_num();
    size_t num_threads = omp_get_num_threads();
    std::pair<node_id_t, node_id_t> partition = get_ith_partition(num_nodes, thr_id, num_threads);
    node_id_t start = partition.first;
    node_id_t end = partition.second;
    assert(start < end);

#pragma omp critical
    std::cout << thr_id << ": " << start << " " << end << std::endl;

    // node_id_t left_root = merge_instr[start].root;
    // node_id_t right_root = merge_instr[end - 1].root;

    bool root_from_left = false;
    if (start > 0) {
      root_from_left = merge_instr[start - 1].root == merge_instr[start].root;
    }
    bool root_exits_right = false;
    if (end < num_nodes) {
      root_exits_right = merge_instr[end - 1].root == merge_instr[end].root;
    }

    node_id_t cur_root = merge_instr[start].root;
#pragma omp critical
  {
    for (node_id_t i = start; i < end; i++) {
      node_id_t root = merge_instr[i].root;
      node_id_t child = merge_instr[i].child;
      std::cout << thr_id << ": " << child << " into " << root << std::endl;
      std::cout << "root_from_left " << root_from_left << " root_exits_right " << root_exits_right << std::endl;

      if (root != cur_root) {
        if (root_from_left) {
          // we hold the global for this merge
          bool query_ready = merge_global(cur_round, local_sketch, global_merges[thr_id]);
          if (query_ready) {
            try {
              num_query += 1;
              if (sample_supernode(global_merges[thr_id].sketch) && !modified) modified = true;
            } catch (...) {
              except = true;
              err = std::current_exception();
            }
          }

          // set root_from_left to false
          root_from_left = false;
        } else {
          // This is an entirely local computation
          // std::cout << std::endl;
          try {
            num_query += 1;
            if (sample_supernode(local_sketch) && !modified) modified = true;
          } catch (...) {
            except = true;
            err = std::current_exception();
          }
        }

        cur_root = root;
        local_sketch.zero_contents();
      }

      // std::cout << " " << child;
      local_sketch.range_merge(*sketches[child], cur_round, 1);
    }

    if (root_exits_right || root_from_left) {
      // global merge where we may or may not own it
      size_t global_id = find_last_partition_of_root(merge_instr, cur_root, start, num_threads);
      if (!root_from_left) {
        // Resolved root_from_left, so we are the first thread to encounter this root
        // set the number of threads that will merge into this component
        std::unique_lock<std::mutex> lk(global_merges[global_id].mtx);
        global_merges[global_id].num_merge_needed = global_id - thr_id + 1;
      }
      bool query_ready = merge_global(cur_round, local_sketch, global_merges[global_id]);
      if (query_ready) {
        try {
          num_query += 1;
          if (sample_supernode(global_merges[thr_id].sketch) && !modified) modified = true;
        } catch (...) {
          except = true;
          err = std::current_exception();
        }
      }
    } else {
      // This is an entirely local computation
      // std::cout << std::endl;
      try {
        num_query += 1;
        if (sample_supernode(local_sketch) && !modified) modified = true;
      } catch (...) {
        except = true;
        err = std::current_exception();
      }
    }
  }
  }

  std::cout << "Number of roots queried = " << num_query << std::endl;

  if (except) {
    // if one of our threads produced an exception throw it here
    std::rethrow_exception(err);
  }

  return modified;
}

std::vector<std::set<node_id_t>> CCSketchAlg::boruvka_emulation() {
  update_locked = true;

  cc_alg_start = std::chrono::steady_clock::now();
  std::vector<MergeInstr> merge_instr(num_nodes);

  size_t num_threads = omp_get_max_threads();
  std::vector<GlobalMergeData> global_merges;
  global_merges.reserve(num_threads);
  for (size_t i = 0; i < num_threads; i++) {
    global_merges.emplace_back(num_nodes, seed);
  }

  dsu.reset();
  for (node_id_t i = 0; i < num_nodes; ++i) {
    merge_instr[i] = {i, i};
    spanning_forest[i].clear();
  }
  size_t round_num = 0;
  bool modified = true;
  while (true) {
    auto start = std::chrono::steady_clock::now();
    modified = perform_boruvka_round(round_num, merge_instr, global_merges);
    std::cout << "round: " << round_num << " = "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << std::endl;

    if (!modified) break;

    // calculate updated merge instructions for next round
    start = std::chrono::steady_clock::now();
#pragma omp parallel for
    for (node_id_t i = 0; i < num_nodes; i++)
      merge_instr[i] = {dsu.find_root(i), i};

    std::sort(merge_instr.begin(), merge_instr.end());

    size_t num_roots = 1;
    size_t cur_root = merge_instr[0].root;
    for (size_t i = 1; i < num_nodes; i++) {
      if (merge_instr[i].root != cur_root) {
        num_roots += 1;
        cur_root = merge_instr[i].root;
      }
    }
    std::cout << "Number of roots = " << num_roots << std::endl;


    std::cout << "post round processing = "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << std::endl;
    ++round_num;
  }
  last_query_rounds = round_num;

  dsu_valid = true;
  shared_dsu_valid = true;

  auto retval = cc_from_dsu();
  cc_alg_end = std::chrono::steady_clock::now();
  update_locked = false;
  return retval;
}

std::vector<std::set<node_id_t>> CCSketchAlg::connected_components() {
  // if the DSU holds the answer, use that
  if (shared_dsu_valid) {
    cc_alg_start = std::chrono::steady_clock::now();
#ifdef VERIFY_SAMPLES_F
    for (node_id_t src = 0; src < num_nodes; ++src) {
      for (const auto &dst : spanning_forest[src]) {
        verifier->verify_edge({src, dst});
      }
    }
#endif
    auto retval = cc_from_dsu();
#ifdef VERIFY_SAMPLES_F
    verifier->verify_soln(retval);
#endif
    cc_alg_end = std::chrono::steady_clock::now();
    return retval;
  }

  std::vector<std::set<node_id_t>> ret;

  bool except = false;
  std::exception_ptr err;
  try {
    ret = boruvka_emulation();
#ifdef VERIFY_SAMPLES_F
    verifier->verify_soln(ret);
#endif
  } catch (...) {
    except = true;
    err = std::current_exception();
  }

  // get ready for ingesting more from the stream
  // reset dsu and resume graph workers
  for (node_id_t i = 0; i < num_nodes; i++) {
    sketches[i]->reset_sample_state();
  }

  // check if boruvka error'd
  if (except) std::rethrow_exception(err);

  return ret;
}

std::vector<std::pair<node_id_t, std::vector<node_id_t>>> CCSketchAlg::calc_spanning_forest() {
  // TODO: Could probably optimize this a bit by writing new code
  connected_components();
  
  std::vector<std::pair<node_id_t, std::vector<node_id_t>>> forest;

  for (node_id_t src = 0; src < num_nodes; src++) {
    if (spanning_forest[src].size() > 0) {
      std::vector<node_id_t> edge_list;
      edge_list.reserve(spanning_forest[src].size());
      for (node_id_t dst : spanning_forest[src]) {
        edge_list.push_back(dst);
      }
      forest.push_back({src, edge_list});
    }
  }
  return forest;
}

bool CCSketchAlg::point_query(node_id_t a, node_id_t b) {
  // DSU check before calling force_flush()
  if (dsu_valid) {
    cc_alg_start = std::chrono::steady_clock::now();
#ifdef VERIFY_SAMPLES_F
    for (node_id_t src = 0; src < num_nodes; ++src) {
      for (const auto &dst : spanning_forest[src]) {
        verifier->verify_edge({src, dst});
      }
    }
#endif
    bool retval = (dsu.find_root(a) == dsu.find_root(b));
    cc_alg_end = std::chrono::steady_clock::now();
    return retval;
  }

  bool except = false;
  std::exception_ptr err;
  bool ret;
  try {
    std::vector<std::set<node_id_t>> ccs = boruvka_emulation();
#ifdef VERIFY_SAMPLES_F
    verifier->verify_soln(ccs);
#endif
    ret = (dsu.find_root(a) == dsu.find_root(b));
  } catch (...) {
    except = true;
    err = std::current_exception();
  }

  // get ready for ingesting more from the stream
  // reset dsu and resume graph workers
  for (node_id_t i = 0; i < num_nodes; i++) {
    sketches[i]->reset_sample_state();
  }

  // check if boruvka errored
  if (except) std::rethrow_exception(err);

  return ret;
}

std::vector<std::set<node_id_t>> CCSketchAlg::cc_from_dsu() {
  // calculate connected components using DSU structure
  std::map<node_id_t, std::set<node_id_t>> temp;
  for (node_id_t i = 0; i < num_nodes; ++i) temp[dsu.find_root(i)].insert(i);
  std::vector<std::set<node_id_t>> retval;
  retval.reserve(temp.size());
  for (const auto &it : temp) retval.push_back(it.second);
  return retval;
}

void CCSketchAlg::write_binary(const std::string &filename) {
  auto binary_out = std::fstream(filename, std::ios::out | std::ios::binary);
  binary_out.write((char *)&seed, sizeof(seed));
  binary_out.write((char *)&num_nodes, sizeof(num_nodes));
  binary_out.write((char *)&config._sketches_factor, sizeof(config._sketches_factor));
  for (node_id_t i = 0; i < num_nodes; ++i) {
    sketches[i]->serialize(binary_out);
  }
  binary_out.close();
}
