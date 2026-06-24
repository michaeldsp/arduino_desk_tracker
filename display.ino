#include "Arduino.h"
#include "DFRobot_UI.h"
#include "DFRobot_GDL.h"
#include "DFRobot_Touch.h"
#include "Wire.h"
#include <time.h>

// Good sources here!!!!
// https://github.com/DFRobot/DFRobot_GDL/blob/master/examples/Touch_ST7365P_320x480/rotating/rotating.ino
// https://github.com/DFRobot/DFRobot_GDL/wiki/English-WIKI

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

// --- Colours used by the dashboard ---
#define COL_BG     COLOR_RGB565_BLACK
#define COL_TEXT   COLOR_RGB565_WHITE
#define COL_DIM    COLOR_RGB565_LGRAY
#define COL_LABEL  COLOR_RGB565_CYAN
#define COL_OK     COLOR_RGB565_GREEN
#define COL_WARN   COLOR_RGB565_ORANGE
#define COL_ALERT  COLOR_RGB565_RED
#define COL_BREAK  COLOR_RGB565_YELLOW

// Screen geometry (filled in by display_init once the rotation is set)
static int16_t SW = 480;
static int16_t SH = 320;

// Backlight level (0-255) for right now, based on the time of day
static uint8_t currentBacklight() {
  uint8_t pct = BRIGHTNESS_DAY;          // assume daytime if the clock isn't set yet
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_hour < DAY_START_HOUR || t.tm_hour >= DAY_END_HOUR)
      pct = BRIGHTNESS_AFTERHOURS;
  }
  return (uint8_t)((uint16_t)pct * 255 / 100);
}

// Switch screen off
void screenOff() {
  analogWrite(TFT_BL, 0);
}

// Switch screen on (at the brightness appropriate for the time of day)
void screenOn() {
  analogWrite(TFT_BL, currentBacklight());
}

// ---- small drawing helpers -----------------------------------------------

// Format seconds as HH:MM (no seconds, so the session timer only moves once a minute)
static void fmtHHMM(unsigned long s, char *buf) {
  unsigned long h = s / 3600;
  unsigned long m = (s % 3600) / 60;
  sprintf(buf, "%02lu:%02lu", h, m);
}

// Format seconds as a friendly "2h 12m" / "23m" duration
static void fmtHM(unsigned long s, char *buf) {
  unsigned long h = s / 3600;
  unsigned long m = (s % 3600) / 60;
  if (h > 0) sprintf(buf, "%luh %lum", h, m);
  else       sprintf(buf, "%lum", m);
}

// Draw classic-font text at a given size/colour (background left untouched)
static void drawText(int16_t x, int16_t y, uint8_t size, uint16_t color, const char *s) {
  screen.setFont();          // revert to the built-in scalable 6x8 font
  screen.setTextSize(size);
  screen.setTextColor(color);
  screen.setCursor(x, y);
  screen.print(s);
}

// Draw text horizontally centred on the screen
static void drawCentered(int16_t y, uint8_t size, uint16_t color, const char *s) {
  int16_t w = (int16_t)strlen(s) * 6 * size;
  drawText((SW - w) / 2, y, size, color, s);
}

// Clear a rectangle back to the background colour
static void clearRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  screen.fillRect(x, y, w, h, COL_BG);
}

// Draw a horizontal progress bar (frac 0.0 - 1.0)
static void drawBar(int16_t x, int16_t y, int16_t w, int16_t h, float frac, uint16_t fg) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  screen.drawRect(x, y, w, h, COL_DIM);
  int16_t innerW = w - 4;
  int16_t fill = (int16_t)(innerW * frac);
  if (fill > 0)        screen.fillRect(x + 2, y + 2, fill, h - 4, fg);
  if (fill < innerW)   screen.fillRect(x + 2 + fill, y + 2, innerW - fill, h - 4, COL_BG);
}

// Small amber "searching" eye, shown when the sensor has briefly lost me.
// open=false draws a closed eye so it blinks when toggled each second.
static void drawEye(int16_t cx, int16_t cy, bool open) {
  screen.fillRect(cx - 20, cy - 16, 40, 32, COL_BG);   // clear its box
  uint16_t c = COL_WARN;
  if (open) {
    // almond lids
    screen.drawLine(cx - 16, cy, cx, cy - 9, c);
    screen.drawLine(cx, cy - 9, cx + 16, cy, c);
    screen.drawLine(cx - 16, cy, cx, cy + 9, c);
    screen.drawLine(cx, cy + 9, cx + 16, cy, c);
    screen.drawCircle(cx, cy, 6, c);     // iris
    screen.fillCircle(cx, cy, 3, c);     // pupil
  } else {
    screen.drawLine(cx - 16, cy, cx + 16, cy, c);   // closed
  }
}

// Colour for a single timeline minute-state (0..4)
static uint16_t timelineColor(uint8_t st) {
  switch (st) {
    case 1: return COLOR_RGB565_GREEN;    // present, working hours
    case 2: return COLOR_RGB565_DGREEN;   // present, off hours
    case 3: return COLOR_RGB565_RED;      // away, working hours
    case 4: return COLOR_RGB565_MAROON;   // away, off hours
    default: return COLOR_RGB565_DGRAY;   // not reached yet
  }
}

// "Severity" so a pixel covering several minutes never hides an away-in-work blip
static uint8_t timelineRank(uint8_t st) {
  switch (st) {
    case 3: return 4;   // away/work  (most important to show)
    case 1: return 3;   // present/work
    case 4: return 2;   // away/off
    case 2: return 1;   // present/off
    default: return 0;  // unknown
  }
}

// Draw the day timeline with 08:00 / 17:00 markers. The window starts at the
// first minute I was present today and runs to midnight, so the active part of
// the day fills the whole bar (rather than wasting space on the small hours).
static void drawTimeline(int16_t x, int16_t y, int16_t w, int16_t h) {
  screen.drawRect(x, y, w, h, COL_DIM);
  int16_t ix = x + 1, iy = y + 1, iw = w - 2, ih = h - 2;

  // First minute I was present today defines the left edge of the window
  int startMin = 0;
  for (int m = 0; m < 1440; m++) {
    if (dayTimeline[m] == 1 || dayTimeline[m] == 2) { startMin = m; break; }
  }
  int winLen = 1440 - startMin;
  if (winLen < 1) winLen = 1;

  for (int px = 0; px < iw; px++) {
    // minutes this pixel column represents within the window
    int mStart = startMin + (int)((long)px * winLen / iw);
    int mEnd   = startMin + (int)((long)(px + 1) * winLen / iw);
    if (mEnd <= mStart) mEnd = mStart + 1;
    if (mEnd > 1440) mEnd = 1440;

    uint8_t best = 0;
    for (int m = mStart; m < mEnd; m++) {
      if (timelineRank(dayTimeline[m]) > timelineRank(best)) best = dayTimeline[m];
    }
    screen.drawFastVLine(ix + px, iy, ih, timelineColor(best));
  }

  // Working-hours markers, remapped into the window (skipped if before it)
  int markMins[2] = { DAY_START_HOUR * 60, DAY_END_HOUR * 60 };
  for (int i = 0; i < 2; i++) {
    if (markMins[i] < startMin || markMins[i] > 1440) continue;
    int16_t mx = ix + (int16_t)((long)(markMins[i] - startMin) * iw / winLen);
    screen.drawFastVLine(mx, y - 2, h + 4, COLOR_RGB565_WHITE);
  }
}

// ---- layout (computed from SW/SH so it adapts to the rotation) -----------

#define SESS_TIME_Y   70
#define SESS_BAR_Y    124
#define DESK_LBL_Y    168
#define DESK_BAR_Y    194
#define FOOT_LBL_Y    240
#define FOOT_VAL_Y    264

// Cached values so each field only repaints when it actually changes
// (no per-second repaint = no flicker now the timer shows only HH:MM)
static char pClock[8]  = "";
static char pSess[8]   = "";
static char pDesk[16]  = "";
static char pBreak[16] = "";
static char pAway[16]  = "";
static int  pWorkPct   = -1;
static int  pSessPct   = -1;
static int  pEye       = -1;     // 0/1: searching-eye drawn last refresh
static int  pTimelineMin = -2;   // last minute index drawn into the 24h view
static int  pAlert     = -1;     // -1 unknown, 0 off, 1 on
static bool dashFresh  = true;   // force a full repaint after a static redraw

void display_init() {
  ui.begin();
  Serial.println("ui.begin() done");

  pinMode(TFT_BL, OUTPUT);

  touch.setRotation(-1);
  screen.setRotation(-1);

  SW = screen.width();
  SH = screen.height();

  screen.setFont();          // built-in font for the dashboard
  screen.fillScreen(COL_BG);
  screenOff();               // stay dark until presence is detected
}

// Full "welcome back" splash shown when the user returns to the desk
void display_welcome_back(unsigned long awaySec) {
  char buf[24];
  screen.fillScreen(COL_BG);
  drawCentered(SH / 2 - 70, 4, COL_LABEL, "WELCOME BACK");
  drawCentered(SH / 2 - 10, 2, COL_TEXT,  "You were away for");
  fmtHM(awaySec, buf);
  drawCentered(SH / 2 + 25, 4, COL_BREAK, buf);
}

// Draw the parts of the dashboard that never change (labels, divider)
void display_dashboard_static() {
  screen.fillScreen(COL_BG);
  dashFresh = true;            // force the next update() to repaint every field

  // Header
  drawText(10, 10, 2, COL_TEXT, "HEALTHY DESK");
  screen.fillRect(0, 36, SW, 2, COL_DIM);

  // Fixed labels for the lower blocks (footer is three even columns)
  drawText(10, DESK_LBL_Y, 2, COL_LABEL, "TODAY AT DESK");
  drawText(10,             FOOT_LBL_Y, 2, COL_LABEL, "LAST BREAK");
  drawText(SW / 3 + 5,     FOOT_LBL_Y, 2, COL_LABEL, "TODAY AWAY");
  drawText(2 * SW / 3 + 5, FOOT_LBL_Y, 2, COL_LABEL, "% AWAY");
}

// Redraw everything that changes. Reads the ui* globals owned by the main sketch.
void display_dashboard_update() {
  char buf[24];
  bool force = dashFresh;
  dashFresh = false;

  // Track the time-of-day backlight level while the screen is on
  analogWrite(TFT_BL, currentBacklight());

  bool blink = (millis() / 1000) % 2;    // 1Hz toggle for the alert flash
  int  alert = uiBreakAlert ? 1 : 0;

  // --- header clock (HH:MM, only moves once a minute) ---
  if (force || strcmp(pClock, uiClock) != 0) {
    clearRect(SW - 80, 10, 80, 18);
    drawText(SW - 70, 10, 2, COL_TEXT, uiClock);
    strcpy(pClock, uiClock);
  }

  // --- session label line (blinks only while the break alert is active) ---
  if (alert) {
    clearRect(0, 50, SW, 22);
    if (blink) drawText(10, 52, 2, COL_ALERT, "TAKE A BREAK!");
  } else if (force || pAlert != 0) {
    clearRect(0, 50, SW, 22);
    drawText(10, 52, 2, COL_LABEL, "THIS SESSION");
  }

  // --- session timer HH:MM (only repaints when the minute changes) ---
  fmtHHMM(uiSessionSec, buf);
  if (force || strcmp(pSess, buf) != 0 || pAlert != alert) {
    uint16_t sessCol = alert ? COL_ALERT : COL_TEXT;
    clearRect(0, SESS_TIME_Y, SW, 40);
    drawCentered(SESS_TIME_Y, 5, sessCol, buf);
    strcpy(pSess, buf);
  }

  // --- session bar: fills toward the break threshold ---
  int sessPct = (int)(100.0f * uiSessionSec / (BREAK_THRESHOLD_MIN * 60UL));
  if (sessPct > 100) sessPct = 100;
  if (force || sessPct != pSessPct || pAlert != alert) {
    uint16_t barCol = alert ? COL_ALERT : (sessPct > 80 ? COL_WARN : COL_OK);
    drawBar(20, SESS_BAR_Y, SW - 40, 26, sessPct / 100.0f, barCol);
    pSessPct = sessPct;
  }

  // --- "searching" eye to the right of the timer while the sensor lost me ---
  int eye = uiSearching ? 1 : 0;
  if (eye) {
    drawEye(SW - 55, SESS_TIME_Y + 20, blink);    // blinks open/closed each second
  } else if (force || pEye != 0) {
    screen.fillRect(SW - 75, SESS_TIME_Y + 4, 40, 32, COL_BG);   // clear when gone
  }
  pEye = eye;

  // --- today at desk (value + bar) ---
  fmtHM(uiTodayDeskSec, buf);
  if (force || strcmp(pDesk, buf) != 0) {
    clearRect(SW / 2, DESK_LBL_Y, SW / 2, 18);
    int16_t vw = (int16_t)strlen(buf) * 6 * 2;
    drawText(SW - 10 - vw, DESK_LBL_Y, 2, COL_TEXT, buf);
    strcpy(pDesk, buf);
  }
  // 24h timeline replaces the old progress bar; redraws once a minute
  if (force || uiDayMinute != pTimelineMin) {
    drawTimeline(20, DESK_BAR_Y, SW - 40, 22);
    pTimelineMin = uiDayMinute;
  }

  // --- footer columns: last break / today away / % away (working hours) ---
  int16_t cw = SW / 3;          // column width

  fmtHM(uiLastBreakSec, buf);
  if (force || strcmp(pBreak, buf) != 0) {
    clearRect(10, FOOT_VAL_Y, cw - 12, 24);
    drawText(10, FOOT_VAL_Y, 3, COL_BREAK, buf);
    strcpy(pBreak, buf);
  }
  fmtHM(uiTodayAwaySec, buf);
  if (force || strcmp(pAway, buf) != 0) {
    clearRect(cw + 5, FOOT_VAL_Y, cw - 7, 24);
    drawText(cw + 5, FOOT_VAL_Y, 3, COL_WARN, buf);
    strcpy(pAway, buf);
  }
  if (force || uiWorkAwayPct != pWorkPct) {
    sprintf(buf, "%d%%", uiWorkAwayPct);
    // green when low, orange past 20%, red past 40%
    uint16_t pctCol = uiWorkAwayPct > 40 ? COL_ALERT
                    : (uiWorkAwayPct > 20 ? COL_WARN : COL_OK);
    clearRect(2 * cw + 5, FOOT_VAL_Y, cw - 7, 24);
    drawText(2 * cw + 5, FOOT_VAL_Y, 3, pctCol, buf);
    pWorkPct = uiWorkAwayPct;
  }

  // --- flashing border while the break alert is active ---
  if (alert) {
    uint16_t border = blink ? COL_ALERT : COL_BG;
    screen.drawRect(0, 0, SW, SH, border);
    screen.drawRect(1, 1, SW - 2, SH - 2, border);
    screen.drawRect(2, 2, SW - 4, SH - 4, border);
  } else if (force || pAlert != 0) {
    screen.drawRect(0, 0, SW, SH, COL_BG);     // clear leftover border
    screen.drawRect(1, 1, SW - 2, SH - 2, COL_BG);
    screen.drawRect(2, 2, SW - 4, SH - 4, COL_BG);
  }

  pAlert = alert;
}
