// Lost Vikings embedding of Nuked-SC55: the module's LCD without a window.
// The MCU writes the display through LCD_Write / LCD_Enable (mcu.cpp); the
// standalone's lcd.cpp renders it with SDL — here nothing is shown and the
// writes are accepted and dropped. Same license as the core (GPL-2.0-or-later).
#include "lcd.h"
#include <atomic>
// Boot signal for the host (v2_sc55.cpp): the firmware switches the display on
// once its start-up is done — measured with the boot probe (scratchpad
// boottest, 2026-09-10): SC-55mk2 v1.01 enables it 0.08 s after reset and
// honours notes from the first one; SC-55 v1.21 enables it at 2.14 s and
// honours notes from 2.0 s. Notes sent before are not sounded.
static std::atomic<int> g_lcd_on{0};
extern "C" int  nsc55_lcd_enabled(void) { return g_lcd_on.load(std::memory_order_acquire); }
extern "C" void nsc55_lcd_reset(void)   { g_lcd_on.store(0, std::memory_order_release); }
int lcd_width = 741;
int lcd_height = 268;
uint32_t lcd_col1 = 0x000000;
uint32_t lcd_col2 = 0x0050c8;
void LCD_SetBackPath(const std::string&) {}
void LCD_Init(void) {}
void LCD_UnInit(void) {}
void LCD_Write(uint32_t, uint8_t) {}
void LCD_Enable(uint32_t enable) { if (enable) g_lcd_on.store(1, std::memory_order_release); }
bool LCD_QuitRequested() { return false; }
void LCD_Sync(void) {}
void LCD_Update(void) {}
