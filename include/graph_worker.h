#ifndef WORKER_GUARD
#define WORKER_GUARD

#include <atomic>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#ifndef USE_FBT_T
#include "work_queue.h"
#endif

// forward declarations
#ifdef USE_FBT_T
class BufferTree;
#endif
class Graph;

class GraphWorker {
public:
#ifdef USE_FBT_F
	/*
	 * Create a GraphWorker object by setting metadata and
	 * spinning up a thread
	 * @param _id     the id of the new GraphWorker
	 * @param _graph  the graph which this GraphWorker will be updating
	 * @param _db     the database data will be extracted from
	 */
	GraphWorker(int _id, Graph *_graph, BufferTree *_db);
#else
  GraphWorker(int _id, Graph *_graph, WorkQueue *_wq);
#endif
	~GraphWorker();

	/*
	 * Returns if the current thread is paused
	 */
	bool get_thr_paused() {return thr_paused;}

	// manage threads
#ifdef USE_FBT_F
  static void start_workers(Graph *_graph, BufferTree *_db); // start the graph workers
#else
  static void start_workers(Graph *_graph, WorkQueue *_wq); // start the graph workers
#endif
	static void stop_workers();    // shutdown and delete GraphWorkers
	static void pause_workers();   // pause the GraphWorkers before CC
	static void unpause_workers(); // unpause the GraphWorkers to resume updates

	// manage configuration
	// configuration should be set before calling start_workers
	static const int factor = 2;
	static int get_num_groups() {return num_groups;} // return the number of GraphWorkers
	static int get_group_size() {return group_size;} // return the number of threads in each worker
	static void set_config(int g, int s) { num_groups = g; group_size = s; }
private:
	/*
	 * This function is used by a new thread to capture the
	 * GraphWorker object and begin running do_work
	 * @param obj is the memory where we will store the GraphWorker obj
	 */
	static void *start_worker(void *obj, int id) {
	  cpu_set_t cpuset;
	  CPU_ZERO(&cpuset);
	  CPU_SET(id % 3, &cpuset);	  
	  int rc = pthread_setaffinity_np(((GraphWorker *) obj)->thr.native_handle(),
					  sizeof(cpu_set_t), &cpuset);
	  if (rc != 0) {
	    std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
	  }
	  ((GraphWorker *)obj)->do_work();
	  return NULL;
	}

	void flush_data_buffer(const std::vector<data_ret_t>& data_buffer);

	void do_work();               // function which runs the GraphWorker process
	int id;
	Graph *graph;
#ifdef USE_FBT_F
	BufferTree *bf;
#else
	WorkQueue *wq;
#endif
	std::thread thr;
	std::atomic<bool> thr_paused; // indicates if this individual thread is paused
	// thread status and status management
	static bool shutdown;
	static bool paused;
	static bool all_workers_completed;
	static std::condition_variable pause_condition;
	static std::mutex pause_lock;
        std::chrono::milliseconds total_mpi_send_work{};
        std::chrono::milliseconds total_mpi_receive_results{};
        std::chrono::milliseconds total_mpi_send_terminate{};
        std::chrono::milliseconds total_applying_deltas{};
        std::chrono::milliseconds total_serialize_work{};
        std::chrono::milliseconds total_deserialize_results{};
        std::chrono::milliseconds total_work_queue{};	
	// configuration
	static int num_groups;
	static int group_size;
	static int next_worker;
	// list of all GraphWorkers
	static GraphWorker **workers;
};

#endif
