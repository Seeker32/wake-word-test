#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

// Panel bus, driver and backlight for the board LCD. main/display.cpp owns everything above the
// panel handle; the backlight stays dark until the first frame is ready.
esp_err_t InitBoardDisplay(esp_lcd_panel_io_handle_t &io, esp_lcd_panel_handle_t &panel);
void BoardDisplayBacklight(bool on);
