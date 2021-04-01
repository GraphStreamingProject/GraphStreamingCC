#pragma once
#include <cmath>
#include <exception>
#include <iostream>
#include <vector>
#include "bucket.h"
#include "prime_generator.h"
#include "types.h"
#include "update.h"
#include "util.h"
#include <gtest/gtest_prod.h>
#include <boost/serialization/access.hpp>

#define bucket_gen(x) double_to_ull(log2(x)+1)
#define guess_gen(x) double_to_ull(log2(x)+2)

/**
 * An implementation of a "sketch" as defined in the L0 algorithm.
 * Note a sketch may only be queried once. Attempting to query multiple times will
 * raise an error.
 */
class Sketch {
  friend class boost::serialization::access;
  template<class Archive> void serialize(Archive & ar, const unsigned int version);

  std::vector<Bucket_Boruvka> buckets;
  ubucket_t large_prime;
  bool already_quered = false;

  FRIEND_TEST(SketchTestSuite, TestExceptions);
  FRIEND_TEST(SketchTestSuite, TestBatchUpdate);

  //Initialize a sketch of a vector of size n
public:
  Sketch(vec_t n, long seed);
  long seed;
  vec_t n;
  /**
   * Update a sketch based on information about one of its indices.
   * @param update the point update.
   */
  void update(Update update);

  /**
   * Update a sketch given a batch of updates
   * @param begin a ForwardIterator to the first update
   * @param end a ForwardIterator to after the last update
   */
  void batch_update(const std::vector<Update> &updates);

  /**
   * Function to query a sketch.
   * @return                        an index in the form of an Update.
   * @throws MultipleQueryException if the sketch has already been queried.
   * @throws NoGoodBucketException  if there are no good buckets to choose an
   *                                index from.
   */
  Update query();

  friend Sketch operator+ (const Sketch &sketch1, const Sketch &sketch2);
  friend Sketch &operator+= (Sketch &sketch1, const Sketch &sketch2);
  friend Sketch operator* (const Sketch &sketch1, long scaling_factor );
  friend std::ostream& operator<< (std::ostream &os, const Sketch &sketch);
};

class AllBucketsZeroException : public std::exception {
public:
  virtual const char* what() const throw() {
    return "All buckets zero";
  }
};

class MultipleQueryException : public std::exception {
public:
  virtual const char* what() const throw() {
    return "This sketch has already been sampled!";
  }
};

class NoGoodBucketException : public std::exception {
public:
  virtual const char* what() const throw() {
    return "Found no good bucket!";
  }
};
