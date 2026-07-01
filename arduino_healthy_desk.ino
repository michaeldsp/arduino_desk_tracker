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
// Retained topics used to persist state across reboots
const char* persist_topic  = "homeassistant/desk/persist";
const char* timeline_topic = "homeassistant/desk/timeline";
bool calibrate = false;

// ===================== USER-TUNABLE SETTINGS =====================
// Complain and tell me to take a break after this many seated minutes.
const unsigned long BREAK_THRESHOLD_MIN = 120;
// How long the "Welcome back" splash stays up before the dashboard (ms).
const unsigned long WELCOME_MS          = 5000;
// Local time for the clock / midnight reset. SAST is UTC+2, no DST.
const long GMT_OFFSET_SEC      = 2 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;
// Screen backlight by time of day (percent of full brightness).
// DAY level is used between DAY_START_HOUR and DAY_END_HOUR, AFTERHOURS otherwise.
const uint8_t BRIGHTNESS_DAY        = 50;   // 08:00 - 17:00
const uint8_t BRIGHTNESS_AFTERHOURS = 30;   // outside working hours
const int     DAY_START_HOUR        = 8;    // 08:00
const int     DAY_END_HOUR          = 17;   // 17:00
// How often to save the running totals to the broker (seconds).
const unsigned long PERSIST_INTERVAL_SEC = 30;
// Require this much continuous detection before counting me as "present",
// so just walking past the sensor doesn't trigger a session.
const unsigned long PRESENCE_CONFIRM_SEC = 30;
// If the sensor loses me (e.g. I sit very still), wait this long before marking
// me absent. If it re-detects me within this window, presence stays unbroken.
const unsigned long LEAVE_GRACE_SEC = 30;
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
bool seenToday = false;               // have I been present at least once today?

// Debounce for arriving: detection must persist before we count it as present
bool          candidate         = false;
unsigned long candidateStartSec = 0;

// Grace period for leaving: tolerate the sensor briefly losing me
bool          pendingLeave = false;   // detection lost, waiting to confirm absence
unsigned long leaveLostSec = 0;       // moment detection was lost

// Absence that falls inside working hours (08:00-17:00), accounted by wall clock
unsigned long workAbsenceSec   = 0;   // absent seconds within the working window today
time_t        lastAccountEpoch = 0;   // wall-clock time of the last accounting tick

// Per-minute timeline of the whole day for the 24h view.
// 0=not reached, 1=present/work, 2=present/off, 3=away/work, 4=away/off
uint8_t dayTimeline[1440] = {0};
int     lastTimelineMin   = -1;       // last minute index we recorded
int     uiDayMinute       = 0;        // current minute of day (display change-detect)

// Values handed to the display layer (display.ino reads these)
unsigned long uiSessionSec   = 0;
unsigned long uiTodayDeskSec = 0;
unsigned long uiTodayAwaySec = 0;
unsigned long uiLastBreakSec = 0;
int           uiWorkAwayPct  = 0;     // % of working hours so far spent away
bool          uiBreakAlert   = false;
bool          uiSearching    = false; // sensor lost me; in the leave-grace window
char          uiClock[8]     = "--:--";

// Display state machine
enum DisplayMode { MODE_OFF, MODE_WELCOME, MODE_DASHBOARD };
DisplayMode displayMode = MODE_OFF;
unsigned long welcomeUntilMs = 0;
unsigned long lastDrawSec    = 0;

// Persistence (restore-from-broker) state
unsigned long lastPersistSec = 0;
bool restoreDone    = false;  // true once the boot-time restore window has closed
bool persistLoaded  = false;  // true if a retained totals snapshot was found and applied
bool timelineLoaded = false;  // true if a retained timeline snapshot was found and applied

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

// Current minute-of-day, or -1 if the clock isn't synced
static int currentMinuteOfDay() {
  time_t now = time(nullptr);
  if (now < 100000) return -1;
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_hour * 60 + t.tm_min;
}

// MQTT message handler: restores the retained snapshots at boot (once)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (restoreDone) return;                          // ignore our own later publishes

  // --- 24h timeline: "<yday>;<1440 chars of 0-4>" ---
  if (strcmp(topic, timeline_topic) == 0) {
    unsigned int i = 0;
    long day = 0;
    bool haveDay = false;
    for (; i < length && payload[i] != ';'; i++) {
      if (payload[i] >= '0' && payload[i] <= '9') {
        day = day * 10 + (payload[i] - '0');
        haveDay = true;
      }
    }
    if (i < length && payload[i] == ';') i++;       // skip the separator

    int today = -1;
    time_t now = time(nullptr);
    struct tm t;
    if (now > 100000) { localtime_r(&now, &t); today = t.tm_yday; }

    if (haveDay && today >= 0 && day == today) {
      for (int m = 0; m < 1440 && i < length; m++, i++) {
        uint8_t c = payload[i];
        dayTimeline[m] = (c >= '0' && c <= '4') ? (uint8_t)(c - '0') : 0;
      }
      lastTimelineMin = today >= 0 ? (t.tm_hour * 60 + t.tm_min) : -1;
      Serial.println("Restored 24h timeline from MQTT.");
    }
    timelineLoaded = true;
    return;
  }

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
    seenToday = (desk > 0);          // already been present today?
    trackedYDay = today;
  } else {
    // Snapshot is from another day (or clock unknown): start the day fresh
    dailyPresenceSec = 0;
    dailyAbsenceSec  = 0;
    workAbsenceSec   = 0;
    seenToday = false;
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

// Save the 24h timeline to the broker as a retained "<yday>;<1440 chars>" message
void publishTimeline() {
  if (WiFi.status() != WL_CONNECTED || !client.connected()) return;
  static char tbuf[1460];
  int n = snprintf(tbuf, sizeof(tbuf), "%d;", trackedYDay);
  for (int m = 0; m < 1440 && n < (int)sizeof(tbuf) - 1; m++) {
    uint8_t st = dayTimeline[m] > 4 ? 0 : dayTimeline[m];
    tbuf[n++] = '0' + st;
  }
  tbuf[n] = '\0';
  client.publish(timeline_topic, tbuf, true);   // retained
}

// SETUP mqtt discovery
void setup_mqtt() {
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
  client.setBufferSize(1600);   // big enough for the 1440-char timeline message

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

      // Subscribe so the broker replays the retained snapshots
      client.subscribe(persist_topic);
      client.subscribe(timeline_topic);
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

  // Show a status splash so the boot delay isn't a blank screen
  screenOn();
  display_boot_status("Connecting to WiFi...");

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
  display_boot_status("Syncing time...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");
  Serial.print("Syncing time");
  unsigned long tStart = millis();
  while (time(nullptr) < 100000 && millis() - tStart < 8000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  // Setup MQTT (Discovery for HA + subscribe to the retained totals)
  display_boot_status("Connecting to broker...");
  setup_mqtt();

  // Wait for the broker to replay the retained snapshot, then stop listening
  // so our own future publishes don't reset us. The loop exits the instant the
  // message arrives, so this only ever waits the full time on a first-ever boot
  // (when no snapshot exists yet) -- normal reboots restore in milliseconds.
  Serial.println("Restoring saved state from MQTT...");
  display_boot_status("Restoring data...");
  unsigned long rStart = millis();
  while (!(persistLoaded && timelineLoaded) && millis() - rStart < 5000) {
    client.loop();
    delay(20);
  }
  restoreDone = true;
  client.unsubscribe(persist_topic);
  client.unsubscribe(timeline_topic);
  Serial.println(persistLoaded ? "Totals restored." : "No saved totals found.");

  // Initalise the c4002
  init_c4002();

  // Calibrate
  if (calibrate) {
    c4002_start_calibration();
  }

  // Start "away" from now so the first detection shows a sane break length
  absenceStartSec = millis() / 1000;

  // Boot finished: go dark until presence is detected
  screenOff();
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
    seenToday = isPresent;        // away doesn't count until first presence today
    memset(dayTimeline, 0, sizeof(dayTimeline));   // clear the 24h view
    lastTimelineMin = -1;
    unsigned long nowSec = millis() / 1000;
    if (isPresent) sessionStartSec = nowSec;
    else           absenceStartSec = nowSec;
  }
}

// Record the current minute of the day into the 24h timeline.
// Fills any skipped minutes so brief processing gaps don't leave holes.
void record_timeline() {
  int m = currentMinuteOfDay();
  if (m < 0) return;                       // clock not synced yet
  uiDayMinute = m;

  bool inWork = (m >= DAY_START_HOUR * 60 && m < DAY_END_HOUR * 60);
  uint8_t st = isPresent ? (inWork ? 1 : 2) : (inWork ? 3 : 4);

  if (lastTimelineMin < 0 || m == lastTimelineMin) {
    dayTimeline[m] = st;
  } else {
    // advance from the last recorded minute up to now (same day, m increases)
    int idx = lastTimelineMin;
    while (idx != m) {
      idx = (idx + 1) % 1440;
      dayTimeline[idx] = st;
    }
  }
  lastTimelineMin = m;
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
          bool firstToday = !seenToday;
          if (seenToday) {
            // A genuine break between sessions today: count it
            dailyAbsenceSec += awayDur;
            lastBreakSec = awayDur;
          } else {
            // First presence of the day: the time before I arrived isn't a break
            seenToday = true;
          }
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
          if (firstToday) display_good_morning();     // first time seen today
          else            display_welcome_back(awayDur);
          displayMode = MODE_WELCOME;
          welcomeUntilMs = millis() + WELCOME_MS;
          transitioned = true;

          if (WiFi.status() == WL_CONNECTED) {
            // First arrival has no "break" to report, so publish 0 absent
            String absentPayload = firstToday ? String(0) : String(awayDur);
            client.publish(state_topic, "ON");
            client.publish(timer_state_topic, "0");
            client.publish(timerabsent_state_topic, absentPayload.c_str());
          }
        }
      } else {
        candidate = false;          // detection dropped: it was just a walk-by
      }
    }

    // Currently present: tolerate the sensor briefly losing me before leaving.
    else {
      if (detected) {
        pendingLeave = false;       // still here (re-detected): cancel any grace
      } else {
        // Detection lost: start/continue the leave-grace timer
        if (!pendingLeave) {
          pendingLeave = true;
          leaveLostSec = runningSeconds;
        }

        // TRANSITION: at desk -> away (lost for the whole grace window)
        else if (runningSeconds - leaveLostSec >= LEAVE_GRACE_SEC) {
          // Session ended (and the break began) when detection was lost
          unsigned long sessDur =
              leaveLostSec > sessionStartSec ? leaveLostSec - sessionStartSec : 0;
          dailyPresenceSec += sessDur;
          lastSessionSec = sessDur;
          isPresent = false;
          pendingLeave = false;
          absenceStartSec = leaveLostSec;

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

  // ---- Working-hours absence accounting + 24h timeline ----
  account_working_hours();
  record_timeline();

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
    // Away only counts once I've been present today (ignore the overnight/pre-arrival gap)
    uiTodayAwaySec = seenToday ? dailyAbsenceSec + (nowSec - absenceStartSec) : 0;
    uiBreakAlert   = false;
  }
  uiLastBreakSec = lastBreakSec;
  uiSearching    = isPresent && pendingLeave;   // sensor lost me, in grace window

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

  // ---- Persist the running totals + timeline to the broker (survives reboots) ----
  if (transitioned || (nowSec - lastPersistSec) >= PERSIST_INTERVAL_SEC) {
    publishPersist();
    publishTimeline();
    lastPersistSec = nowSec;
  }

  delay(100); // Small delay to prevent CPU hammering
}