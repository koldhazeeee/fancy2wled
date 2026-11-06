// Runs the benchmark cases on this computer:
//   python bench/run_host.py [--label before] [--out bench/results/host-before.json]
// (which compiles this file with the core and runs it). Numbers are per operation; compare runs of the same machine only.
#include <chrono>
#include <stdio.h>
#include <string.h>

#include "bench_runner.h"

static uint64_t nowNs() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
  const char* out = nullptr;
  const char* label = "run";
  int reps = 15;
  double ms = 30;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = atof(argv[++i]);
  }
  benchInit();
  FILE* f = out ? fopen(out, "w") : nullptr;
  if (f) fprintf(f, "{\"label\":\"%s\",\"platform\":\"host\",\"unit\":\"ns per operation\",\"cases\":{\n", label);
  printf("%-24s %14s %14s   %s\n", "case", "fastest (ns)", "median (ns)", "what");
  const int n = benchCaseCount();
  for (int i = 0; i < n; i++) {
    const BenchCase& c = benchCase(i);
    const BenchResult r = benchMeasure(c, nowNs, ms, reps);
    printf("%-24s %14.1f %14.1f   %s\n", c.name, r.minNs, r.medNs, c.what);
    if (f) fprintf(f, "%s\"%s\":{\"min\":%.2f,\"med\":%.2f,\"iters\":%d}\n", i ? "," : "", c.name, r.minNs, r.medNs, r.iters);
  }
  if (f) { fprintf(f, "}}\n"); fclose(f); }
  return gBenchSink == 0xFFFFFFFFu;            // (never true; keeps the sink alive)
}
