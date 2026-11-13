#include "wake_detector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "esp_check.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

extern const uint8_t model_data[] asm(WAKE_MODEL_SYMBOL);

namespace {
constexpr char kTag[] = "wake_detector";
constexpr int kFeatureWindowMs = 30;
// The score only peaks after the whole wake word, and the sliding average adds kWindow * feature
// step of lag on top (5 * 10 ms), while the model's receptive field is ~0.7 s. Silence must
// therefore outlast that budget before the gate closes, otherwise a valid hit is dropped.
constexpr int kVadHangoverMs = 1000;
static_assert(WAKE_FEATURE_STEP > 0 && WAKE_FEATURE_STEP <= kFeatureWindowMs);
static_assert(WAKE_PROBABILITY_CUTOFF >= 0 && WAKE_PROBABILITY_CUTOFF <= 1);
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
    ESP_RETURN_ON_FALSE(allocator, ESP_ERR_NO_MEM, kTag, "Allocate model variable arena");
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
    config.window = {kFeatureWindowMs, WAKE_FEATURE_STEP};
    config.filterbank.num_channels = kFeatures;
    config.filterbank.lower_band_limit = 125.0f;
    config.filterbank.upper_band_limit = 7500.0f;
    config.noise_reduction = {10, 0.025f, 0.06f, 0.05f};
    config.pcan_gain_control = {1, 0.95f, 80.0f, 21};
    config.log_scale = {1, 6};
    ESP_RETURN_ON_FALSE(FrontendPopulateState(&config, &frontend_, kWakeSampleRate), ESP_ERR_NO_MEM,
                        kTag, "Initialize fixed-point audio frontend");
    // Normal mode (VAD_MODE_0) reports speech more readily than aggressive modes; the hangover keeps
    // the gate open through the quiet tail of a spoken wake word.
    vad_ = vad_create_with_param(VAD_MODE_0, kWakeSampleRate, kVadFrameMs, 30, kVadHangoverMs);
    ESP_RETURN_ON_FALSE(vad_, ESP_ERR_NO_MEM, kTag, "Initialize ESP-SR VAD");
    ESP_LOGI(kTag, "Model ready, %u tensor arena bytes used, ESP-SR VAD enabled",
             static_cast<unsigned>(interpreter_->arena_used_bytes()));
    return ESP_OK;
}

esp_err_t WakeDetector::Reset() {
    FrontendReset(&frontend_);
    probabilities_.fill(0);
    feature_row_ = 0;
    probability_slot_ = 0;
    probability_count_ = 0;
    vad_reset_trigger(vad_);
    vad_samples_ = 0;
    speech_active_ = false;
    return interpreter_->Reset() == kTfLiteOk ? ESP_OK : ESP_FAIL;
}

void WakeDetector::FeedVad(std::span<const int16_t> pcm) {
    while (!pcm.empty()) {
        const size_t count = std::min(pcm.size(), vad_frame_.size() - vad_samples_);
        std::copy_n(pcm.data(), count, vad_frame_.data() + vad_samples_);
        pcm = pcm.subspan(count);
        vad_samples_ += count;
        if (vad_samples_ == vad_frame_.size()) {
            speech_active_ = vad_process_with_trigger(vad_, vad_frame_.data()) == VAD_SPEECH;
            vad_samples_ = 0;
        }
    }
}

esp_err_t WakeDetector::Process(std::span<const int16_t> pcm, bool &detected, float &score) {
    detected = false;
    while (!pcm.empty()) {
        size_t consumed = 0;
        const auto features = FrontendProcessSamples(&frontend_, pcm.data(), pcm.size(), &consumed);
        ESP_RETURN_ON_FALSE(consumed > 0 && consumed <= pcm.size(), ESP_FAIL, kTag, "Frontend stalled");
        FeedVad(pcm.first(consumed));
        pcm = pcm.subspan(consumed);
        if (features.size == 0) continue;
        ESP_RETURN_ON_FALSE(features.size == kFeatures, ESP_ERR_INVALID_SIZE, kTag, "Unexpected feature width");

        auto *row = input_->data.int8 + feature_row_ * kFeatures;
        for (int i = 0; i < kFeatures; ++i) {
            // The training frontend expresses its fixed-point features in units of 1/25.6.
            const int quantized = std::lround(features.values[i] / (25.6f * input_->params.scale));
            row[i] = std::clamp<int32_t>(quantized + input_->params.zero_point, -128, 127);
        }
        if (++feature_row_ != static_cast<size_t>(input_->dims->data[1])) continue;
        feature_row_ = 0;
        ESP_RETURN_ON_FALSE(interpreter_->Invoke() == kTfLiteOk, ESP_FAIL, kTag, "Model inference");

        probabilities_[probability_slot_] = (output_->data.uint8[0] - output_->params.zero_point) * output_->params.scale;
        probability_slot_ = (probability_slot_ + 1) % kWindow;
        probability_count_ = std::min(probability_count_ + 1, kWindow);
        score = std::accumulate(probabilities_.begin(), probabilities_.end(), 0.0f) / kWindow;
        if (probability_count_ == kWindow && score >= WAKE_PROBABILITY_CUTOFF) {
            if (speech_active_) {
                detected = true;
                break;
            }
            ++vad_blocked_count_;
            ESP_LOGD(kTag, "Wake candidate blocked by VAD (%.3f)", score);
        }
    }
    return ESP_OK;
}
