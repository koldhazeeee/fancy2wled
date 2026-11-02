// Optional performance statistics, for finding out how hard the board is working (setup page -> Home Assistant (MQTT) ->
// "Performance stats (debug)"). Everything here costs nothing unless the switch is on: no timers run, no counters tick, and the
// instrumented code paths only test one flag.
//
// While on:
//  * CPU load per core: a hardware timer fires about 1400 times a second on each core and notes whether the idle task was the one
//    running. The share of "not idle" samples is the load. (The firmware build has no FreeRTOS run-time counters, and
//    counting idle-hook calls reads far too high for the short bursts this program works in.)
//  * Main loop: how much of the time the loop spends working, and its longest pass in the window (a stall detector).
//  * Frames: how many go out to WLED per second and how long each takes (colour mixing + smoothing + sending).
//  * Memory: free heap, the lowest it has been since boot, the largest free block, and how much stack the tasks have left.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"

volatile bool gPerfOn = false;

static const uint32_t kSampleUs = 731;       // not a multiple of the 1 ms RTOS tick, so the samples are not locked to it
static volatile uint32_t gBusy[2] = {0, 0}, gTotal[2] = {0, 0};
static TaskHandle_t gIdle[2] = {nullptr, nullptr};
static hw_timer_t* gTimer[2] = {nullptr, nullptr};
static uint32_t gPrevBusy[2], gPrevTotal[2];
static uint32_t gWinStart = 0;
static uint32_t gLoopT0 = 0, gLoopWork = 0, gLoopMax = 0;
static uint32_t gFrameSum = 0, gFrameMax = 0, gFrameN = 0;

static void IRAM_ATTR sampleCore(int core) {
  gTotal[core]++;
  if (xTaskGetCurrentTaskHandleForCPU(core) != gIdle[core]) gBusy[core]++;
}
static void IRAM_ATTR isrCore0() { sampleCore(0); }
static void IRAM_ATTR isrCore1() { sampleCore(1); }

// A timer's interrupt is serviced by the core that attached it, so each timer is started (and stopped) on its own core.
static void timerOn(int core) {
  gIdle[core] = xTaskGetIdleTaskHandleForCPU(core);
  hw_timer_t* t = timerBegin(core, 80, true);                 // 80 MHz / 80 = 1 tick per microsecond
  if (!t) return;
  timerAttachInterrupt(t, core ? isrCore1 : isrCore0, true);
  timerAlarmWrite(t, kSampleUs, true);
  timerAlarmEnable(t);
  gTimer[core] = t;
}
static void timerOff(int core) {
  if (!gTimer[core]) return;
  timerAlarmDisable(gTimer[core]);
  timerDetachInterrupt(gTimer[core]);
  timerEnd(gTimer[core]);
  gTimer[core] = nullptr;
}

struct Job { int core; bool start; volatile bool done; };
static void jobTask(void* p) {
  Job* j = (Job*)p;
  if (j->start) timerOn(j->core); else timerOff(j->core);
  j->done = true;
  vTaskDelete(nullptr);
}
static void runOnCore(int core, bool start) {
  if ((int)xPortGetCoreID() == core) { if (start) timerOn(core); else timerOff(core); return; }
  static Job job;
  job.core = core; job.start = start; job.done = false;
  xTaskCreatePinnedToCore(jobTask, "perfjob", 3072, &job, 1, nullptr, core);
  for (int i = 0; i < 300 && !job.done; i++) delay(1);
}

void perfSync(bool wanted) {
  if (wanted == gPerfOn) return;
  if (wanted) {
    for (int c = 0; c < 2; c++) { gBusy[c] = gTotal[c] = 0; gPrevBusy[c] = gPrevTotal[c] = 0; }
    gLoopT0 = gLoopWork = gLoopMax = 0;
    gFrameSum = gFrameMax = gFrameN = 0;
    gWinStart = micros();
    runOnCore(0, true);
    runOnCore(1, true);
    gPerfOn = true;
  } else {
    gPerfOn = false;
    runOnCore(0, false);
    runOnCore(1, false);
  }
}

void perfLoopBegin() { gLoopT0 = micros(); }
void perfLoopEnd() {
  if (!gLoopT0) return;
  const uint32_t d = micros() - gLoopT0;
  gLoopT0 = 0;
  gLoopWork += d;
  if (d > gLoopMax) gLoopMax = d;
}
void perfFrame(uint32_t us) {
  gFrameSum += us;
  gFrameN++;
  if (us > gFrameMax) gFrameMax = us;
}

void perfTake(PerfSnapshot& p) {
  const uint32_t now = micros();
  const uint32_t win = now - gWinStart;
  p.windowS = win / 1e6f;
  for (int c = 0; c < 2; c++) {
    const uint32_t b = gBusy[c], t = gTotal[c];
    const uint32_t db = b - gPrevBusy[c], dt = t - gPrevTotal[c];
    gPrevBusy[c] = b; gPrevTotal[c] = t;
    (c ? p.cpu1 : p.cpu0) = dt ? 100.f * db / dt : 0.f;
  }
  p.loopBusy = win ? 100.f * gLoopWork / win : 0.f;
  if (p.loopBusy > 100.f) p.loopBusy = 100.f;
  p.loopMaxMs = gLoopMax / 1000.f;
  p.frameAvgMs = gFrameN ? gFrameSum / 1000.f / gFrameN : 0.f;
  p.frameMaxMs = gFrameMax / 1000.f;
  p.outFps = p.windowS > 0.f ? gFrameN / p.windowS : 0.f;
  p.heapFree = esp_get_free_heap_size();
  p.heapMin = esp_get_minimum_free_heap_size();
  p.heapBig = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  p.stackLoop = (uint32_t)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  TaskHandle_t m = xTaskGetHandle("mqtt_task");
  p.stackMqtt = m ? (uint32_t)uxTaskGetStackHighWaterMark(m) * sizeof(StackType_t) : 0;
  gWinStart = now;
  gLoopWork = gLoopMax = 0;
  gFrameSum = gFrameMax = gFrameN = 0;
}
