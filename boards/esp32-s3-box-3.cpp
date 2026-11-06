#include "board_audio.h"
#include "resample_util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_ae_rate_cvt.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "box3_audio";
constexpr uint32_t kAudioRate = 24000;
constexpr gpio_num_t kWsPin = GPIO_NUM_45;
constexpr gpio_num_t kMclkPin = GPIO_NUM_2;
constexpr gpio_num_t kBclkPin = GPIO_NUM_17;
constexpr gpio_num_t kDinPin = GPIO_NUM_16;
constexpr gpio_num_t kDoutPin = GPIO_NUM_15;
constexpr int kPaPin = GPIO_NUM_46;
constexpr gpio_num_t kSdaPin = GPIO_NUM_8;
constexpr gpio_num_t kSclPin = GPIO_NUM_18;

constexpr uint32_t kReadSamples = kAudioRate * 8 / 1000;
// The rate converter needs 128 samples of headroom beyond the resampled block.
constexpr size_t kResamplerPadSamples = 128;
static_assert(kReadSamples * kWakeSampleRate / kAudioRate + kResamplerPadSamples <= kWakeReadSamples,
              "Capture block resamples beyond kWakeReadSamples");
constexpr size_t kDmaBlocks = 6;
constexpr int kMicGain = CONFIG_WAKE_MIC_GAIN;
constexpr int kSpeakerVolume = CONFIG_WAKE_SPEAKER_VOLUME;

esp_codec_dev_handle_t mic;
esp_codec_dev_handle_t speaker;
esp_ae_rate_cvt_handle_t resampler;

esp_err_t InitI2s(i2s_chan_handle_t &tx, i2s_chan_handle_t &rx) {
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channels.dma_desc_num = kDmaBlocks;
    channels.dma_frame_num = kReadSamples;
    channels.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channels, &tx, &rx), kTag, "I2S channels");

    i2s_std_config_t output = {};
    output.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kAudioRate);
    output.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    output.gpio_cfg = { .mclk = kMclkPin, .bclk = kBclkPin, .ws = kWsPin,
                        .dout = kDoutPin, .din = GPIO_NUM_NC, .invert_flags = {} };

    i2s_tdm_config_t input = {};
    input.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kAudioRate);
    input.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
        static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    input.gpio_cfg = { .mclk = kMclkPin, .bclk = kBclkPin, .ws = kWsPin,
                       .dout = GPIO_NUM_NC, .din = kDinPin, .invert_flags = {} };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &output), kTag, "I2S speaker");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &input), kTag, "I2S microphone");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), kTag, "Enable speaker clock");
    return i2s_channel_enable(rx);
}
}  // namespace

esp_err_t InitBoardAudio() {
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    ESP_RETURN_ON_ERROR(InitI2s(tx, rx), kTag, "Initialize I2S");

    i2c_master_bus_config_t i2c = {};
    i2c.i2c_port = I2C_NUM_1;
    i2c.sda_io_num = kSdaPin;
    i2c.scl_io_num = kSclPin;
    i2c.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c.glitch_ignore_cnt = 7;
    i2c.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &bus), kTag, "Initialize codec I2C");

    audio_codec_i2s_cfg_t i2s = {};
    i2s.port = I2S_NUM_0;
    i2s.rx_handle = rx;
    i2s.tx_handle = tx;
    const auto *data = audio_codec_new_i2s_data(&i2s);
    const auto *gpio = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t control = {};
    control.port = I2C_NUM_1;
    control.bus_handle = bus;
    control.addr = ES8311_CODEC_DEFAULT_ADDR;
    const auto *dac_control = audio_codec_new_i2c_ctrl(&control);
    control.addr = ES7210_CODEC_DEFAULT_ADDR;
    const auto *adc_control = audio_codec_new_i2c_ctrl(&control);
    ESP_RETURN_ON_FALSE(data && gpio && dac_control && adc_control, ESP_ERR_NO_MEM, kTag, "Codec interfaces");

    es8311_codec_cfg_t dac = {};
    dac.ctrl_if = dac_control;
    dac.gpio_if = gpio;
    dac.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    dac.pa_pin = kPaPin;
    dac.use_mclk = true;
    dac.hw_gain.pa_voltage = 5.0f;
    dac.hw_gain.codec_dac_voltage = 3.3f;
    es7210_codec_cfg_t adc = {};
    adc.ctrl_if = adc_control;
    adc.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;

    esp_codec_dev_cfg_t output = { ESP_CODEC_DEV_TYPE_OUT, es8311_codec_new(&dac), data };
    esp_codec_dev_cfg_t input = { ESP_CODEC_DEV_TYPE_IN, es7210_codec_new(&adc), data };
    ESP_RETURN_ON_FALSE(output.codec_if && input.codec_if, ESP_FAIL, kTag, "Create codecs");
    speaker = esp_codec_dev_new(&output);
    mic = esp_codec_dev_new(&input);
    ESP_RETURN_ON_FALSE(speaker && mic, ESP_ERR_NO_MEM, kTag, "Create audio devices");

    esp_codec_dev_sample_info_t mic_format = {};
    mic_format.sample_rate = kAudioRate;
    mic_format.bits_per_sample = 16;
    mic_format.channel = 4;
    mic_format.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    esp_codec_dev_sample_info_t speaker_format = {};
    speaker_format.sample_rate = kAudioRate;
    speaker_format.bits_per_sample = 16;
    speaker_format.channel = 1;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(speaker, &speaker_format), kTag, "Open speaker");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(speaker, kSpeakerVolume), kTag, "Speaker volume");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(mic, &mic_format), kTag, "Open microphone");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_channel_gain(mic, mic_format.channel_mask, kMicGain), kTag, "Mic gain");

    esp_ae_rate_cvt_cfg_t rate = {};
    rate.src_rate = kAudioRate;
    rate.dest_rate = kWakeSampleRate;
    rate.channel = 1;
    rate.bits_per_sample = 16;
    rate.complexity = 2;
    rate.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    ESP_RETURN_ON_FALSE(esp_ae_rate_cvt_open(&rate, &resampler) == ESP_AE_ERR_OK,
                        ESP_FAIL, kTag, "Open microphone resampler");
    uint32_t required_samples = 0;
    ESP_RETURN_ON_FALSE(esp_ae_rate_cvt_get_max_out_sample_num(resampler, kReadSamples, &required_samples) ==
                            ESP_AE_ERR_OK && required_samples <= kWakeReadSamples,
                        ESP_ERR_INVALID_SIZE, kTag, "Resampler output buffer");
    return ESP_OK;
}

esp_err_t ReadWakePcm(std::span<int16_t, kWakeReadSamples> pcm, size_t &samples) {
    std::array<int16_t, kReadSamples> input;
    samples = 0;
    ESP_RETURN_ON_ERROR(esp_codec_dev_read(mic, input.data(), sizeof(input)), kTag, "Read microphone");
    uint32_t output_samples = pcm.size();
    ESP_RETURN_ON_FALSE(esp_ae_rate_cvt_process(resampler, input.data(), input.size(), pcm.data(),
                                              &output_samples) == ESP_AE_ERR_OK,
                        ESP_FAIL, kTag, "Resample microphone");
    samples = output_samples;
    return ESP_OK;
}

esp_err_t PlayWakePrompt(std::span<const uint8_t> pcm, uint32_t sample_rate) {
    if (pcm.size() % sizeof(int16_t) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const int16_t *input = reinterpret_cast<const int16_t *>(pcm.data());
    size_t input_samples = pcm.size() / sizeof(int16_t);

    std::vector<int16_t> converted;
    if (sample_rate != kAudioRate) {
        ESP_RETURN_ON_ERROR(ResamplePcm16Mono({input, input_samples}, sample_rate, kAudioRate, converted),
                            kTag, "Resample prompt");
        input = converted.data();
        input_samples = converted.size();
    }

    std::array<int16_t, kReadSamples> block;
    esp_err_t playback = ESP_OK;
    size_t offset = 0;
    while (offset < input_samples && playback == ESP_OK) {
        const size_t chunk = std::min(input_samples - offset, block.size());
        std::memcpy(block.data(), input + offset, chunk * sizeof(int16_t));
        playback = esp_codec_dev_write(speaker, block.data(), chunk * sizeof(int16_t));
        offset += chunk;
    }

    // Flush the TX DMA queue and discard capture containing the prompt or its tail.
    block.fill(0);
    for (size_t i = 0; i < kDmaBlocks + 10 && playback == ESP_OK; ++i) {
        playback = esp_codec_dev_write(speaker, block.data(), sizeof(block));
    }
    for (size_t i = 0; i < kDmaBlocks; ++i) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_read(mic, block.data(), sizeof(block)), kTag, "Drain microphone");
    }
    ESP_RETURN_ON_FALSE(esp_ae_rate_cvt_reset(resampler) == ESP_AE_ERR_OK, ESP_FAIL, kTag, "Reset resampler");
    return playback;
}
