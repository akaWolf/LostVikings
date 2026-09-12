// Lost Vikings embedding of Nuked-SC55: the module's LCD without a window.
// The MCU writes the display through LCD_Write / LCD_Enable (mcu.cpp); the
// standalone's lcd.cpp renders it with SDL — here nothing is shown and the
// writes are accepted and dropped. Same license as the core (GPL-2.0-or-later).
#include "lcd.h"
int lcd_width = 741;
int lcd_height = 268;
uint32_t lcd_col1 = 0x000000;
uint32_t lcd_col2 = 0x0050c8;
void LCD_SetBackPath(const std::string&) {}
void LCD_Init(void) {}
void LCD_UnInit(void) {}
void LCD_Write(uint32_t, uint8_t) {}
void LCD_Enable(uint32_t) {}
bool LCD_QuitRequested() { return false; }
void LCD_Sync(void) {}
void LCD_Update(void) {}
