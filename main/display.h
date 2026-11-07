#pragma once

#include <cstddef>

#include "esp_err.h"

// On-screen dashboard for the ESP32-S3-BOX-3 LCD: CPU load, internal heap and the wake cutoff.
// Every call below is thread safe and a no-op when InitDisplay() failed, so a missing or
// misconfigured panel never stops detection.

esp_err_t InitDisplay();

// Latest smoothed wake score (0..1) from the audio loop, shown next to the cutoff.
void DisplayScore(float score);

// CPU busy percentages (-1 when unavailable) and internal heap bytes, as logged in the RES line.
void DisplayResources(int cpu0_pct, int cpu1_pct, size_t heap_free, size_t heap_min, size_t heap_largest);
