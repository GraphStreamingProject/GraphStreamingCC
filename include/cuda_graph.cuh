#pragma once
#include <vector>
#include <map>
#include <util.h>
#include <cuda_kernel.cuh>
#include <atomic>

class CudaGraph {
    public: 
        int k;

        CudaUpdateParams *cudaUpdateParams;
        Supernode** supernodes;
        long* sketchSeeds;

        std::vector<std::mutex> mutexes;
        std::atomic<vec_t> offset;
        std::vector<cudaStream_t> streams;
        std::vector<int> streams_deltaApplied;
        std::vector<int> streams_src;
        std::vector<std::chrono::duration<double>> transfer_times;
        std::vector<std::chrono::duration<double>> delta_apply_times;


        CudaKernel cudaKernel;

        // Number of threads
        int num_device_threads;
        
        // Number of blocks
        int num_device_blocks;

        int num_host_threads;
        int batch_size;
        int stream_multiplier;
        size_t sketch_size;

        bool isInit = false; 
        bool kInit = false;

        // Default constructor
        CudaGraph() {}

        void configure(CudaUpdateParams* _cudaUpdateParams, Supernode** _supernodes, long* _sketchSeeds, int _num_host_threads) {
            cudaUpdateParams = _cudaUpdateParams;
            supernodes = _supernodes;
            sketchSeeds = _sketchSeeds;
            offset = 0;

            mutexes = std::vector<std::mutex>(cudaUpdateParams[0].num_nodes);

            num_device_threads = 1024;
            num_device_blocks = 1;
            num_host_threads = _num_host_threads;
            batch_size = cudaUpdateParams[0].batch_size;
            stream_multiplier = cudaUpdateParams[0].stream_multiplier;
            sketch_size = cudaUpdateParams[0].num_sketches * cudaUpdateParams[0].num_elems;

            /*for (int i = 0; i < num_host_threads; i++) {
                loop_times.push_back(std::vector<double>{});
            }*/
            
            // Assuming num_host_threads is even number
            for (int i = 0; i < num_host_threads * stream_multiplier; i++) {
                cudaStream_t stream;

                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);

                streams.push_back(stream);
                streams_deltaApplied.push_back(1);
                streams_src.push_back(-1);
            }

            isInit = true;
            
        };

        void k_configure(CudaUpdateParams* _cudaUpdateParams, Supernode** _kSupernodes, long* _sketchSeeds, int _num_host_threads, int _k) {
            cudaUpdateParams = _cudaUpdateParams;
            supernodes = _kSupernodes;
            sketchSeeds = _sketchSeeds;
            k = _k;
            offset = 0;

            mutexes = std::vector<std::mutex>(cudaUpdateParams[0].num_nodes);

            num_device_threads = 1024;
            num_device_blocks = 1;
            num_host_threads = _num_host_threads;
            batch_size = cudaUpdateParams[0].batch_size;
            stream_multiplier = cudaUpdateParams[0].stream_multiplier;
            sketch_size = cudaUpdateParams[0].num_sketches * cudaUpdateParams[0].num_elems;

            transfer_times.reserve(num_host_threads);
            delta_apply_times.reserve(num_host_threads);
            
            // Assuming num_host_threads is even number
            for (int i = 0; i < num_host_threads * stream_multiplier; i++) {
                cudaStream_t stream;

                cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);

                streams.push_back(stream);
                streams_deltaApplied.push_back(1);
                streams_src.push_back(-1);
            }

            kInit = true;
            std::cout << "cuda_graph: kGraph has been initialized.\n";
            std::cout << "Num thread blocks: " << num_device_blocks << "\n";
            std::cout << "Num threads for each block: " << num_device_threads << "\n";
        }

        void k_batch_update(int id, node_id_t src, const std::vector<node_id_t> &edges) {
            int stream_id = id * stream_multiplier;
            int stream_offset = 0;
            while(true) {
                if (cudaStreamQuery(streams[stream_id + stream_offset]) == cudaSuccess) {
                    // Update stream_id
                    stream_id += stream_offset;

                    // CUDA Stream is available, but does not have any delta sketch
                    if(streams_deltaApplied[stream_id] == 0) {
                        streams_deltaApplied[stream_id] = 1;

                        // Bring back delta sketch
                        auto transfer_start = std::chrono::steady_clock::now();
                        cudaMemcpyAsync(&cudaUpdateParams[0].h_bucket_a[k * stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_a[k * stream_id * sketch_size], k * sketch_size * sizeof(vec_t), cudaMemcpyDeviceToHost, streams[stream_id]);
                        cudaMemcpyAsync(&cudaUpdateParams[0].h_bucket_c[k * stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_c[k * stream_id * sketch_size], k * sketch_size * sizeof(vec_hash_t), cudaMemcpyDeviceToHost, streams[stream_id]);

                        cudaStreamSynchronize(streams[stream_id]);
                        auto transfer_end = std::chrono::steady_clock::now();
                        transfer_times[id] += transfer_end - transfer_start;

                        if(streams_src[stream_id] == -1) {
                            std::cout << "Stream #" << stream_id << ": Shouldn't be here!\n";
                        }

                        // Apply the delta sketch
                        auto delta_apply_start = std::chrono::steady_clock::now();
                        std::unique_lock<std::mutex> lk(mutexes[streams_src[stream_id]]);
                        for (int i = 0; i < k; i++) {
                            for (int j = 0; j < cudaUpdateParams[0].num_sketches; j++) {
                                Sketch* sketch = supernodes[(streams_src[stream_id] * k) + i]->get_sketch(j);
                                vec_t* bucket_a = sketch->get_bucket_a();
                                vec_hash_t* bucket_c = sketch->get_bucket_c();

                                for (size_t m = 0; m < cudaUpdateParams[0].num_elems; m++) {
                                    bucket_a[m] ^= cudaUpdateParams[0].h_bucket_a[(stream_id * sketch_size * k) + (i * sketch_size) + (j * cudaUpdateParams[0].num_elems) + m];
                                    bucket_c[m] ^= cudaUpdateParams[0].h_bucket_c[(stream_id * sketch_size * k) + (i * sketch_size) + (j * cudaUpdateParams[0].num_elems) + m];
                                }
                            }
                        }
                        lk.unlock();
                        streams_src[stream_id] = -1;
                        auto delta_apply_end = std::chrono::steady_clock::now();
                        delta_apply_times[id] += delta_apply_end - delta_apply_start;
                    }
                    else {
                        if (streams_src[stream_id] != -1) {
                            std::cout << "Stream #" << stream_id << ": not applying but has delta sketch: " << streams_src[stream_id] << " deltaApplied: " << streams_deltaApplied[stream_id] << "\n";
                        }
                    }

                    break;
                }
                stream_offset++;
                if (stream_offset == stream_multiplier) {
                    stream_offset = 0;
                }
            }
            int start_index = stream_id * batch_size;
            int count = 0;
            for (vec_t i = start_index; i < start_index + edges.size(); i++) {
                cudaUpdateParams[0].h_edgeUpdates[i] = static_cast<vec_t>(concat_pairing_fn(src, edges[count]));
                count++;
            }            
            streams_src[stream_id] = src;
            streams_deltaApplied[stream_id] = 0;
            cudaMemcpyAsync(&cudaUpdateParams[0].d_edgeUpdates[start_index], &cudaUpdateParams[0].h_edgeUpdates[start_index], edges.size() * sizeof(vec_t), cudaMemcpyHostToDevice, streams[stream_id]);
            cudaKernel.k_gtsStreamUpdate(num_device_threads, num_device_blocks, k, (stream_id * sketch_size * k), src, streams[stream_id], start_index, edges.size(), cudaUpdateParams, sketchSeeds);
            
        }

        void batch_update(int id, node_id_t src, const std::vector<node_id_t> &edges) {
            if (kInit) {
                k_batch_update(id, src, edges);
                return;
            }
            if (!isInit) {
                std::cout << "CudaGraph has not been initialized!\n";
            }

            int stream_id = id * stream_multiplier;
            int stream_offset = 0;
            while(true) {
                if (cudaStreamQuery(streams[stream_id + stream_offset]) == cudaSuccess) {
                    // Update stream_id
                    stream_id += stream_offset;

                    // CUDA Stream is available, but does not have any delta sketch
                    if(streams_deltaApplied[stream_id] == 0) {
                        streams_deltaApplied[stream_id] = 1;

                        // Bring back delta sketch
                        cudaMemcpyAsync(&cudaUpdateParams[0].h_bucket_a[stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_a[stream_id * sketch_size], sketch_size * sizeof(vec_t), cudaMemcpyDeviceToHost, streams[stream_id]);
                        cudaMemcpyAsync(&cudaUpdateParams[0].h_bucket_c[stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_c[stream_id * sketch_size], sketch_size * sizeof(vec_hash_t), cudaMemcpyDeviceToHost, streams[stream_id]);

                        cudaStreamSynchronize(streams[stream_id]);

                        if(streams_src[stream_id] == -1) {
                            std::cout << "Stream #" << stream_id << ": Shouldn't be here!\n";
                        }

                        // Apply the delta sketch
                        std::unique_lock<std::mutex> lk(mutexes[streams_src[stream_id]]);
                        for (int i = 0; i < cudaUpdateParams[0].num_sketches; i++) {
                            Sketch* sketch = supernodes[streams_src[stream_id]]->get_sketch(i);
                            vec_t* bucket_a = sketch->get_bucket_a();
                            vec_hash_t* bucket_c = sketch->get_bucket_c();

                            for (size_t j = 0; j < cudaUpdateParams[0].num_elems; j++) {
                                bucket_a[j] ^= cudaUpdateParams[0].h_bucket_a[(stream_id * sketch_size) + (i * cudaUpdateParams[0].num_elems) + j];
                                bucket_c[j] ^= cudaUpdateParams[0].h_bucket_c[(stream_id * sketch_size) + (i * cudaUpdateParams[0].num_elems) + j];
                            }
                        }
                        lk.unlock();
                        streams_src[stream_id] = -1;
                    }
                    else {
                        if (streams_src[stream_id] != -1) {
                            std::cout << "Stream #" << stream_id << ": not applying but has delta sketch: " << streams_src[stream_id] << " deltaApplied: " << streams_deltaApplied[stream_id] << "\n";
                        }
                    }

                    break;
                }
                stream_offset++;
                if (stream_offset == stream_multiplier) {
                    stream_offset = 0;
                }
            }
            int start_index = stream_id * batch_size;
            int count = 0;
            for (vec_t i = start_index; i < start_index + edges.size(); i++) {
                cudaUpdateParams[0].h_edgeUpdates[i] = static_cast<vec_t>(concat_pairing_fn(src, edges[count]));
                count++;
            }            
            streams_src[stream_id] = src;
            streams_deltaApplied[stream_id] = 0;
            cudaMemcpyAsync(&cudaUpdateParams[0].d_edgeUpdates[start_index], &cudaUpdateParams[0].h_edgeUpdates[start_index], edges.size() * sizeof(vec_t), cudaMemcpyHostToDevice, streams[stream_id]);
            cudaKernel.gtsStreamUpdate(num_device_threads, num_device_blocks, stream_id * sketch_size, src, streams[stream_id], start_index, edges.size(), cudaUpdateParams, sketchSeeds);
        };

        void k_applyFlushUpdates() {
            for (int stream_id = 0; stream_id < num_host_threads * stream_multiplier; stream_id++) {
                if(streams_deltaApplied[stream_id] == 0) {
                    streams_deltaApplied[stream_id] = 1;
                    
                    cudaMemcpy(&cudaUpdateParams[0].h_bucket_a[k * stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_a[k * stream_id * sketch_size], k * sketch_size * sizeof(vec_t), cudaMemcpyDeviceToHost);
                    cudaMemcpy(&cudaUpdateParams[0].h_bucket_c[k * stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_c[k * stream_id * sketch_size], k * sketch_size * sizeof(vec_hash_t), cudaMemcpyDeviceToHost);

                    // Apply the delta sketch
                    for (int i = 0; i < k; i++) {
                        for (int j = 0; j < cudaUpdateParams[0].num_sketches; j++) {
                            Sketch* sketch = supernodes[(streams_src[stream_id] * k) + i]->get_sketch(j);
                            vec_t* bucket_a = sketch->get_bucket_a();
                            vec_hash_t* bucket_c = sketch->get_bucket_c();

                            for (size_t m = 0; m < cudaUpdateParams[0].num_elems; m++) {
                                bucket_a[m] ^= cudaUpdateParams[0].h_bucket_a[(stream_id * sketch_size * k) + (i * sketch_size) + (j * cudaUpdateParams[0].num_elems) + m];
                                bucket_c[m] ^= cudaUpdateParams[0].h_bucket_c[(stream_id * sketch_size * k) + (i * sketch_size) + (j * cudaUpdateParams[0].num_elems) + m];
                            }
                        }
                }
                    streams_src[stream_id] = -1;
                }
            }
        }

        void applyFlushUpdates() {
            for (int stream_id = 0; stream_id < num_host_threads * stream_multiplier; stream_id++) {
                if(streams_deltaApplied[stream_id] == 0) {
                    streams_deltaApplied[stream_id] = 1;
                    
                    cudaMemcpy(&cudaUpdateParams[0].h_bucket_a[stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_a[stream_id * sketch_size], sketch_size * sizeof(vec_t), cudaMemcpyDeviceToHost);
                    cudaMemcpy(&cudaUpdateParams[0].h_bucket_c[stream_id * sketch_size], &cudaUpdateParams[0].d_bucket_c[stream_id * sketch_size], sketch_size * sizeof(vec_hash_t), cudaMemcpyDeviceToHost);

                    // Apply the delta sketch
                    for (int i = 0; i < cudaUpdateParams[0].num_sketches; i++) {
                        Sketch* sketch = supernodes[streams_src[stream_id]]->get_sketch(i);
                        vec_t* bucket_a = sketch->get_bucket_a();
                        vec_hash_t* bucket_c = sketch->get_bucket_c();

                        for (size_t j = 0; j < cudaUpdateParams[0].num_elems; j++) {
                            bucket_a[j] ^= cudaUpdateParams[0].h_bucket_a[(stream_id * sketch_size) + (i * cudaUpdateParams[0].num_elems) + j];
                            bucket_c[j] ^= cudaUpdateParams[0].h_bucket_c[(stream_id * sketch_size) + (i * cudaUpdateParams[0].num_elems) + j];
                        }
                    }
                    streams_src[stream_id] = -1;
                }
            }
        }
};