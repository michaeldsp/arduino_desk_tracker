#include "DFRobot_C4002.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// Replace with your network credentials
const char* ssid     = "DSPiot";
const char* password = "0828512690iot";
const char* mqtt_server  = "192.168.60.254"; // Your HA/MQTT IP Address
const char* mqtt_user    = "";
const char* mqtt_pass    = "";

const char* discovery_topic = "homeassistant/binary_sensor/c4002_motion/config";
const char* state_topic     = "homeassistant/binary_sensor/c4002_motion/state";
const char* timer_discovery_topic = "homeassistant/sensor/c4002_presence_time/config";
const char* timer_state_topic     = "homeassistant/sensor/c4002_presence_time/state";
const char* timerabsent_discovery_topic     = "homeassistant/sensor/c4002_absent_time/config";
const char* timerabsent_state_topic     = "homeassistant/sensor/c4002_absent_time/state";
// Retained topic used to persist the daily totals across reboots
const char* persist_topic = "homeassistant/desk/persist";
bool calibrate = false;

// ===================== USER-TUNABLE SETTINGS =====================
// Complain and tell me to take a break after this many seated minutes.
const unsigned long BREAK_THRESHOLD_MIN = 120;
// "Today at desk" bar is full at this many minutes (an 8h working day).
const unsigned long DAILY_GOAL_MIN      = 480;
// How long the "Welcome back" splash stays up before the dashboard (ms).
const unsigned long WELCOME_MS          = 5000;
// Local time for the clock / midnight reset. SAST is UTC+2, no DST.
const long GMT_OFFSET_SEC      = 2 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;
// Screen backlight by time of day (percent of full brightness).
// DAY level is used between DAY_START_HOUR and DAY_END_HOUR, AFTERHOURS otherwise.
const uint8_t BRIGHTNESS_DAY        = 60;   // 08:00 - 17:00
const uint8_t BRIGHTNESS_AFTERHOURS = 40;   // outside working hours
const int     DAY_START_HOUR        = 8;    // 08:00
const int     DAY_END_HOUR          = 17;   // 17:00
// How often to save the running totals to the broker (seconds).
const unsigned long PERSIST_INTERVAL_SEC = 30;
// Require this much continuous detection before counting me as "present",
// so just walking past the sensor doesn't trigger a session.
const unsigned long PRESENCE_CONFIRM_SEC = 30;
// ================================================================

// Presence / timing state
bool isPresent = false;
unsigned long sessionStartSec  = 0;   // when the current at-desk session began
unsigned long absenceStartSec  = 0;   // when the current away period began
unsigned long dailyPresenceSec = 0;   // completed at-desk time so far today
unsigned long dailyAbsenceSec  = 0;   // completed away time so far today
unsigned long lastBreakSec     = 0;   // length of the most recent completed break
unsigned long lastSessionSec   = 0;   // length of the most recent completed session
int trackedYDay = -1;                 // day-of-year, used to reset at midnight

// Debounce for arriving: detection must persist before we count it as present
bool          candidate         = false;
unsigned long candidateStartSec = 0;

// Absence that falls inside working hours (08:00-17:00), accounted by wall clock
unsigned long workAbsenceSec   = 0;   // absent seconds within the working window today
time_t        lastAccountEpoch = 0;   // wall-clock time of the last accounting tick

// Values handed to the display layer (display.ino reads these)
unsigned long uiSessionSec   = 0;
unsigned long uiTodayDeskSec = 0;
unsigned long uiTodayAwaySec = 0;
unsigned long uiLastBreakSec = 0;
int           uiWorkAwayPct  = 0;     // % of working hours so far spent away
bool          uiBreakAlert   = false;
char          uiClock[8]     = "--:--";

// Display state machine
enum DisplayMode { MODE_OFF, MODE_WELCOME, MODE_DASHBOARD };
DisplayMode displayMode = MODE_OFF;
unsigned long welcomeUntilMs = 0;
unsigned long lastDrawSec    = 0;

// Persistence (restore-from-broker) state
unsigned long lastPersistSec = 0;
bool restoreDone   = false;   // true once the boot-time restore window has closed
bool persistLoaded = false;   // true if a retained snapshot was found and applied

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastha = 0;

// Pull an integer value for "key" out of a small JSON object string
static bool parseJsonLong(const char* json, const char* key, long* out) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  *out = atol(p + 1);
  return true;
}

// MQTT message handler: restores the retained daily totals at boot (once)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (restoreDone) return;                          // ignore our own later publishes
  if (strcmp(topic, persist_topic) != 0) return;

  char buf[192];
  unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';

  long day = -1, desk = 0, away = 0, workaway = 0, lastbreak = 0;
  parseJsonLong(buf, "day", &day);
  parseJsonLong(buf, "desk", &desk);
  parseJsonLong(buf, "away", &away);
  parseJsonLong(buf, "workaway", &workaway);
  parseJsonLong(buf, "lastbreak", &lastbreak);

  int today = -1;
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm t;
    localtime_r(&now, &t);
    today = t.tm_yday;
  }

  if (today >= 0 && day == today) {
    // Same calendar day: pick up exactly where we left off
    dailyPresenceSec = (unsigned long)desk;
    dailyAbsenceSec  = (unsigned long)away;
    workAbsenceSec   = (unsigned long)workaway;
    trackedYDay = today;
  } else {
    // Snapshot is from another day (or clock unknown): start the day fresh
    dailyPresenceSec = 0;
    dailyAbsenceSec  = 0;
    workAbsenceSec   = 0;
    if (today >= 0) trackedYDay = today;
  }
  lastBreakSec = (unsigned long)lastbreak;
  persistLoaded = true;

  Serial.print("Restored from MQTT: desk=");
  Serial.print(dailyPresenceSec);
  Serial.print("s away=");
  Serial.print(dailyAbsenceSec);
  Serial.print("s lastbreak=");
  Serial.println(lastBreakSec);
}

// Save the running totals to the broker as a retained message
void publishPersist() {
  if (WiFi.status() != WL_CONNECTED || !client.connected()) return;
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"day\":%d,\"desk\":%lu,\"away\":%lu,\"workaway\":%lu,\"lastbreak\":%lu}",
           trackedYDay, uiTodayDeskSec, uiTodayAwaySec, workAbsenceSec, uiLastBreakSec);
  client.publish(persist_topic, buf, true);   // retained
}

// SETUP mqtt discovery
void setup_mqtt() {
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);

  // Home Assistant MQTT Discovery Payload (JSON)
  // This tells HA: "I am a motion sensor named C4002"
  String payload = "{\"name\": \"C4002 mmWave\", \"device_class\": \"motion\", \"state_topic\": \"" + String(state_topic) + "\"}";
  String timerPayload = "{\"name\": \"Presence Duration\", \"stat_t\": \"" + String(timer_state_topic) + "\", \"unit_of_meas\": \"s\", \"dev_cla\": \"duration\"}";
  String timerAbsentPayload = "{\"name\": \"Absent Duration\", \"stat_t\": \"" + String(timerabsent_state_topic) + "\", \"unit_of_meas\": \"s\", \"dev_cla\": \"duration\"}";
  
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32C5_mmWave", mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      // Publish discovery config
      client.publish(discovery_topic, payload.c_str(), true);

      // Payload for the timer (Device Class: duration)
      client.publish(timer_discovery_topic, timerPayload.c_str(), true);
      client.publish(timerabsent_discovery_topic, timerAbsentPayload.c_str(), true);

      // Subscribe so the broker replays the retained totals snapshot
      client.subscribe(persist_topic);
    } else {
      delay(5000);
    }
  }


}

// This function runs automatically whenever a WiFi event occurs
void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("WiFi Connected! IP: ");
            Serial.println(WiFi.localIP());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("WiFi Lost! Attempting to reconnect...");
            WiFi.begin(ssid, password); // Force a retry
            break;
        default: break;
    }
}

// main setup
void setup() {
  Serial.begin(115200);

  Serial.print("display_init()");
  display_init();
  Serial.print("display initalised");

  // This starts the "Configuration Portal" if it can't connect
  // It will stay in the portal until it connects or times out
  Serial.println("\n--- ESP32-C5 WiFi Connection ---");
  Serial.print("Connecting to: ");
  Serial.println(ssid);

  // Register the event handler before starting WiFi
  WiFi.onEvent(WiFiEvent);
  // Start the connection
  WiFi.begin(ssid, password);

  // Wait for connection (with a simple animation)
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");

  // ESP32-C5 Dual-Band Status
  // Note: WiFi.getBand() returns 1 for 5GHz and 0 for 2.4GHz
  Serial.print("Connected on Band: ");
  if (WiFi.getBand() == 1) {
    Serial.println("5GHz (WiFi 6)");
  } else {
    Serial.println("2.4GHz");
  }
  
  Serial.print("Signal Strength (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");


  // IP Address info
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Sync the clock (used for the on-screen time, the midnight reset, and to
  // tell whether a restored snapshot belongs to today). Wait briefly for NTP.
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");
  Serial.print("Syncing time");
  unsigned long tStart = millis();
  while (time(nullptr) < 100000 && millis() - tStart < 8000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  // Setup MQTT (Discovery for HA + subscribe to the retained totals)
  setup_mqtt();

  // Give the broker a moment to replay the retained snapshot, then stop
  // listening so our own future publishes don't reset us.
  Serial.println("Restoring saved totals from MQTT...");
  unsigned long rStart = millis();
  while (!persistLoaded && millis() - rStart < 2000) {
    client.loop();
    delay(20);
  }
  restoreDone = true;
  client.unsubscribe(persist_topic);
  Serial.println(persistLoaded ? "Totals restored." : "No saved totals found.");

  // Initalise the c4002
  init_c4002();

  // Calibrate
  if (calibrate) {
    c4002_start_calibration();
  }

  // Start "away" from now so the first detection shows a sane break length
  absenceStartSec = millis() / 1000;
}

// Refresh the on-screen clock and reset the daily totals at local midnight
void update_clock_and_day() {
  time_t now = time(nullptr);
  if (now < 100000) {          // NTP not synced yet
    strcpy(uiClock, "--:--");
    return;
  }
  struct tm t;
  localtime_r(&now, &t);
  snprintf(uiClock, sizeof(uiClock), "%02d:%02d", t.tm_hour, t.tm_min);

  if (trackedYDay == -1) {
    trackedYDay = t.tm_yday;
  } else if (t.tm_yday != trackedYDay) {
    // New day: zero the daily totals and rebase the running timer
    trackedYDay = t.tm_yday;
    dailyPresenceSec = 0;
    dailyAbsenceSec  = 0;
    workAbsenceSec   = 0;
    unsigned long nowSec = millis() / 1000;
    if (isPresent) sessionStartSec = nowSec;
    else           absenceStartSec = nowSec;
  }
}

// Account for absence that falls inside working hours and compute the % away.
// Uses the wall clock so it ignores time outside 08:00-17:00 and freezes at 17:00.
void account_working_hours() {
  time_t now = time(nullptr);
  if (now < 100000) {            // clock not synced yet
    uiWorkAwayPct = 0;
    return;
  }
  struct tm t;
  localtime_r(&now, &t);

  // Today's working window expressed as epoch times
  struct tm sd = t;
  sd.tm_hour = 0; sd.tm_min = 0; sd.tm_sec = 0;
  time_t midnight = mktime(&sd);
  time_t wStart = midnight + (time_t)DAY_START_HOUR * 3600;
  time_t wEnd   = midnight + (time_t)DAY_END_HOUR   * 3600;

  // Add the portion of [lastTick, now] that was absent AND inside the window
  if (lastAccountEpoch > 0 && now > lastAccountEpoch && !isPresent) {
    time_t a = lastAccountEpoch > wStart ? lastAccountEpoch : wStart;
    time_t b = now < wEnd ? now : wEnd;
    if (b > a) workAbsenceSec += (unsigned long)(b - a);
  }
  lastAccountEpoch = now;

  // Working seconds elapsed so far today (0 before 08:00, capped at 17:00)
  long sod = (long)t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  long wLen = (long)(DAY_END_HOUR - DAY_START_HOUR) * 3600;
  long elapsed = sod - (long)DAY_START_HOUR * 3600;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > wLen) elapsed = wLen;

  if (elapsed > 0) {
    long pct = (long)workAbsenceSec * 100 / elapsed;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uiWorkAwayPct = (int)pct;
  } else {
    uiWorkAwayPct = 0;         // before the working day starts
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) client.loop();

  // c4002 sensor calibration happening
  if (calibrate) {
    calibration_loop();
    delay(100);
    return;
  }

  // Keep the clock fresh and roll the daily totals over at midnight
  update_clock_and_day();

  bool transitioned = false;     // set on a presence change, to force a save
  sRetResult_t retResult = c4002_getNoteInfo();

  if (retResult.noteType == eResult) {
    eTargetState_t currentState = c4002_getTargetState();
    sPresenceTarget_t target = c4002_getPresenceTargetInfo();
    unsigned long runningSeconds = millis() / 1000;
    bool detected = (currentState == eMotion || currentState == ePresence);

    // While away, wait for detection to persist before counting me present.
    // This filters out walk-bys: a brief blip resets the candidate timer.
    if (!isPresent) {
      if (detected) {
        if (!candidate) {
          candidate = true;
          candidateStartSec = runningSeconds;   // moment detection began
        }

        // TRANSITION: away -> at desk (detection held long enough)
        if (runningSeconds - candidateStartSec >= PRESENCE_CONFIRM_SEC) {
          // Break ended when I actually arrived, not when it was confirmed
          unsigned long awayDur =
              candidateStartSec > absenceStartSec ? candidateStartSec - absenceStartSec : 0;
          dailyAbsenceSec += awayDur;
          lastBreakSec = awayDur;
          isPresent = true;
          candidate = false;
          sessionStartSec = candidateStartSec;  // count session from arrival

          Serial.print("[");
          Serial.print(runningSeconds);
          Serial.print("s] STATUS: Welcome back, away for ");
          Serial.print(awayDur);
          Serial.println("s");

          // Wake the screen and greet the user
          screenOn();
          display_welcome_back(awayDur);
          displayMode = MODE_WELCOME;
          welcomeUntilMs = millis() + WELCOME_MS;
          transitioned = true;

          if (WiFi.status() == WL_CONNECTED) {
            client.publish(state_topic, "ON");
            client.publish(timer_state_topic, "0");
            client.publish(timerabsent_state_topic, String(awayDur).c_str());
          }
        }
      } else {
        candidate = false;          // detection dropped: it was just a walk-by
      }
    }

    // TRANSITION: at desk -> away  (the user just left)
    else if (isPresent && currentState == eNoTarget) {
      unsigned long sessDur = runningSeconds - sessionStartSec;
      dailyPresenceSec += sessDur;
      lastSessionSec = sessDur;
      isPresent = false;
      absenceStartSec = runningSeconds;

      Serial.print("[");
      Serial.print(runningSeconds);
      Serial.print("s] STATUS: Desk vacated, session was ");
      Serial.print(sessDur);
      Serial.println("s");

      // Blank the screen while away
      screenOff();
      displayMode = MODE_OFF;
      transitioned = true;

      if (WiFi.status() == WL_CONNECTED) {
        client.publish(state_topic, "OFF");
        client.publish(timer_state_topic, String(sessDur).c_str());
        client.publish(timerabsent_state_topic, "0");
      }
    }

    // Periodic Home Assistant update every 10s
    if (WiFi.status() == WL_CONNECTED && (runningSeconds - lastha) >= 10) {
      if (isPresent) {
        client.publish(state_topic, "ON");
        client.publish(timer_state_topic, String(runningSeconds - sessionStartSec).c_str());
      } else {
        client.publish(state_topic, "OFF");
        client.publish(timerabsent_state_topic, String(runningSeconds - absenceStartSec).c_str());
      }
      lastha = runningSeconds;
    }
  }

  // ---- Working-hours absence accounting ----
  account_working_hours();

  // ---- Recompute the values the display shows ----
  unsigned long nowSec = millis() / 1000;
  if (isPresent) {
    uiSessionSec   = nowSec - sessionStartSec;
    uiTodayDeskSec = dailyPresenceSec + uiSessionSec;
    uiTodayAwaySec = dailyAbsenceSec;
    uiBreakAlert   = uiSessionSec >= BREAK_THRESHOLD_MIN * 60UL;
  } else {
    uiSessionSec   = 0;
    uiTodayDeskSec = dailyPresenceSec;
    uiTodayAwaySec = dailyAbsenceSec + (nowSec - absenceStartSec);
    uiBreakAlert   = false;
  }
  uiLastBreakSec = lastBreakSec;

  // ---- Drive the display state machine ----
  if (displayMode == MODE_WELCOME && millis() > welcomeUntilMs) {
    displayMode = MODE_DASHBOARD;
    display_dashboard_static();
    lastDrawSec = nowSec - 1;        // force an immediate refresh below
  }
  if (displayMode == MODE_DASHBOARD && nowSec != lastDrawSec) {
    display_dashboard_update();
    lastDrawSec = nowSec;
  }

  // ---- Persist the running totals to the broker (survives reboots) ----
  if (transitioned || (nowSec - lastPersistSec) >= PERSIST_INTERVAL_SEC) {
    publishPersist();
    lastPersistSec = nowSec;
  }

  delay(100); // Small delay to prevent CPU hammering
}