#include <vector>
#include <graph.h>
#include <binary_graph_stream.h>
#include "../src/cuda_kernel.cu"

typedef std::pair<Edge, UpdateType> GraphUpdate;

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cout << "ERROR: Incorrect number of arguments!" << std::endl;
    std::cout << "Arguments: stream_file, graph_workers, number of threads per edge update" << std::endl;
  }

  std::string stream_file = argv[1];
  int num_threads = std::atoi(argv[2]);
  if (num_threads < 1) {
    std::cout << "ERROR: Invalid number of graph workers! Must be > 0." << std::endl;
  }

  int num_threads_per_update = std::atoi(argv[3]);
  if (num_threads_per_update == 1) {
    std::cout << "Running with one thread for each edge update" << std::endl;
  }
  else if (num_threads_per_update == 2) {
    std::cout << "Running with two threads for each edge update" << std::endl;
  }
  else {
    std::cout << "ERROR: Invalid number of threads per edge update. Must be 1 or 2." << std::endl;
  }

  BinaryGraphStream stream(stream_file, 1024*32);
  node_id_t num_nodes = stream.nodes();
  size_t num_updates  = stream.edges();
  std::cout << "Running process_stream with CUDA: " << std::endl;
  std::cout << "Processing stream: " << stream_file << std::endl;
  std::cout << "nodes       = " << num_nodes << std::endl;
  std::cout << "num_updates = " << num_updates << std::endl;
  std::cout << std::endl;

  auto config = GraphConfiguration().gutter_sys(STANDALONE).num_groups(num_threads);
  config.gutter_conf().gutter_factor(-4);
  Graph g{num_nodes, config};

  int *nodeUpdates;
  cudaMallocManaged(&nodeUpdates, num_updates * sizeof(int) * 2);

  vec_t *edgeUpdates;
  cudaMallocManaged(&edgeUpdates, num_updates * sizeof(vec_t) * 2);

  Supernode** supernodes;
  supernodes = g.getSupernodes();

  CudaSupernode* cudaSupernodes;
  cudaMallocManaged(&cudaSupernodes, num_nodes * sizeof(CudaSupernode));

  // Collect all the edges that need to be updated
  // 1 Thread will be assigned to update the endpoint nodes of each edge
  for (size_t e = 0; e < num_updates; e++) {
    GraphUpdate graphUpdate = stream.get_edge();
    Edge updatedEdge = graphUpdate.first;
    nodeUpdates[(e * 2)] = updatedEdge.first;
    nodeUpdates[(e * 2) + 1] = updatedEdge.second;
    edgeUpdates[(e * 2)] = static_cast<vec_t>(concat_pairing_fn(updatedEdge.first, updatedEdge.second));
    edgeUpdates[(e * 2) + 1] = static_cast<vec_t>(concat_pairing_fn(updatedEdge.second, updatedEdge.first));
  }

  // Create a vector of cuda supernodes and sketches
  for (int i = 0; i < num_nodes; i++) {
    int num_sketches = supernodes[i]->get_num_sktch();
    
    CudaSketch* cudaSketches;
    cudaMallocManaged(&cudaSketches, num_sketches * sizeof(CudaSketch));

    for (int j = 0; j < num_sketches; j++) {
      Sketch* sketch = supernodes[i]->get_sketch(j);
      CudaSketch cudaSketch(sketch->get_bucket_a(), sketch->get_bucket_c(), sketch->get_failure_factor(), sketch->get_num_elems(), sketch->get_num_buckets(), sketch->get_num_guesses(), sketch->get_seed());
      cudaSketches[j] = cudaSketch;
    }
    cudaSupernodes[i] = CudaSupernode(cudaSketches, supernodes[i]->seed, num_sketches);
  }

  // Number of threads
  int num_device_threads = 1 << 10;
  
  // Number of blocks
  int num_device_blocks = 1;

  if(num_threads_per_update == 1) {
    num_device_blocks = (num_updates + num_device_threads - 1) / num_device_threads;
  }
  else { // Need twice number of total threads in grid
    num_device_blocks = ((num_updates * 2) + num_device_threads - 1) / num_device_threads;
  }


  auto ins_start = std::chrono::steady_clock::now();

  // Call kernel code
  streamUpdate(num_device_threads, num_device_blocks, nodeUpdates, num_updates, num_nodes, edgeUpdates, cudaSupernodes, num_threads_per_update);

  // Update graph's num_updates value
  g.num_updates += num_updates * 2;

  auto cc_start = std::chrono::steady_clock::now();
  auto CC_num = g.connected_components().size();
  std::chrono::duration<double> insert_time = g.flush_end - ins_start;
  std::chrono::duration<double> cc_time = std::chrono::steady_clock::now() - cc_start;
  double num_seconds = insert_time.count();
  std::cout << "Total insertion time was: " << num_seconds << std::endl;
  std::cout << "Insertion rate was:       " << stream.edges() / num_seconds << std::endl;
  std::cout << "CC query latency:         " << cc_time.count() << std::endl;
  std::cout << "Connected Components:     " << CC_num << std::endl;
}
