#include "board_audio.h"
#include "display.h"
#include "resource_monitor.h"
#include "wav.h"
#include "wake_detector.h"

#include <algorithm>
#include <array>
#include <cinttypes>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const uint8_t prompt_start[] asm("_binary_wake_up_wav_start");
extern const uint8_t prompt_end[] asm("_binary_wake_up_wav_end");

extern "C" void app_main() {
    constexpr char tag[] = "wake_word";
    const auto prompt = ParsePcm16MonoWave({prompt_start, static_cast<size_t>(prompt_end - prompt_start)});
    ESP_ERROR_CHECK(prompt.samples.empty() ? ESP_ERR_INVALID_ARG : ESP_OK);
    static WakeDetector detector;
    ESP_ERROR_CHECK(detector.Init());
    ESP_ERROR_CHECK(InitBoardAudio());
    if (const esp_err_t display = InitDisplay(); display != ESP_OK) {
        ESP_LOGE(tag, "Dashboard unavailable (%s), continuing without it", esp_err_to_name(display));
    }

    constexpr int64_t report_interval_us = 5'000'000;
    ResourceMonitor monitor;
    monitor.Sample();
    int64_t last_report_us = esp_timer_get_time();
    int64_t process_total_us = 0;
    int64_t process_max_us = 0;
    size_t process_count = 0;

    std::array<int16_t, kWakeReadSamples> pcm;
    float last_score = 0;
    ESP_LOGI(tag, "Listening for %s", WAKE_WORD);
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t window_us = now_us - last_report_us;
        if (window_us >= report_interval_us) {
            const auto resources = monitor.Sample();
            const int cpu1_pct = resources.cpu_busy_pct.size() > 1 ? resources.cpu_busy_pct[1] : -1;
            ESP_LOGI(tag,
                     "RES window_ms=%" PRId64 " cpu0_pct=%d cpu1_pct=%d heap_free_B=%zu heap_min_B=%zu "
                     "heap_largest_B=%zu main_stack_min_B=%zu process_count=%zu process_avg_us=%" PRId64
                     " process_max_us=%" PRId64,
                     window_us / 1000, resources.cpu_busy_pct[0], cpu1_pct, resources.heap_free_bytes,
                     resources.heap_min_free_bytes, resources.heap_largest_free_block_bytes,
                     resources.stack_min_free_bytes, process_count,
                     process_count ? process_total_us / static_cast<int64_t>(process_count) : 0, process_max_us);
            DisplayResources(resources.cpu_busy_pct[0], cpu1_pct, resources.heap_free_bytes,
                             resources.heap_min_free_bytes, resources.heap_largest_free_block_bytes);
            last_report_us = now_us;
            process_total_us = 0;
            process_max_us = 0;
            process_count = 0;
        }

        size_t samples;
        if (ReadWakePcm(pcm, samples) != ESP_OK) {
            ESP_ERROR_CHECK(detector.Reset());
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool detected;
        // Process only writes score when it ran an inference, so carry the last value forward.
        float score = last_score;
        const int64_t process_start_us = esp_timer_get_time();
        ESP_ERROR_CHECK(detector.Process(std::span(pcm).first(samples), detected, score));
        last_score = score;
        DisplayScore(score);
        const int64_t process_us = esp_timer_get_time() - process_start_us;
        process_total_us += process_us;
        process_max_us = std::max(process_max_us, process_us);
        ++process_count;
        if (detected) {
            ESP_LOGI(tag, "Wake word detected: %s (%.3f)", WAKE_WORD, score);
            const int64_t playback_start_us = esp_timer_get_time();
            const esp_err_t playback = PlayWakePrompt(prompt.samples, prompt.sample_rate);
            const int64_t playback_ms = (esp_timer_get_time() - playback_start_us) / 1000;
            ESP_ERROR_CHECK_WITHOUT_ABORT(playback);
            ESP_LOGI(tag, "WAKE playback_ms=%" PRId64, playback_ms);
            ESP_ERROR_CHECK(detector.Reset());
            ESP_LOGI(tag, "Listening for %s", WAKE_WORD);
        }
    }
}
