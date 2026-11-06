// Benchmark cases for the relay's hot paths: see bench_cases.cpp and docs/PERFORMANCE.md.
#pragma once
#include <stdint.h>

struct BenchCase {
  const char* name;
  const char* what;
  void (*run)(int iters);          // does `iters` repetitions of the operation
};
extern volatile uint32_t gBenchSink;
void benchInit();                  // builds the inputs (once)
int benchCaseCount();
const BenchCase& benchCase(int i);
