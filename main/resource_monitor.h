#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct ResourceSnapshot {
    std::array<int, configNUMBER_OF_CORES> cpu_busy_pct;
    size_t heap_free_bytes;
    size_t heap_min_free_bytes;
    size_t heap_largest_free_block_bytes;
    size_t stack_min_free_bytes;
};

class ResourceMonitor {
public:
    // Call from task context. CPU values are -1 until a sampling window is available.
    ResourceSnapshot Sample(TaskHandle_t task = nullptr);

private:
    struct CpuCounters {
        int64_t time_us;
        uint64_t idle_us;
    };

    static void ReadCpuCounters(void *context);

    std::array<CpuCounters, configNUMBER_OF_CORES> previous_{};
};
