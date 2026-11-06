// The board's benchmark build (pio run -e atoms3-bench -t upload; then python bench/run_board.py): instead of the relay it runs the benchmark cases of
// bench/bench_cases.cpp on the real ESP32-S3 and prints the timings over USB serial. No Wi-Fi, no other task: nothing disturbs the measurement.
// It exists only in that environment (FWB_BENCH); the normal firmware does not contain it.
#ifdef FWB_BENCH
#include <Arduino.h>
#include <esp_timer.h>

#include "bench_runner.h"

static uint64_t nowNs() { return (uint64_t)esp_timer_get_time() * 1000ull; }

void benchMain() {
  Serial.begin(115200);
  delay(1500);
  benchInit();
  for (;;) {                                     // runs the whole set over and over, so a reader can attach at any time
    Serial.printf("BENCH_BEGIN cpu_mhz=%d\n", (int)getCpuFrequencyMhz());
    const int n = benchCaseCount();
    for (int i = 0; i < n; i++) {
      const BenchCase& c = benchCase(i);
      const BenchResult r = benchMeasure(c, nowNs, 40.0, 9);
      Serial.printf("BENCH {\"n\":\"%s\",\"min\":%.1f,\"med\":%.1f,\"iters\":%d}\n", c.name, r.minNs, r.medNs, r.iters);
    }
    Serial.printf("BENCH_END sink=%u\n", (unsigned)gBenchSink);
    delay(500);
  }
}
#endif
