#include "Arduino.h"
#include "DFRobot_UI.h"
#include "DFRobot_GDL.h"
#include "DFRobot_Touch.h"
#include "Wire.h"

// Good sources here!!!!
// https://github.com/DFRobot/DFRobot_GDL/blob/master/examples/Touch_ST7365P_320x480/rotating/rotating.ino


// --- PIN allocation ---
#define TFT_DC    D2    
#define TFT_CS    D6     
#define TFT_RST   D3   
#define TFT_BL    D13  

// CRITICAL FIX: Moved TOUCH_RST to 4 because I2C_SCL is using 10
#define TOUCH_RST 4
#define TOUCH_INT D11
#define I2C_SDA   9
#define I2C_SCL   10

// --- Hardware Object ---
DFRobot_Touch_GT911_IPS touch(0X5D,TOUCH_RST,TOUCH_INT);
DFRobot_ST7365P_320x480_HW_SPI screen(TFT_DC, TFT_CS, TFT_RST);
DFRobot_UI ui(&screen, &touch);

void display_init() {
  ui.begin();
  Serial.println("ui.begin() done");

  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, 128);  // 50% brightness (255 max, 128 half, 0 off)

  touch.setRotation(-1);
  screen.setRotation(-1);

  ui.setTheme(DFRobot_UI::MODERN);
  Serial.println("set theme done");

  /*

    // 1. Hardware Init
    screen.begin();
    screen.setRotation(1); // Landscape

    // 2. Turn on Backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    // 3. Clear Screen to White
    // GDL uses RGB565 colors. 0xFFFF is White.
    screen.fillScreen(0xFFFF); 

    // 4. Draw a Red Square
    // Parameters: x, y, width, height, color
    // 0xF800 is Pure Red in RGB565
    uint16_t red = 0xF800;
    screen.fillRect(190, 110, 100, 100, red);
  */
}

void display_refresh() {
  ui.refresh();
}