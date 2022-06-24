#include <gtest/gtest.h>
#include <fstream>
#include <algorithm>
#include "../include/graph.h"
#include "../graph_worker.h"
#include "../include/test/file_graph_verifier.h"
#include "../include/test/mat_graph_verifier.h"
#include "../include/test/graph_gen.h"

/**
 * For many of these tests (especially for those upon very sparse and small graphs)
 * we allow for a certain number of failures per test.
 * This is because the responsibility of these tests is to quickly alert us 
 * to “this code is very wrong” whereas the statistical testing is responsible 
 * for a more fine grained analysis.
 * In this context a false positive is much worse than a false negative.
 * With 2 failures allowed per test our entire testing suite should fail 1/5000 runs.
 */

// We create this class and instantiate a paramaterized test suite so that we
// can run these tests both with the GutterTree and with StandAloneGutters
class GraphTest : public testing::TestWithParam<GutterSystem> {

};
INSTANTIATE_TEST_SUITE_P(GraphTestSuite, GraphTest, testing::Values(GUTTERTREE, STANDALONE, CACHETREE));

TEST_P(GraphTest, SmallGraphConnectivity) {
  GraphConfiguration config;
  config.gutter_sys = GetParam();
  const std::string fname = __FILE__;
  size_t pos = fname.find_last_of("\\/");
  const std::string curr_dir = (std::string::npos == pos) ? "" : fname.substr(0, pos);
  std::ifstream in{curr_dir + "/res/multiples_graph_1024.txt"};
  node_id_t num_nodes;
  in >> num_nodes;
  edge_id_t m;
  in >> m;
  node_id_t a, b;
  Graph g{num_nodes, config};
  while (m--) {
    in >> a >> b;
    g.update({{a, b}, INSERT});
  }
  g.set_verifier(std::make_unique<FileGraphVerifier>(curr_dir + "/res/multiples_graph_1024.txt"));
  ASSERT_EQ(78, g.connected_components().size());
}

TEST(GraphTest, IFconnectedComponentsAlgRunTHENupdateLocked) {
  GraphConfiguration config;
  config.gutter_sys = STANDALONE;
  const std::string fname = __FILE__;
  size_t pos = fname.find_last_of("\\/");
  const std::string curr_dir = (std::string::npos == pos) ? "" : fname.substr(0, pos);
  std::ifstream in{curr_dir + "/res/multiples_graph_1024.txt"};
  node_id_t num_nodes;
  in >> num_nodes;
  edge_id_t m;
  in >> m;
  node_id_t a, b;
  Graph g{num_nodes, config};
  while (m--) {
    in >> a >> b;
    g.update({{a, b}, INSERT});
  }
  g.set_verifier(std::make_unique<FileGraphVerifier>(curr_dir + "/res/multiples_graph_1024.txt"));
  g.connected_components();
  ASSERT_THROW(g.update({{1,2}, INSERT}), UpdateLockedException);
  ASSERT_THROW(g.update({{1,2}, DELETE}), UpdateLockedException);
}

TEST(GraphTest, TestSupernodeRestoreAfterCCFailure) {
  for (int s = 0; s < 2; s++) {
    GraphConfiguration config;
    config.backup_in_mem = s == 0;
    const std::string fname = __FILE__;
    size_t pos = fname.find_last_of("\\/");
    const std::string curr_dir = (std::string::npos == pos) ? "" : fname.substr(0, pos);
    std::ifstream in{curr_dir + "/res/multiples_graph_1024.txt"};
    node_id_t num_nodes;
    in >> num_nodes;
    edge_id_t m;
    in >> m;
    node_id_t a, b;
    Graph g{num_nodes, config};
    while (m--) {
      in >> a >> b;
      g.update({{a, b}, INSERT});
    }
    g.set_verifier(std::make_unique<FileGraphVerifier>(curr_dir + "/res/multiples_graph_1024.txt"));
    g.should_fail_CC();

    // flush to make sure copy supernodes is consistent with graph supernodes
    g.gts->force_flush();
    GraphWorker::pause_workers();
    Supernode* copy_supernodes[num_nodes];
    for (node_id_t i = 0; i < num_nodes; ++i) {
      copy_supernodes[i] = Supernode::makeSupernode(*g.supernodes[i]);
    }

    ASSERT_THROW(g.connected_components(true), OutOfQueriesException);
    for (node_id_t i = 0; i < num_nodes; ++i) {
      for (int j = 0; j < copy_supernodes[i]->get_num_sktch(); ++j) {
        ASSERT_TRUE(*copy_supernodes[i]->get_sketch(j) ==
                  *g.supernodes[i]->get_sketch(j));
      }
    }
  }
}

TEST_P(GraphTest, TestCorrectnessOnSmallRandomGraphs) {
  GraphConfiguration config;
  config.gutter_sys = GetParam();
  int num_trials = 5;
  while (num_trials--) {
    generate_stream();
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g{n, config};
    int type, a, b;
    while (m--) {
      in >> type >> a >> b;
      if (type == INSERT) {
        g.update({{a, b}, INSERT});
      } else g.update({{a, b}, DELETE});
    }

    g.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    g.connected_components();
  }
}

TEST_P(GraphTest, TestCorrectnessOnSmallSparseGraphs) {
  GraphConfiguration config;
  config.gutter_sys = GetParam();
  int num_trials = 5;
  while(num_trials--) {
    generate_stream({1024,0.002,0.5,0,"./sample.txt","./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g{n, config};
    int type, a, b;
    while (m--) {
      in >> type >> a >> b;
      if (type == INSERT) {
        g.update({{a, b}, INSERT});
      } else g.update({{a, b}, DELETE});
    }

    g.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    g.connected_components();
  } 
}

TEST_P(GraphTest, TestCorrectnessOfReheating) {
  GraphConfiguration config;
  config.gutter_sys = GetParam();
  int num_trials = 5;
  while (num_trials--) {
    generate_stream({1024,0.002,0.5,0,"./sample.txt","./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph *g = new Graph (n, config);
    int type, a, b;
    printf("number of updates = %lu\n", m);
    while (m--) {
      in >> type >> a >> b;
      if (type == INSERT) g->update({{a, b}, INSERT});
      else g->update({{a, b}, DELETE});
    }
    g->write_binary("./out_temp.txt");
    g->set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    std::vector<std::set<node_id_t>> g_res;
    g_res = g->connected_components();
    printf("number of CC = %lu\n", g_res.size());
    delete g; // delete g to avoid having multiple graphs open at once. Which is illegal.

    Graph reheated {"./out_temp.txt"};
    reheated.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    auto reheated_res = reheated.connected_components();
    printf("number of reheated CC = %lu\n", reheated_res.size());
    ASSERT_EQ(g_res.size(), reheated_res.size());
    for (unsigned i = 0; i < g_res.size(); ++i) {
      std::vector<node_id_t> symdif;
      std::set_symmetric_difference(g_res[i].begin(), g_res[i].end(),
          reheated_res[i].begin(), reheated_res[i].end(),
          std::back_inserter(symdif));
      ASSERT_EQ(0, symdif.size());
    }
  }
}

// Test the multithreaded system by specifiying multiple
// Graph Workers of size 2. Ingest a stream and run CC algorithm.
TEST_P(GraphTest, MultipleWorkers) {
  GraphConfiguration config;
  config.gutter_sys = GetParam();
  config.num_groups = 4;
  config.group_size = 2;
  int num_trials = 5;
  while(num_trials--) {
    generate_stream({1024,0.002,0.5,0,"./sample.txt","./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g{n, config};
    int type, a, b;
    while (m--) {
      in >> type >> a >> b;
      if (type == INSERT) {
        g.update({{a, b}, INSERT});
      } else g.update({{a, b}, DELETE});
    }

    g.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    g.connected_components();
  } 
}

TEST(GraphTest, TestQueryDuringStream) {
  GraphConfiguration config;
  config.gutter_sys = STANDALONE;
  config.backup_in_mem = false;
  { // test copying to disk
    generate_stream({1024, 0.002, 0.5, 0, "./sample.txt", "./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g(n, config);
    MatGraphVerifier verify(n);

    int type;
    node_id_t a, b;
    edge_id_t tenth = m / 10;
    for(int j = 0; j < 9; j++) {
      for (edge_id_t i = 0; i < tenth; i++) {
        in >> type >> a >> b;
        g.update({{a,b}, (UpdateType)type});
        verify.edge_update(a, b);
      }
      verify.reset_cc_state();
      g.set_verifier(std::make_unique<MatGraphVerifier>(verify));
      g.connected_components(true);
    }
    m -= 9 * tenth;
    while(m--) {
      in >> type >> a >> b;
      g.update({{a,b}, (UpdateType)type});
      verify.edge_update(a, b);
    }
    verify.reset_cc_state();
    g.set_verifier(std::make_unique<MatGraphVerifier>(verify));
    g.connected_components();
  }

  config.backup_in_mem = true;
  { // test copying in memory
    generate_stream({1024, 0.002, 0.5, 0, "./sample.txt", "./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g(n, config);
    MatGraphVerifier verify(n);

    int type;
    node_id_t a, b;
    edge_id_t tenth = m / 10;
    for(int j = 0; j < 9; j++) {
      for (edge_id_t i = 0; i < tenth; i++) {
        in >> type >> a >> b;
        g.update({{a,b}, (UpdateType)type});
        verify.edge_update(a, b);
      }
      verify.reset_cc_state();
      g.set_verifier(std::make_unique<MatGraphVerifier>(verify));
      g.connected_components(true);
    }
    m -= 9 * tenth;
    while(m--) {
      in >> type >> a >> b;
      g.update({{a,b}, (UpdateType)type});
      verify.edge_update(a, b);
    }
    verify.reset_cc_state();
    g.set_verifier(std::make_unique<MatGraphVerifier>(verify));
    g.connected_components();
  }
}

TEST(GraphTest, MultipleInsertThreads) {
  GraphConfiguration config;
  config.gutter_sys = STANDALONE;
  int num_threads = 4;

  generate_stream({1024, 0.2, 0.5, 0, "./sample.txt", "./cumul_sample.txt"});
  std::ifstream in{"./sample.txt"};
  node_id_t n;
  edge_id_t m;
  in >> n >> m;
  int per_thread = m / num_threads;
  Graph g(n, config, num_threads);
  std::vector<std::vector<GraphUpdate>> updates(num_threads,
          std::vector<GraphUpdate>(per_thread));

  int type;
  node_id_t a, b;
  for (int i = 0; i < num_threads; ++i) {
    for (int j = 0; j < per_thread; ++j) {
      in >> type >> a >> b;
      updates[i][j] = {{a,b}, (UpdateType)type};
    }
  }
  for (edge_id_t i = per_thread * num_threads; i < m; ++i) {
    in >> type >> a >> b;
    g.update({{a,b}, (UpdateType)type});
  }

  auto task = [&updates, &g](int id) {
      for (auto upd : updates[id]) {
        g.update(upd, id);
      }
    return;
  };

  std::thread threads[num_threads];
  for (int i = 0; i < num_threads; ++i) {
    threads[i] = std::thread(task, i);
  }
  for (int i = 0; i < num_threads; ++i) {
    threads[i].join();
  }

  g.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
  g.connected_components();
}

std::string execCommand(const std::string cmd, int& out_exitStatus)
{
  out_exitStatus = 0;
  auto pPipe = ::popen(cmd.c_str(), "r");
  if(pPipe == nullptr)
  {
    throw std::runtime_error("Cannot open pipe");
  }

  std::array<char, 256> buffer;

  std::string result;

  while(not std::feof(pPipe))
  {
    auto bytes = std::fread(buffer.data(), 1, buffer.size(), pPipe);
    result.append(buffer.data(), bytes);
  }

  auto rc = ::pclose(pPipe);

  if(WIFEXITED(rc))
  {
    out_exitStatus = WEXITSTATUS(rc);
  }

  return result;
}

TEST(GraphTest, TestCorrectnessOfKickstarter) {
  GraphConfiguration config;
  config.gutter_sys = STANDALONE;
  int num_trials = 5;
  int num_fails = 0;
  while(num_trials--) {
    generate_stream({1024,0.002,0.5,0,"./sample.txt","./cumul_sample.txt"});
    std::ifstream in{"./sample.txt"};
    node_id_t n;
    edge_id_t m;
    in >> n >> m;
    Graph g{n, config};
    int type, a, b;
    while (m--) {
      in >> type >> a >> b;
      if (type == INSERT) {
      g.update({{a, b}, INSERT});
      } else g.update({{a, b}, DELETE});
    }
    in.close();

    g.set_verifier(std::make_unique<FileGraphVerifier>("./cumul_sample.txt"));
    auto res = g.connected_components();
    std::set<std::set<node_id_t>> set_res;
    for (auto& s : res) {
      set_res.insert(s);
    }

    // get first insertions into snap format
    std::ifstream snap_in { "./sample.txt" };
    std::ofstream snap_out { "./TEMP_SNAP_F" };
    snap_in >> n >> m;
    while(m--) {
      snap_in >> type >> a >> b;
      if (type == DELETE) break;
      snap_out << a << "\t" << b << "\n";
    }
    ++m;
    snap_out.close();
    int exitStatus = 0;
    std::string convert_cmd = "/home/victor/CODE/graphbolt/tools/converters"
                              "/SNAPtoAdjConverter -s"
                              " ./TEMP_SNAP_F"
                              " ./graph.adj";
    std::string cmd = "/home/victor/CODE/graphbolt/apps/ConnectedComponents"
                      " -s"
                      " -numberOfUpdateBatches 1"
                      " -nEdges " + std::to_string(m) +
                      " -streamPath ./edge_ops.txt"
                      " -outputFile ./KS_OUT_F"
                      " ./graph.adj";
    auto result = execCommand(convert_cmd, exitStatus);
    std::cout << result << std::endl;

    std::ofstream ops_out { "./edge_ops.txt" };
    while(m--) {
      snap_in >> type >> a >> b;
      ops_out << (type == INSERT ? "a" : "d") << "\t" << a << "\t" << b << "\n";
    }
    ops_out.close();


    result = execCommand(cmd, exitStatus);
    std::cout << result << std::endl;

    std::ifstream ks_in { "./KS_OUT_F1" };
    std::vector<std::set<node_id_t>> ccs(n);
    for (unsigned i = 0; i < n; ++i) {
      ks_in >> a >> b >> b >> b;
      ccs[b].insert(a);
    }
    std::set<std::set<node_id_t>> comparison;
    for (unsigned i = 0; i < n; ++i) {
      if (!ccs[i].empty()) {
        comparison.insert(ccs[i]);
      }
    }
    std::vector<std::set<node_id_t>> symdif(set_res.size() + comparison.size());
    auto it = std::set_symmetric_difference(set_res.begin(), set_res.end(),
                                            comparison.begin(), comparison
                                            .end(),
                                            symdif.begin());
    symdif.resize(it - symdif.begin());
    for (auto &s : symdif) {
      std::cout << "{ ";
      std::cerr << "{ ";
      for (auto val : s) {
        std::cout << val << ", ";
        std::cerr << val << ", ";
      }
      std::cout << "},\n";
      std::cerr << "},\n";
    }
    // special case where the graph contains no edges from last, or last few,
    // vertices. if this happens, Kickstarter will have fewer vertices in its
    // vertex set than we expect
    bool in_desc = true;
    for (unsigned i = 1; i <= symdif.size(); ++i) {
      std::set<node_id_t> e = {n - i};
      if (symdif[symdif.size() - i] != e) {
        in_desc = false;
        break;
      }
    }
    if (!in_desc) {
      std::cerr << "FAIL\n";
      std::cout << "FAIL\n";
      ++num_fails;
    }
  }
  std::cout << "Failures: " << num_fails << "/" << num_trials << std::endl;
//  ASSERT_EQ(num_fails, 0);
}
