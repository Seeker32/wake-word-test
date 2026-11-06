#include "wake_detector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "esp_check.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

extern const uint8_t model_data[] asm(WAKE_MODEL_SYMBOL);

namespace {
constexpr char kTag[] = "wake_detector";
}

esp_err_t WakeDetector::Init() {
    const auto *model = tflite::GetModel(model_data);
    ESP_RETURN_ON_FALSE(model->version() == TFLITE_SCHEMA_VERSION, ESP_ERR_NOT_SUPPORTED, kTag, "TFLite schema");

    const std::array registrations = {
        ops_.AddCallOnce(), ops_.AddVarHandle(), ops_.AddReshape(), ops_.AddReadVariable(),
        ops_.AddConcatenation(), ops_.AddStridedSlice(), ops_.AddAssignVariable(),
        ops_.AddConv2D(), ops_.AddDepthwiseConv2D(), ops_.AddSplitV(),
        ops_.AddFullyConnected(), ops_.AddLogistic(), ops_.AddQuantize(),
    };
    ESP_RETURN_ON_FALSE(std::ranges::all_of(registrations, [](auto status) { return status == kTfLiteOk; }),
                        ESP_FAIL, kTag, "Register model operators");

    auto *allocator = tflite::MicroAllocator::Create(variable_arena_.data(), variable_arena_.size());
    auto *variables = tflite::MicroResourceVariables::Create(allocator, 20);
    ESP_RETURN_ON_FALSE(variables, ESP_ERR_NO_MEM, kTag, "Allocate model variable table");
    interpreter_.emplace(model, ops_, tensor_arena_.data(), tensor_arena_.size(), variables);
    ESP_RETURN_ON_FALSE(interpreter_->AllocateTensors() == kTfLiteOk, ESP_ERR_NO_MEM, kTag, "Allocate model tensors");
    input_ = interpreter_->input(0);
    output_ = interpreter_->output(0);
    ESP_RETURN_ON_FALSE(input_ && input_->type == kTfLiteInt8 && input_->dims->size == 3 &&
                            input_->dims->data[0] == 1 && input_->dims->data[1] > 0 &&
                            input_->dims->data[2] == kFeatures && input_->params.scale > 0 &&
                            output_ && output_->type == kTfLiteUInt8 && output_->bytes == 1 && output_->params.scale > 0,
                        ESP_ERR_NOT_SUPPORTED, kTag, "Unexpected model input or output");

    FrontendConfig config = {};
    config.window = {30, WAKE_FEATURE_STEP};
    config.filterbank.num_channels = kFeatures;
    config.filterbank.lower_band_limit = 125.0f;
    config.filterbank.upper_band_limit = 7500.0f;
    config.noise_reduction = {10, 0.025f, 0.06f, 0.05f};
    config.pcan_gain_control = {1, 0.95f, 80.0f, 21};
    config.log_scale = {1, 6};
    ESP_RETURN_ON_FALSE(FrontendPopulateState(&config, &frontend_, kWakeSampleRate), ESP_ERR_NO_MEM,
                        kTag, "Initialize fixed-point audio frontend");
    ESP_LOGI(kTag, "Model ready, %u tensor arena bytes used", static_cast<unsigned>(interpreter_->arena_used_bytes()));
    return ESP_OK;
}

esp_err_t WakeDetector::Reset() {
    FrontendReset(&frontend_);
    probabilities_.fill(0);
    feature_row_ = 0;
    probability_slot_ = 0;
    probability_count_ = 0;
    return interpreter_->Reset() == kTfLiteOk ? ESP_OK : ESP_FAIL;
}

esp_err_t WakeDetector::Process(std::span<const int16_t> pcm, bool &detected, float &score) {
    detected = false;
    while (!pcm.empty()) {
        size_t consumed = 0;
        const auto features = FrontendProcessSamples(&frontend_, pcm.data(), pcm.size(), &consumed);
        ESP_RETURN_ON_FALSE(consumed > 0 && consumed <= pcm.size(), ESP_FAIL, kTag, "Frontend stalled");
        pcm = pcm.subspan(consumed);
        if (features.size == 0) continue;
        ESP_RETURN_ON_FALSE(features.size == kFeatures, ESP_ERR_INVALID_SIZE, kTag, "Unexpected feature width");

        auto *row = input_->data.int8 + feature_row_ * kFeatures;
        for (int i = 0; i < kFeatures; ++i) {
            // The training frontend expresses its fixed-point features in units of 1/25.6.
            const int quantized = std::lround(features.values[i] / (25.6f * input_->params.scale));
            row[i] = std::clamp(quantized + input_->params.zero_point, -128, 127);
        }
        if (++feature_row_ != static_cast<size_t>(input_->dims->data[1])) continue;
        feature_row_ = 0;
        ESP_RETURN_ON_FALSE(interpreter_->Invoke() == kTfLiteOk, ESP_FAIL, kTag, "Model inference");

        probabilities_[probability_slot_] = (output_->data.uint8[0] - output_->params.zero_point) * output_->params.scale;
        probability_slot_ = (probability_slot_ + 1) % kWindow;
        probability_count_ = std::min(probability_count_ + 1, kWindow);
        score = std::accumulate(probabilities_.begin(), probabilities_.end(), 0.0f) / kWindow;
        if (probability_count_ == kWindow && score >= WAKE_PROBABILITY_CUTOFF) {
            detected = true;
            break;
        }
    }
    return ESP_OK;
}
