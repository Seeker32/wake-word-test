#include "display.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "board_display.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

namespace {

constexpr char kTag[] = "display";

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kDrawLines = 40;

constexpr int kTickMs = 100;
// Generous for one 25.6 kB chunk at 40 MHz; a stall here must not freeze the device.
constexpr TickType_t kFlushTimeout = pdMS_TO_TICKS(200);

// Everything the UI task renders, written by the audio loop under s_lock.
struct Dashboard {
    float score;
    int cpu0_pct;
    int cpu1_pct;
    size_t heap_free;
    size_t heap_min;
    size_t heap_largest;
};

Dashboard s_data;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

bool s_ready = false;
SemaphoreHandle_t s_flush_done = nullptr;
lv_obj_t *s_cpu = nullptr;
lv_obj_t *s_heap = nullptr;
lv_obj_t *s_cutoff = nullptr;
char s_cpu_text[32];
char s_heap_text[32];
char s_cutoff_text[32];
bool s_cutoff_hot = false;

void Paint(lv_obj_t *label, char *cache, size_t size, const char *text) {
    if (std::strncmp(cache, text, size) == 0) {
        return;
    }
    std::snprintf(cache, size, "%s", text);
    lv_label_set_text(label, cache);
}

// LVGL busy-waits for the panel transfer when no wait callback is set, which starves the idle
// task and everything else on that core. Block on the SPI completion instead.
bool OnColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
    lvgl_port_flush_ready(static_cast<lv_display_t *>(ctx));
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

void WaitForFlush(lv_display_t *) {
    if (xSemaphoreTake(s_flush_done, kFlushTimeout) != pdTRUE) {
        ESP_LOGW(kTag, "Panel flush timed out, frame may tear");
    }
}

void Tick(lv_timer_t *) {
    Dashboard data;
    portENTER_CRITICAL(&s_lock);
    data = s_data;
    portEXIT_CRITICAL(&s_lock);

    char text[32];
    std::snprintf(text, sizeof(text), "CPU %d%%/%d%%", data.cpu0_pct, data.cpu1_pct);
    Paint(s_cpu, s_cpu_text, sizeof(s_cpu_text), text);

    std::snprintf(text, sizeof(text), "heap %zu/%zu/%zu KB", data.heap_free / 1024, data.heap_min / 1024,
                  data.heap_largest / 1024);
    Paint(s_heap, s_heap_text, sizeof(s_heap_text), text);

    // Repainted on the first tick too, so a torn boot frame cannot leave the line broken.
    std::snprintf(text, sizeof(text), "cutoff %.3f | score %.3f", static_cast<double>(WAKE_PROBABILITY_CUTOFF),
                  static_cast<double>(data.score));
    Paint(s_cutoff, s_cutoff_text, sizeof(s_cutoff_text), text);

    // Green once the live score reaches the cutoff, so a wake is visible at a glance.
    const bool hot = data.score >= WAKE_PROBABILITY_CUTOFF;
    if (hot != s_cutoff_hot) {
        s_cutoff_hot = hot;
        lv_obj_set_style_text_color(s_cutoff, hot ? lv_color_hex(0x00FF80) : lv_color_hex(0x00E0FF), 0);
    }
}

void BuildUi() {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    s_cpu = lv_label_create(screen);
    lv_obj_set_pos(s_cpu, 8, 8);
    lv_obj_set_style_text_font(s_cpu, &lv_font_montserrat_16, 0);

    s_heap = lv_label_create(screen);
    lv_obj_set_pos(s_heap, 8, 44);
    lv_obj_set_style_text_font(s_heap, &lv_font_montserrat_16, 0);

    s_cutoff = lv_label_create(screen);
    lv_obj_set_pos(s_cutoff, 8, 80);
    lv_obj_set_style_text_font(s_cutoff, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cutoff, lv_color_hex(0x00E0FF), 0);
}

}  // namespace

esp_err_t InitDisplay() {
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    ESP_RETURN_ON_ERROR(InitBoardDisplay(io, panel), kTag, "Board display");

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 3;
    // Keep rendering off the core that runs the wake word loop; CPU1 has nothing else to do.
    port.task_affinity = 1;
    port.timer_period_ms = 20;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), kTag, "LVGL port");

    // A DMA capable internal buffer avoids a copy per flush; two PSRAM buffers cover a tight heap.
    lv_display_t *disp = nullptr;
    for (const bool psram : {false, true}) {
        lvgl_port_display_cfg_t cfg = {};
        cfg.io_handle = io;
        cfg.panel_handle = panel;
        cfg.buffer_size = kWidth * kDrawLines;
        cfg.double_buffer = psram;
        cfg.hres = kWidth;
        cfg.vres = kHeight;
        cfg.rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = true};
        cfg.color_format = LV_COLOR_FORMAT_RGB565;
        cfg.flags.buff_dma = !psram;
        cfg.flags.buff_spiram = psram;
        cfg.flags.swap_bytes = true;
        disp = lvgl_port_add_disp(&cfg);
        if (disp) {
            ESP_LOGI(kTag, "LCD attached, draw buffers in %s", psram ? "PSRAM" : "internal RAM");
            break;
        }
    }
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_NO_MEM, kTag, "LVGL display");

    // Replace the port's bare flush-ready callback so the LVGL task can block, not spin.
    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, kTag, "Flush semaphore");
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = OnColorTransDone};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, disp), kTag,
                        "Panel IO callbacks");
    lv_display_set_flush_wait_cb(disp, WaitForFlush);

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    BuildUi();
    // Paint one full frame before the backlight comes on, so no area keeps its power-on GRAM.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    lv_timer_create(Tick, kTickMs, nullptr);
    lvgl_port_unlock();

    BoardDisplayBacklight(true);
    s_ready = true;
    return ESP_OK;
}

void DisplayScore(float score) {
    if (!s_ready) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_data.score = score;
    portEXIT_CRITICAL(&s_lock);
}

void DisplayResources(int cpu0_pct, int cpu1_pct, size_t heap_free, size_t heap_min, size_t heap_largest) {
    if (!s_ready) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_data.cpu0_pct = cpu0_pct;
    s_data.cpu1_pct = cpu1_pct;
    s_data.heap_free = heap_free;
    s_data.heap_min = heap_min;
    s_data.heap_largest = heap_largest;
    portEXIT_CRITICAL(&s_lock);
}
