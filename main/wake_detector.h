#pragma once

#include <array>
#include <optional>

#include "board_audio.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

class WakeDetector {
public:
    esp_err_t Init();
    esp_err_t Reset();
    esp_err_t Process(std::span<const int16_t> pcm, bool &detected, float &score);

private:
    static constexpr int kFeatures = 40;
    static constexpr size_t kWindow = WAKE_SLIDING_WINDOW;
    static_assert(kWindow > 0);

    alignas(16) std::array<uint8_t, WAKE_TENSOR_ARENA> tensor_arena_;
    alignas(16) std::array<uint8_t, 1024> variable_arena_;
    tflite::MicroMutableOpResolver<13> ops_;
    std::optional<tflite::MicroInterpreter> interpreter_;
    FrontendState frontend_{};
    TfLiteTensor *input_ = nullptr;
    TfLiteTensor *output_ = nullptr;
    std::array<float, kWindow> probabilities_{};
    size_t feature_row_ = 0;
    size_t probability_slot_ = 0;
    size_t probability_count_ = 0;
};
