// Board layer for the breadboard "compact-wifi" wiring popularized by
// xiaozhi-esp32: an INMP441 MEMS microphone and a MAX98357A amplifier on
// separate (simplex) I2S ports with no codec control interface.
// Wiring follows xiaozhi-esp32/main/boards/bread-compact-wifi/config.h.

#include "board_audio.h"
#include "resample_util.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char kTag[] = "bread_audio";
constexpr uint32_t kSpkRate = 24000;
constexpr size_t kMicFrames = kWakeSampleRate / 100;  // 10 ms of frames per read
static_assert(kMicFrames <= kWakeReadSamples, "10 ms capture must fit the wake buffer");
constexpr size_t kDmaBlocks = 6;

constexpr gpio_num_t kMicWsPin = GPIO_NUM_4;
constexpr gpio_num_t kMicSckPin = GPIO_NUM_5;
constexpr gpio_num_t kMicDinPin = GPIO_NUM_6;
constexpr gpio_num_t kSpkBclkPin = GPIO_NUM_15;
constexpr gpio_num_t kSpkWsPin = GPIO_NUM_16;
constexpr gpio_num_t kSpkDoutPin = GPIO_NUM_7;

i2s_chan_handle_t tx;
i2s_chan_handle_t rx;

esp_err_t InitI2s() {
    i2s_chan_config_t spk_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    spk_chan.dma_desc_num = kDmaBlocks;
    spk_chan.dma_frame_num = kMicFrames;
    spk_chan.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan, &tx, nullptr), kTag, "Speaker channel");

    i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    mic_chan.dma_desc_num = kDmaBlocks;
    mic_chan.dma_frame_num = kMicFrames;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan, nullptr, &rx), kTag, "Microphone channel");

    // Both devices run 32-bit mono (left slot) Philips frames like xiaozhi's
    // no-codec driver: the INMP441 needs wide slots, the MAX98357A reads MSBs.
    i2s_std_config_t spk = {};
    spk.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSpkRate);
    spk.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    spk.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    spk.gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = kSpkBclkPin, .ws = kSpkWsPin,
                     .dout = kSpkDoutPin, .din = I2S_GPIO_UNUSED, .invert_flags = {} };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &spk), kTag, "Init speaker");

    i2s_std_config_t mic = {};
    mic.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kWakeSampleRate);
    mic.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    mic.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    mic.gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = kMicSckPin, .ws = kMicWsPin,
                     .dout = I2S_GPIO_UNUSED, .din = kMicDinPin, .invert_flags = {} };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &mic), kTag, "Init microphone");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), kTag, "Enable speaker clock");
    return i2s_channel_enable(rx);
}
}  // namespace

esp_err_t InitBoardAudio() {
    return InitI2s();
}

esp_err_t ReadWakePcm(std::span<int16_t, kWakeReadSamples> pcm, size_t &samples) {
    std::array<int32_t, kMicFrames> raw;
    samples = 0;
    size_t bytes_read = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_read(rx, raw.data(), sizeof(raw), &bytes_read, portMAX_DELAY),
                        kTag, "Read microphone");
    const size_t frames = std::min(bytes_read / sizeof(int32_t), pcm.size());
    for (size_t i = 0; i < frames; ++i) {
        // INMP441 data is left-aligned in the 32-bit slot; scale it exactly
        // like xiaozhi's no-codec driver (top 20 bits clamped to int16).
        const int32_t value = raw[i] >> 12;
        pcm[i] = static_cast<int16_t>(std::clamp(value, -int32_t{INT16_MAX}, int32_t{INT16_MAX}));
    }
    samples = frames;
    return ESP_OK;
}

esp_err_t PlayWakePrompt(std::span<const uint8_t> pcm, uint32_t sample_rate) {
    if (pcm.size() % sizeof(int16_t) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const int16_t *input = reinterpret_cast<const int16_t *>(pcm.data());
    size_t input_samples = pcm.size() / sizeof(int16_t);

    std::vector<int16_t> converted;
    if (sample_rate != kSpkRate) {
        ESP_RETURN_ON_ERROR(ResamplePcm16Mono({input, input_samples}, sample_rate, kSpkRate, converted),
                            kTag, "Resample prompt");
        input = converted.data();
        input_samples = converted.size();
    }

    std::array<int32_t, kMicFrames> block;
    esp_err_t playback = ESP_OK;
    size_t offset = 0;
    while (offset < input_samples && playback == ESP_OK) {
        const size_t chunk = std::min(input_samples - offset, block.size());
        for (size_t i = 0; i < chunk; ++i) {
            block[i] = static_cast<int32_t>(input[offset + i]) << 16;  // 16 MSBs
        }
        size_t bytes_written = 0;
        playback = i2s_channel_write(tx, block.data(), chunk * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        offset += chunk;
    }

    // Flush the TX DMA queue and discard capture containing the prompt or its tail.
    block.fill(0);
    for (size_t i = 0; i < kDmaBlocks + 10 && playback == ESP_OK; ++i) {
        size_t bytes_written = 0;
        playback = i2s_channel_write(tx, block.data(), sizeof(block), &bytes_written, portMAX_DELAY);
    }
    std::array<int32_t, kMicFrames> discard;
    for (size_t i = 0; i < kDmaBlocks; ++i) {
        size_t bytes_read = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_read(rx, discard.data(), sizeof(discard), &bytes_read, portMAX_DELAY),
                            kTag, "Drain microphone");
    }
    return playback;
}
