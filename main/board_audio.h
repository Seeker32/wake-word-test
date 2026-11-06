#pragma once

#include <cstdint>
#include <span>

#include "esp_err.h"

constexpr uint32_t kWakeSampleRate = 16000;
constexpr size_t kWakeReadSamples = 256;

esp_err_t InitBoardAudio();
esp_err_t ReadWakePcm(std::span<int16_t, kWakeReadSamples> pcm, size_t &samples);
esp_err_t PlayWakePrompt(std::span<const uint8_t> pcm, uint32_t sample_rate);
