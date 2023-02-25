#pragma once
#include <xxhash.h>
#include <graph_zeppelin_common.h>

typedef uint64_t col_hash_t;
static const auto& vec_hash = XXH3_64bits_withSeed;
static const auto& col_hash = XXH3_64bits_withSeed;

// Is a stream update an insertion or a deletion
// BREAKPOINT: special type that indicates that a break point has been reached
// a break point may be either the end of the stream or the index of a query
enum UpdateType {
  INSERT = 0,
  DELETE = 1,
  BREAKPOINT = 2
};

struct Edge {
  node_id_t src = 0;
  node_id_t dst = 0;

  bool operator< (const Edge&oth) const {
    if (src == oth.src)
      return dst < oth.dst;
    return src < oth.src;
  }
};

struct GraphUpdate {
  Edge edge;
  UpdateType type;
};

#define likely_if(x) if(__builtin_expect((bool)(x), true))
#define unlikely_if(x) if (__builtin_expect((bool)(x), false))
