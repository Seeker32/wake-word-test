#include "board_audio.h"
#include "wav.h"
#include "wake_detector.h"

#include <array>

#include "esp_log.h"
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

    std::array<int16_t, kWakeReadSamples> pcm;
    ESP_LOGI(tag, "Listening for %s", WAKE_WORD);
    for (;;) {
        size_t samples;
        if (ReadWakePcm(pcm, samples) != ESP_OK) {
            ESP_ERROR_CHECK(detector.Reset());
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool detected;
        float score;
        ESP_ERROR_CHECK(detector.Process(std::span(pcm).first(samples), detected, score));
        if (detected) {
            ESP_LOGI(tag, "Wake word detected: %s (%.3f)", WAKE_WORD, score);
            ESP_ERROR_CHECK_WITHOUT_ABORT(PlayWakePrompt(prompt.samples, prompt.sample_rate));
            ESP_ERROR_CHECK(detector.Reset());
            ESP_LOGI(tag, "Listening for %s", WAKE_WORD);
        }
    }
}
