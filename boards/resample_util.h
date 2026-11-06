#pragma once

// One-shot 16-bit mono resampler for short prompt buffers. Board layers use it
// when the embedded prompt rate does not match the board's playback rate.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "esp_ae_rate_cvt.h"
#include "esp_check.h"

// Resamples a 16-bit mono buffer and returns the result in `output`.
// A short silence tail is fed to the converter so internally buffered samples
// are emitted, then the result is trimmed to the expected length.
inline esp_err_t ResamplePcm16Mono(std::span<const int16_t> input, uint32_t src_rate,
                                   uint32_t dest_rate, std::vector<int16_t> &output) {
    constexpr char kTag[] = "resample_util";
    if (input.empty() || input.size() > UINT32_MAX || src_rate == 0 || dest_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_rate == dest_rate) {
        output.assign(input.begin(), input.end());
        return ESP_OK;
    }

    esp_ae_rate_cvt_cfg_t cfg = {};
    cfg.src_rate = src_rate;
    cfg.dest_rate = dest_rate;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.complexity = 2;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    esp_ae_rate_cvt_handle_t cvt = nullptr;
    ESP_RETURN_ON_FALSE(esp_ae_rate_cvt_open(&cfg, &cvt) == ESP_AE_ERR_OK, ESP_FAIL, kTag, "Open resampler");

    const uint32_t tail = std::min<uint32_t>(dest_rate / 1000 * 5, 256);  // 5 ms flush
    const uint32_t input_samples = static_cast<uint32_t>(input.size());
    const size_t expected = input_samples * dest_rate / src_rate;
    esp_err_t err = ESP_FAIL;
    do {
        uint32_t max_out = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(cvt, input_samples + tail, &max_out) != ESP_AE_ERR_OK) {
            break;
        }
        output.resize(max_out);
        uint32_t produced = max_out;
        if (esp_ae_rate_cvt_process(cvt, const_cast<int16_t *>(input.data()), input_samples,
                                    output.data(), &produced) != ESP_AE_ERR_OK) {
            break;
        }
        std::array<int16_t, 256> silence = {};
        uint32_t flushed = max_out - produced;
        if (esp_ae_rate_cvt_process(cvt, silence.data(), tail, output.data() + produced, &flushed) != ESP_AE_ERR_OK) {
            break;
        }
        output.resize(std::min<size_t>(produced + flushed, expected));
        err = ESP_OK;
    } while (false);
    esp_ae_rate_cvt_close(cvt);
    return err;
}
