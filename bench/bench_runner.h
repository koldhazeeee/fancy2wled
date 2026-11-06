// Timing loop shared by the PC runner and the board's benchmark build: picks an iteration count so one repetition takes about targetMs, then times `reps`
// repetitions and reports the fastest and the median time per operation (the fastest is the least disturbed by interrupts and other tasks).
#pragma once
#include <stdint.h>
#include <stdlib.h>

#include "bench_cases.h"

typedef uint64_t (*BenchClockNs)();          // nanoseconds, any origin

struct BenchResult {
  double minNs = 0, medNs = 0;
  int iters = 0;
};

inline int benchCmpDouble(const void* a, const void* b) {
  const double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

inline BenchResult benchMeasure(const BenchCase& c, BenchClockNs now, double targetMs, int reps) {
  BenchResult r;
  c.run(1);                                    // warm up: code and data caches
  int iters = 1;
  for (;;) {                                   // find an iteration count that takes about a quarter of the target
    const uint64_t t0 = now();
    c.run(iters);
    const double ms = (double)(now() - t0) / 1e6;
    if (ms >= targetMs / 4 || iters >= (1 << 26)) {
      iters = (int)((double)iters * targetMs / (ms > 0.0001 ? ms : 0.0001));
      if (iters < 1) iters = 1;
      break;
    }
    iters *= 2;
  }
  double t[64];
  if (reps > 64) reps = 64;
  for (int i = 0; i < reps; i++) {
    const uint64_t t0 = now();
    c.run(iters);
    t[i] = (double)(now() - t0) / (double)iters;
  }
  qsort(t, (size_t)reps, sizeof t[0], benchCmpDouble);
  r.minNs = t[0];
  r.medNs = t[reps / 2];
  r.iters = iters;
  return r;
}
