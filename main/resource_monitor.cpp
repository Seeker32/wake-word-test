#include "resource_monitor.h"

#include <algorithm>

#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && \
    CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 && !CONFIG_FREERTOS_SMP
namespace {
constexpr int CpuBusyPercent(uint64_t elapsed_us, uint64_t idle_us) {
    return (elapsed_us - std::min(idle_us, elapsed_us)) * 100 / elapsed_us;
}

static_assert(CpuBusyPercent(5000, 3750) == 25);
static_assert(CpuBusyPercent(5000, 5001) == 0);
}  // namespace

void ResourceMonitor::ReadCpuCounters(void *context) {
    auto &counters = *static_cast<CpuCounters *>(context);
    counters.time_us = esp_timer_get_time();
    counters.idle_us = ulTaskGetIdleRunTimeCounterForCore(xPortGetCoreID());
}
#endif

ResourceSnapshot ResourceMonitor::Sample(TaskHandle_t task) {
    constexpr uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ResourceSnapshot snapshot = {
        {},
        heap_caps_get_free_size(heap_caps),
        heap_caps_get_minimum_free_size(heap_caps),
        heap_caps_get_largest_free_block(heap_caps),
        uxTaskGetStackHighWaterMark(task),
    };
    snapshot.cpu_busy_pct.fill(-1);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && \
    CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 && !CONFIG_FREERTOS_SMP
    for (int core = 0; core < configNUMBER_OF_CORES; ++core) {
        CpuCounters current{};
        if (core == xPortGetCoreID()) {
            ReadCpuCounters(&current);
        }
#if CONFIG_ESP_IPC_ENABLE
        else {
            // Run on the other core so its current Idle slice is accounted for first.
            if (esp_ipc_call_blocking(core, ReadCpuCounters, &current) != ESP_OK) {
                current = {};
            }
        }
#endif
        const auto &previous = previous_[core];
        if (previous.time_us > 0 && current.time_us > previous.time_us &&
            current.idle_us >= previous.idle_us) {
            snapshot.cpu_busy_pct[core] = CpuBusyPercent(current.time_us - previous.time_us,
                                                        current.idle_us - previous.idle_us);
        }
        previous_[core] = current;
    }
#endif
    return snapshot;
}
