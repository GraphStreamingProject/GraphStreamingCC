#include <binary_file_stream.h>
#include <cc_sketch_alg.h>
#include <KEdgeConnect.h>
#include <graph_sketch_driver.h>
#include <sys/resource.h>  // for rusage
#include <test/mat_graph_verifier.h>
#include <map>
#include <xxhash.h>
#include <vector>
#include <algorithm>

#include <thread>

// TODO: make num_edge_connect an input argument;
// TODO: Daniel's concern: right now, the deletion of the edge to the stream makes the query only possible to be supported once -- fix this later


static bool shutdown = false;

class MinCutSimple {
public:
    const node_id_t num_nodes;
    unsigned int num_forest;  // this value is k in the k-edge connectivity
    unsigned int num_subgraphs;
    const int my_prime = 100003; // Prime number for polynomial hashing
    std::vector<std::vector<int>> hash_coefficients;
    std::vector<std::unique_ptr<KEdgeConnect>> k_edge_algs;

    explicit MinCutSimple(node_id_t num_nodes, const std::vector<std::vector<CCAlgConfiguration>> &config_vec):
            num_nodes(num_nodes) {
        num_subgraphs = (unsigned int)(2*std::ceil(std::log2(num_nodes)));
        // TODO: make the approximation factor tunable later
        num_forest = 10*num_subgraphs;
        for(unsigned int i=0;i<num_subgraphs;i++){
            k_edge_algs.push_back(std::make_unique<KEdgeConnect>(num_nodes, num_forest, config_vec[i]));
        }
        // Initialize coefficients randomly
        std::random_device rd_ind;
        std::mt19937 gen_ind(rd_ind());
        std::uniform_int_distribution<int> dist_coeff(1, my_prime - 1); // random numbers between 1 and p-1
        for (int i =0; i<num_subgraphs; i++) {
            std::vector<int> this_subgraph_coeff;
            for (int j = 0; j < num_subgraphs; j++) {
                this_subgraph_coeff.push_back(dist_coeff(gen_ind));
            }
            hash_coefficients.push_back(this_subgraph_coeff);
        }
    }

    ~MinCutSimple();

    void allocate_worker_memory(size_t num_workers){
        for(unsigned int i=0;i<num_subgraphs;i++){
            k_edge_algs[i]->allocate_worker_memory(num_workers);
        }
    }

    size_t get_desired_updates_per_batch(){
        return k_edge_algs[0]->get_desired_updates_per_batch();
    }

    node_id_t get_num_vertices() { return num_nodes; }

    // Function to calculate power modulo prime
    static int power(int x, int y, int p) {
        int res = 1; // Initialize result
        x = x % p; // Update x if it is more than or equal to p
        while (y > 0) {
            // If y is odd, multiply x with result
            if (y & 1)
                res = (res * x) % p;
            // y must be even now
            y = y >> 1; // y = y/2
            x = (x * x) % p;
        }
        return res;
    }

    // Function to generate k-wise independent hash
    int k_wise_hash(const std::vector<int>& coefficients, unsigned int src_vertex, unsigned int dst_vertex) {
        int hash_val = 0;
        if (src_vertex>dst_vertex){
            std::swap(src_vertex, dst_vertex);
        }
        unsigned int edge_id = src_vertex*num_nodes + dst_vertex;
        for (int i = 0; i < coefficients.size(); ++i) {
            hash_val = (hash_val + coefficients[i] * power(edge_id, i, my_prime)) % my_prime;
        }
        return (hash_val % 2);
    }

    
    void pre_insert(GraphUpdate upd, node_id_t thr_id) { }

    // Custom comparator function to sort dst_vertices based on end_index in descending order
    static bool compareEndIndexDescending(const unsigned int &a, const unsigned int &b, const std::vector<unsigned int> &end_index) {
        return end_index[a] > end_index[b];
    }

    // TODO: Change the apply update batch function as opposed to change pre_insert

    void apply_update_batch(size_t thr_id, node_id_t src_vertex, const std::vector<node_id_t> &dst_vertices) {
        std::vector<std::pair<unsigned int, unsigned int>> dst_end_index;
        // Collect the end-index on which an edge is deleted, then sort the indices to achieve O(N) total update time
        // for the vector.
        std::pair<node_id_t, unsigned int> temp_pair;
        for (auto dst_vertex: dst_vertices){
            for (unsigned int i=1;i<num_subgraphs;i++) {
                if(k_wise_hash(hash_coefficients[i], src_vertex, dst_vertex)==0) {
                    temp_pair.first = dst_vertex;
                    temp_pair.second = i;
                    dst_end_index.push_back(temp_pair);
                    break;
                }
            }
        }
        // Sort end_index vector by the end_index of dst_end_index
        std::sort(dst_end_index.begin(), dst_end_index.end(), [](auto &left, auto &right) {
            return left.second > right.second;
        });

        std::vector<node_id_t> input_dst_vertices;
        for (auto & pair : dst_end_index){
            input_dst_vertices.push_back(pair.first);
        }

        unsigned int position = input_dst_vertices.size()-1;
        for(unsigned int i=0;i<num_subgraphs;i++) {
            k_edge_algs[i]->apply_update_batch(thr_id, src_vertex, input_dst_vertices);
            // The following while loop: keep the position variable to be always aligned with the last vertex
            // that has not been deleted yet
            // If the vertex-corresponding end_index is at most the iteration number, it means it has already been
            // accounted for in this iteration, and should not be accounted of at the next iteration; so we should
            // remove
            while (dst_end_index[position].second<=i){
                input_dst_vertices.pop_back();
                position--;
            }
        }
    }

    bool has_cached_query(){
        bool cached_query_flag = true;
        for (unsigned int i=0;i<num_subgraphs;i++) {
            cached_query_flag = cached_query_flag && k_edge_algs[i]->has_cached_query();
        }
    }

    void print_configuration(){k_edge_algs[0]->print_configuration(); }

    void query(){

    }
};

class TwoEdgeConnect {
 public:
  const node_id_t num_nodes;
  CCSketchAlg cc_alg_1;
  CCSketchAlg cc_alg_2;

  explicit TwoEdgeConnect(node_id_t num_nodes, const CCAlgConfiguration &config_1,
                          const CCAlgConfiguration &config_2)
      : num_nodes(num_nodes), cc_alg_1(num_nodes, config_1), cc_alg_2(num_nodes, config_2) {}

  void allocate_worker_memory(size_t num_workers) {
    cc_alg_1.allocate_worker_memory(num_workers);
    cc_alg_2.allocate_worker_memory(num_workers);
  }

  size_t get_desired_updates_per_batch() {
    // I don't want to return double because the updates are sent to both
    return cc_alg_1.get_desired_updates_per_batch();
  }

  node_id_t get_num_vertices() { return num_nodes; }

  void pre_insert(GraphUpdate upd, node_id_t thr_id) {
    cc_alg_1.pre_insert(upd, thr_id);
    cc_alg_2.pre_insert(upd, thr_id);
  }

  void apply_update_batch(size_t thr_id, node_id_t src_vertex,
                          const std::vector<node_id_t> &dst_vertices) {
    cc_alg_1.apply_update_batch(thr_id, src_vertex, dst_vertices);
    cc_alg_2.apply_update_batch(thr_id, src_vertex, dst_vertices);
  }

  bool has_cached_query() { return cc_alg_1.has_cached_query() && cc_alg_2.has_cached_query(); }

  void print_configuration() { cc_alg_1.print_configuration(); }

  void query() {
    std::vector<std::pair<node_id_t, std::vector<node_id_t>>> forest =
        cc_alg_1.calc_spanning_forest();

    GraphUpdate temp_edge;

    temp_edge.type = DELETE;

    std::cout << "SPANNING FOREST 1" << std::endl;
    for (unsigned int j = 0; j < forest.size(); j++) {
      std::cout << forest[j].first << ":";
      for (auto dst : forest[j].second) {
        std::cout << " " << dst;
        temp_edge.edge.src = forest[j].first;
        temp_edge.edge.dst = dst;
        cc_alg_2.update(temp_edge);
      }
      std::cout << std::endl;
    }

    std::vector<std::pair<node_id_t, std::vector<node_id_t>>> forest2 =
        cc_alg_2.calc_spanning_forest();

    std::cout << "SPANNING FOREST 2" << std::endl;
    for (unsigned int j = 0; j < forest.size(); j++) {
        std::cout << forest[j].first << ":";
        for (auto dst: forest[j].second) {
            std::cout << " " << dst;
        }
        std::cout << std::endl;
    }

    // TODO: reinsert into alg 2?
  }
};

static double get_max_mem_used() {
  struct rusage data;
  getrusage(RUSAGE_SELF, &data);
  return (double)data.ru_maxrss / 1024.0;
}

/*
 * Function which is run in a seperate thread and will query
 * the graph for the number of updates it has processed
 * @param total       the total number of edge updates
 * @param g           the graph object to query
 * @param start_time  the time that we started stream ingestion
 */
template <typename DriverType>

void track_insertions(uint64_t total, DriverType *driver,
                      std::chrono::steady_clock::time_point start_time) {
  total = total * 2;  // we insert 2 edge updates per edge

  printf("Insertions\n");
  printf("Progress:                    | 0%%\r");
  fflush(stdout);
  std::chrono::steady_clock::time_point start = start_time;
  std::chrono::steady_clock::time_point prev = start_time;
  uint64_t prev_updates = 0;

  while (true) {
    sleep(1);
    uint64_t updates = driver->get_total_updates();
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::chrono::duration<double> total_diff = now - start;
    std::chrono::duration<double> cur_diff = now - prev;

    // calculate the insertion rate
    uint64_t upd_delta = updates - prev_updates;
    // divide insertions per second by 2 because each edge is split into two updates
    // we care about edges per second not about stream updates
    size_t ins_per_sec = (((double)(upd_delta)) / cur_diff.count()) / 2;

    if (updates >= total || shutdown) break;

    // display the progress
    int progress = updates / (total * .05);
    printf("Progress:%s%s", std::string(progress, '=').c_str(),
           std::string(20 - progress, ' ').c_str());
    printf("| %i%% -- %lu per second\r", progress * 5, ins_per_sec);
    fflush(stdout);
  }

  printf("Progress:====================| Done                             \n");
  return;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cout << "ERROR: Incorrect number of arguments!" << std::endl;
    std::cout << "Arguments: stream_file, graph_workers, reader_threads" << std::endl;
    exit(EXIT_FAILURE);
  }

  shutdown = false;
  std::string stream_file = argv[1];
  int num_threads = std::atoi(argv[2]);
  if (num_threads < 1) {
    std::cout << "ERROR: Invalid number of graph workers! Must be > 0." << std::endl;
    exit(EXIT_FAILURE);
  }
  size_t reader_threads = std::atol(argv[3]);
  unsigned int num_edge_connect = 5;

  BinaryFileStream stream(stream_file);
  BinaryFileStream stream_ref(stream_file);
  node_id_t num_nodes = stream.vertices();
  size_t num_updates = stream.edges();
  std::cout << "Processing stream: " << stream_file << std::endl;
  std::cout << "nodes       = " << num_nodes << std::endl;
  std::cout << "num_updates = " << num_updates << std::endl;
  std::cout << std::endl;

  auto driver_config = DriverConfiguration().gutter_sys(CACHETREE).worker_threads(num_threads);
  std::vector<CCAlgConfiguration> config_vec;

  for (unsigned int i=0;i<num_edge_connect;i++){
      config_vec.push_back(CCAlgConfiguration().batch_factor(1));
  }

  KEdgeConnect k_edge_alg{num_nodes, num_edge_connect, config_vec};

  GraphSketchDriver<KEdgeConnect> driver{&k_edge_alg, &stream, driver_config, reader_threads};

  auto ins_start = std::chrono::steady_clock::now();
  std::thread querier(track_insertions<GraphSketchDriver<KEdgeConnect>>, num_updates, &driver, ins_start);

  driver.process_stream_until(END_OF_STREAM);

  auto cc_start = std::chrono::steady_clock::now();
  driver.prep_query();
  k_edge_alg.query();


  size_t m = stream_ref.edges();
  // test the edges in the spanning forest are in the original graph
  MatGraphVerifier kEdgeVerifier(num_nodes);

  while (m--) {
     GraphStreamUpdate upd;
     stream_ref.get_update_buffer(&upd, 1);
     kEdgeVerifier.edge_update(upd.edge.src, upd.edge.dst);
   }

  std::vector<std::vector<bool>> test_adj_mat(num_nodes);
  test_adj_mat =  kEdgeVerifier.extract_adj_matrix();

  Edge temp_edge;
  std::vector<std::pair<node_id_t, std::vector<node_id_t>>> temp_forest;
  for(unsigned int i=0;i<num_edge_connect;i++) {
      temp_forest = k_edge_alg.forests_collection[i];
      // Test the maximality of the connected components
      DisjointSetUnion<node_id_t> kruskal_dsu(num_nodes);
      std::vector<std::set<node_id_t>> temp_retval;
      for (unsigned int l = 0; l < temp_forest.size(); l++) {
          for (unsigned int j = 0; j < temp_forest[l].second.size(); j++) {
              kruskal_dsu.merge(temp_forest[l].first, temp_forest[l].second[j]);
          }
      }
      std::map<node_id_t, std::set<node_id_t>> temp_map;
      for (unsigned l = 0; l < num_nodes; ++l) {
          temp_map[kruskal_dsu.find_root(l)].insert(l);
      }
      temp_retval.reserve(temp_map.size());
      for (const auto& entry : temp_map) {
          temp_retval.push_back(entry.second);
      }
      std::cout<< std::endl;
      kEdgeVerifier.reset_cc_state();
      kEdgeVerifier.verify_soln(temp_retval);
      // End of the test of CC maximality
      // start of the test of CC edge existence
      for (unsigned int j = 0; j < temp_forest.size(); j++) {
            for (auto dst: temp_forest[j].second) {
                temp_edge.src = temp_forest[j].first;
                temp_edge.dst = dst;
                kEdgeVerifier.verify_edge(temp_edge);
                kEdgeVerifier.edge_update(temp_edge.src, temp_edge.dst);
            }
      }
      test_adj_mat =  kEdgeVerifier.extract_adj_matrix();
  }

  unsigned long CC_nums[num_edge_connect];
  for(unsigned int i=0;i<num_edge_connect;i++){
      CC_nums[i]= k_edge_alg.cc_alg[i]->connected_components().size();
  }

  std::chrono::duration<double> insert_time = driver.flush_end - ins_start;
  std::chrono::duration<double> cc_time = std::chrono::steady_clock::now() - cc_start;
  std::chrono::duration<double> flush_time = driver.flush_end - driver.flush_start;
  std::chrono::duration<double> cc_alg_time =
          k_edge_alg.cc_alg[num_edge_connect-1]->cc_alg_end - k_edge_alg.cc_alg[0]->cc_alg_start;

  shutdown = true;
  querier.join();

  double num_seconds = insert_time.count();
  std::cout << "Total insertion time(sec):    " << num_seconds << std::endl;
  std::cout << "Updates per second:           " << stream.edges() / num_seconds << std::endl;
  std::cout << "Total CC query latency:       " << cc_time.count() << std::endl;
  std::cout << "  Flush Gutters(sec):           " << flush_time.count() << std::endl;
  std::cout << "  Boruvka's Algorithm(sec):     " << cc_alg_time.count() << std::endl;
  for(unsigned int i=0;i<num_edge_connect;i++){
      std::cout << "Number of connected Component in :         " << i+1 << " is " << CC_nums[i] << std::endl;
  }
  std::cout << "Maximum Memory Usage(MiB):    " << get_max_mem_used() << std::endl;
}
