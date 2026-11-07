#include "board_audio.h"
#include "board_display.h"
#include "resample_util.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_ae_rate_cvt.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_ops.h"
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

// LCD on SPI3: ILI9341, 320x240 landscape, backlight on a plain GPIO.
constexpr char kDisplayTag[] = "box3_display";
constexpr gpio_num_t kLcdMosi = GPIO_NUM_6;
constexpr gpio_num_t kLcdPclk = GPIO_NUM_7;
constexpr gpio_num_t kLcdCs = GPIO_NUM_5;
constexpr gpio_num_t kLcdDc = GPIO_NUM_4;
constexpr gpio_num_t kLcdRst = GPIO_NUM_48;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_47;
constexpr int kLcdWidth = 320;
constexpr int kLcdDrawLines = 40;
constexpr int kLcdClockHz = 40 * 1000 * 1000;
// LEDC PWM backlight: full brightness washes the black level out to blue-grey on this panel.
constexpr int kBacklightPercent = 60;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_1;

// Panel specific power/gamma sequence copied from the ESP-BOX-3 BSP.
constexpr uint8_t kCmdPower1[] = {0xFF, 0x93, 0x42};
constexpr uint8_t kCmdPower2[] = {0x0E, 0x0E};
constexpr uint8_t kCmdVcom[] = {0xD0};
constexpr uint8_t kCmdPower3[] = {0x02};
constexpr uint8_t kCmdGain[] = {0x02};
constexpr uint8_t kCmdGammaPos[] = {0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39,
                                    0x48, 0x02, 0x0A, 0x08, 0x17, 0x17, 0x0F};
constexpr uint8_t kCmdGammaNeg[] = {0x00, 0x28, 0x29, 0x01, 0x0D, 0x03, 0x3F, 0x33,
                                    0x52, 0x04, 0x0F, 0x0E, 0x37, 0x38, 0x0F};
constexpr uint8_t kCmdFrameRate[] = {0x00, 0x1B};
constexpr uint8_t kCmdMadctl[] = {0x08};
constexpr uint8_t kCmdPixelFormat[] = {0x55};
constexpr uint8_t kCmdGate[] = {0x06};

constexpr ili9341_lcd_init_cmd_t kLcdInit[] = {
    {0xC8, kCmdPower1, std::size(kCmdPower1), 0},
    {0xC0, kCmdPower2, std::size(kCmdPower2), 0},
    {0xC5, kCmdVcom, std::size(kCmdVcom), 0},
    {0xC1, kCmdPower3, std::size(kCmdPower3), 0},
    {0xB4, kCmdGain, std::size(kCmdGain), 0},
    {0xE0, kCmdGammaPos, std::size(kCmdGammaPos), 0},
    {0xE1, kCmdGammaNeg, std::size(kCmdGammaNeg), 0},
    {0xB1, kCmdFrameRate, std::size(kCmdFrameRate), 0},
    {0x36, kCmdMadctl, std::size(kCmdMadctl), 0},
    {0x3A, kCmdPixelFormat, std::size(kCmdPixelFormat), 0},
    {0xB7, kCmdGate, std::size(kCmdGate), 0},
    {0x11, nullptr, 0, 0x80},
    {0x29, nullptr, 0, 0x80},
    {0x00, nullptr, 0xFF, 0},
};

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

esp_err_t InitBoardDisplay(esp_lcd_panel_io_handle_t &io, esp_lcd_panel_handle_t &panel) {
    ledc_timer_config_t backlight_timer = {};
    backlight_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    backlight_timer.duty_resolution = LEDC_TIMER_10_BIT;
    backlight_timer.timer_num = kBacklightTimer;
    backlight_timer.freq_hz = 5000;
    backlight_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), kDisplayTag, "Backlight timer");

    ledc_channel_config_t backlight = {};
    backlight.gpio_num = kLcdBacklight;
    backlight.speed_mode = LEDC_LOW_SPEED_MODE;
    backlight.channel = kBacklightChannel;
    backlight.intr_type = LEDC_INTR_DISABLE;
    backlight.timer_sel = kBacklightTimer;
    backlight.duty = 0;
    backlight.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight), kDisplayTag, "Backlight channel");

    spi_bus_config_t bus = {};
    bus.mosi_io_num = kLcdMosi;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = kLcdPclk;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = kLcdWidth * kLcdDrawLines * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), kDisplayTag, "LCD SPI bus");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = kLcdCs;
    io_config.dc_gpio_num = kLcdDc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kLcdClockHz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI3_HOST), &io_config, &io),
                        kDisplayTag, "LCD panel IO");

    constexpr ili9341_vendor_config_t vendor = {
        .init_cmds = kLcdInit,
        .init_cmds_size = static_cast<uint16_t>(std::size(kLcdInit)),
    };
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = kLcdRst;
    panel_config.vendor_config = const_cast<ili9341_vendor_config_t *>(&vendor);
    panel_config.flags.reset_active_high = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_config, &panel), kDisplayTag, "ILI9341 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), kDisplayTag, "Panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), kDisplayTag, "Panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, true, true), kDisplayTag, "Panel mirror");
    return esp_lcd_panel_disp_on_off(panel, true);
}

void BoardDisplayBacklight(bool on) {
    const uint32_t duty = on ? static_cast<uint32_t>((1023 * kBacklightPercent) / 100) : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel);
}
