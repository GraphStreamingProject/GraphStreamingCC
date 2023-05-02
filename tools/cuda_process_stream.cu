#include <vector>
#include <graph.h>
#include <map>
#include <binary_graph_stream.h>
#include "../src/cuda_kernel.cu"

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cout << "ERROR: Incorrect number of arguments!" << std::endl;
    std::cout << "Arguments: stream_file, graph_workers" << std::endl;
  }

  std::string stream_file = argv[1];
  int num_threads = std::atoi(argv[2]);
  if (num_threads < 1) {
    std::cout << "ERROR: Invalid number of graph workers! Must be > 0." << std::endl;
  }

  BinaryGraphStream_MT stream(stream_file, 1024*32);
  node_id_t num_nodes = stream.nodes();
  size_t num_updates  = stream.edges();
  std::cout << "Running process_stream with CUDA: " << std::endl;
  std::cout << "Processing stream: " << stream_file << std::endl;
  std::cout << "nodes       = " << num_nodes << std::endl;
  std::cout << "num_updates = " << num_updates << std::endl;
  std::cout << std::endl;

  auto config = GraphConfiguration().gutter_sys(STANDALONE).num_groups(num_threads);
  config.gutter_conf().gutter_factor(-4);
  Graph g{num_nodes, config, 1};

  Supernode** supernodes;
  supernodes = g.getSupernodes();

  // Get variable from sample supernode
  int num_sketches = supernodes[0]->get_num_sktch();
  
  // Get variables from sample sketch
  size_t num_elems = supernodes[0]->get_sketch(0)->get_num_elems();
  size_t num_columns = supernodes[0]->get_sketch(0)->get_columns();
  size_t num_guesses = supernodes[0]->get_sketch(0)->get_num_guesses();
  
  CudaUpdateParams* cudaUpdateParams;
  gpuErrchk(cudaMallocManaged(&cudaUpdateParams, sizeof(CudaUpdateParams)));
  cudaUpdateParams[0] = CudaUpdateParams(num_nodes, num_updates, num_sketches, num_elems, num_columns, num_guesses);

  std::cout << "num_sketches: " << num_sketches << "\n";
  std::cout << "num_elems: " << num_elems << "\n";
  std::cout << "num_columns: " << num_columns << "\n";
  std::cout << "num_guesses: " << num_guesses << "\n";

  CudaCCParams* cudaCCParams;
  gpuErrchk(cudaMallocManaged(&cudaCCParams, sizeof(CudaCCParams)));
  cudaCCParams[0] = CudaCCParams(num_nodes, num_sketches, num_elems, num_columns, num_guesses);

  std::pair<Edge, SampleSketchRet> *graph_query = new std::pair<Edge, SampleSketchRet>[num_nodes];

  // Hashmap that stores node ids and edge ids that need to be updated
  std::map<int, std::vector<vec_t>> graphUpdates;
  std::vector<node_id_t> graph_reps(num_nodes);

  for (int i = 0; i < num_nodes; i++) {
    // Initialize each key in graphUpdates with empty vector
    graphUpdates[i] = std::vector<vec_t>{};
    
    // Initialize cudaUpdateParams
    cudaUpdateParams[0].nodeNumUpdates[i] = 0;
    cudaUpdateParams[0].nodeStartIndex[i] = 0;

    // Initialize cudaCCParams
    cudaCCParams[0].reps[i] = i;
    cudaCCParams[0].query[i] = {1, ZERO};
    cudaCCParams[0].parent[i] = i;
    cudaCCParams[0].size[i] = 1;

    cudaCCParams[0].sample_idxs[i] = supernodes[i]->curr_idx();
    cudaCCParams[0].merged_sketches[i] = supernodes[i]->get_merged_sketches();

    graph_reps[i] = i;
  }
  
  MT_StreamReader reader(stream);
  GraphUpdate upd;

  // Collect all the edges that need to be updated
  for (size_t e = 0; e < num_updates; e++) {
    upd = reader.get_edge();
    Edge &edge = upd.edge;

    graphUpdates[edge.src].push_back(static_cast<vec_t>(concat_pairing_fn(edge.src, edge.dst)));
    graphUpdates[edge.dst].push_back(static_cast<vec_t>(concat_pairing_fn(edge.dst, edge.src)));   
  }

  std::cout << "Finished initializing graphUpdates\n";

  // Transfer graphUpdates to nodeUpdates and edgeUpdates
  int nodeIt = 0;
  int startIndex = 0;
  for (auto it = graphUpdates.begin(); it != graphUpdates.end(); it++) {
    cudaUpdateParams[0].nodeStartIndex[it->first] = startIndex;
    cudaUpdateParams[0].nodeNumUpdates[it->first] = it->second.size();
    for (int i = 0; i < it->second.size(); i++) {
      cudaUpdateParams[0].nodeUpdates[nodeIt] = it->first;
      cudaUpdateParams[0].edgeUpdates[nodeIt] = it->second.at(i);
      nodeIt++;
    }
    startIndex += it->second.size();
  }

  std::cout << "Finished initializing nodeUpdates and edgeUpdates\n";

  CudaSketch* cudaSketches;
  gpuErrchk(cudaMallocManaged(&cudaSketches, num_nodes * num_sketches * sizeof(CudaSketch)));

  long* sketchSeeds;
  gpuErrchk(cudaMallocManaged(&sketchSeeds, num_nodes * num_sketches * sizeof(long)));

  // Allocate space for all buckets
  vec_t* d_bucket_a;
  vec_hash_t* d_bucket_c;
  gpuErrchk(cudaMallocManaged(&d_bucket_a, (num_nodes * num_sketches * num_elems * sizeof(vec_t))));
  gpuErrchk(cudaMallocManaged(&d_bucket_c, (num_nodes * num_sketches * num_elems * sizeof(vec_hash_t))));

  for (int i = 0; i < (num_nodes * num_sketches * num_elems); i++) {
    d_bucket_a[i] = 0;
    d_bucket_c[i] = 0;
  }

  // Create a vector of cuda supernodes and sketches
  for (int i = 0; i < num_nodes; i++) {
    for (int j = 0; j < num_sketches; j++) {
      Sketch* sketch = supernodes[i]->get_sketch(j);

      int bucket_id = (i * num_sketches * num_elems) + (j * num_elems);
      vec_t* bucket_a = &d_bucket_a[bucket_id];
      vec_hash_t* bucket_c = &d_bucket_c[bucket_id];

      // Rewrite sketch's bucket_a and bucket_c memory location
      sketch->set_bucket_a(bucket_a);
      sketch->set_bucket_c(bucket_c);

      CudaSketch cudaSketch(bucket_a, bucket_c, sketch->get_seed());
      cudaSketches[(i * num_sketches) + j] = cudaSketch;
      sketchSeeds[(i * num_sketches) + j] = sketch->get_seed();
    }
  }

  // Number of threads
  int num_device_threads = 1024;
  
  // Number of blocks
  int num_device_blocks = num_nodes;

  int device_id = cudaGetDevice(&device_id);
  int device_count = 0;
  cudaGetDeviceCount(&device_count);
  std::cout << "CUDA Device Count: " << device_count << "\n";
  std::cout << "CUDA Device ID: " << device_id << "\n";

  std::cout << "Allocated Shared Memory of: " << (num_elems * num_sketches * sizeof(vec_t_cu)) + (num_elems * num_sketches * sizeof(vec_hash_t)) << "\n";

  // Prefetch memory to device 
  gpuErrchk(cudaMemPrefetchAsync(cudaUpdateParams[0].nodeUpdates, num_updates * sizeof(node_id_t) * 2, device_id));
  gpuErrchk(cudaMemPrefetchAsync(cudaUpdateParams[0].edgeUpdates, num_updates * sizeof(vec_t) * 2, device_id));
  gpuErrchk(cudaMemPrefetchAsync(cudaUpdateParams[0].nodeNumUpdates, num_nodes * sizeof(node_id_t), device_id));
  gpuErrchk(cudaMemPrefetchAsync(cudaUpdateParams[0].nodeStartIndex, num_nodes * sizeof(node_id_t), device_id));
  gpuErrchk(cudaMemPrefetchAsync(cudaSketches, num_nodes * num_sketches * sizeof(CudaSketch), device_id));
  gpuErrchk(cudaMemPrefetchAsync(sketchSeeds, num_nodes * num_sketches * sizeof(long), device_id));
  gpuErrchk(cudaMemPrefetchAsync(d_bucket_a, num_nodes * num_sketches * num_elems * sizeof(vec_t), device_id));
  gpuErrchk(cudaMemPrefetchAsync(d_bucket_c, num_nodes * num_sketches * num_elems * sizeof(vec_hash_t), device_id));

  // Start timer for kernel
  auto ins_start = std::chrono::steady_clock::now();

  // Call kernel code
  std::cout << "Update Kernel Starting...\n";
  streamUpdate(num_device_threads, num_device_blocks, cudaUpdateParams, cudaSketches, sketchSeeds);
  std::cout << "Update Kernel finished.\n";

  // End timer for kernel
  auto ins_end = std::chrono::steady_clock::now();
  
  // Update graph's num_updates value
  g.num_updates += num_updates * 2;

  // Start timer for cc
  auto cc_start = std::chrono::steady_clock::now();

  bool first_round = true;
  Supernode** copy_supernodes;
  std::vector<std::chrono::duration<double>> round_durations;
  std::vector<std::chrono::duration<double>> sample_durations;
  std::vector<std::chrono::duration<double>> to_merge_durations;
  std::vector<std::chrono::duration<double>> merge_durations;

  // Start sampling supernodes
  do {
    // Start timer for initial time for round
    auto round_start = std::chrono::steady_clock::now();

    cudaCCParams[0].modified[0] = false;

    // Number of blocks
    num_device_blocks = (cudaCCParams[0].num_nodes[0] + num_device_threads - 1) / num_device_threads;

    // Get and check sample_idx of each supernodes
    for (int i = 0; i < cudaCCParams[0].num_nodes[0]; i++) {
      int index = cudaCCParams[0].reps[i];

      if(cudaCCParams[0].sample_idxs[index] >= cudaCCParams[0].merged_sketches[index]) throw OutOfQueriesException();

      Sketch* sketch = supernodes[index]->get_sketch(cudaCCParams[0].sample_idxs[index]);

      // Check if this sketch has already been queried
      if(sketch->get_queried()) throw MultipleQueryException();
      
      sketch->set_queried(true);

      // Increment current supernode's sample idx
      cudaCCParams[0].sample_idxs[index]++;
    }

    // Start timer for sampling
    auto sample_start = std::chrono::steady_clock::now();

    // Sample each supernodes
    cuda_sample_supernodes(num_device_threads, num_device_blocks, cudaCCParams, cudaSketches);

    // End timer for sampling
    auto sample_end = std::chrono::steady_clock::now();
    sample_durations.push_back(sample_end - sample_start);

    // Start timer for to_merge
    auto to_merge_start = std::chrono::steady_clock::now();

    // Reset to_merge
    for(int i = 0; i < num_nodes; i++) {
      cudaCCParams[0].to_merge[i].size[0] = 0;
    }

    cuda_supernodes_to_merge(num_device_threads, num_device_blocks, cudaCCParams);

    // End timer for to_merge
    auto to_merge_end = std::chrono::steady_clock::now();
    to_merge_durations.push_back(to_merge_end - to_merge_start);

    // Start timer for merge
    auto merge_start = std::chrono::steady_clock::now();

    cuda_merge_supernodes(num_device_threads, num_device_blocks, cudaCCParams, cudaSketches);

    // End timer for merge
    auto merge_end = std::chrono::steady_clock::now();
    merge_durations.push_back(merge_end - merge_start);

    first_round = false;

    // End timer for round
    auto round_end = std::chrono::steady_clock::now();
    round_durations.push_back(round_end - round_start);

  } while (cudaCCParams[0].modified[0] == true);

  for (node_id_t i = 0; i < num_nodes; ++i) {
    g.setSize(i, cudaCCParams[0].size[i]);
    g.setParent(i, cudaCCParams[0].parent[i]);
  }

  // Find connected components
  auto CC_num = g.cc_from_dsu().size();

  // End timer for cc
  auto cc_end = std::chrono::steady_clock::now();

  std::chrono::duration<double> insert_time = ins_end - ins_start;
  std::chrono::duration<double> cc_time = cc_end - cc_start;

  double num_seconds = insert_time.count();
  std::cout << "Total insertion time(sec):    " << num_seconds << std::endl;
  std::cout << "Updates per second:           " << stream.edges() / num_seconds << std::endl;
  std::cout << "Total CC query latency:       " << cc_time.count() << std::endl;

  for (int i = 0; i < sample_durations.size(); i++) {
    std::cout << "    Round " << i << ":                  " << round_durations[i].count() << std::endl;
    std::cout << "        Sampling:               " << sample_durations[i].count() << std::endl;
    std::cout << "        To_Merge:               " << to_merge_durations[i].count() << std::endl;
    std::cout << "        Merge:                  " << merge_durations[i].count() << std::endl;
  }
  std::cout << "Connected Components:         " << CC_num << std::endl;
}
