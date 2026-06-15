/*
#include "DFRobot_C4002.h"

DFRobot_C4002 c4002(&Serial1, 115200, 4, 5);

void setup()
{
  Serial.begin(115200);

  // Initialize the C4002 sensor
  while (c4002.begin() != true) {
    Serial.println("C4002 begin failed!");
    delay(1000);
  }
  Serial.println("C4002 begin success!");
  delay(50);

  // Set the run led to off
  if (c4002.setRunLedState(eLedOff)) {
    Serial.println("Set run led success!");
  } else {
    Serial.println("Set run led failed!");
  }
  delay(50);

  // Set the out led to off
  if (c4002.setOutLedState(eLedOff)) {
    Serial.println("Set out led success!");
  } else {
    Serial.println("Set out led failed!");
  }
  delay(50);

  // Set the Resolution mode to 80cm.
  if (c4002.setResolutionMode(eResolution80Cm)) {
    Serial.println("Set resolution mode success!");
  } else {
    Serial.println("Set resolution mode failed!");
  }
  delay(50);

  // Set the detect range to 0-1100 cm
  uint16_t clostRange = 10;
  uint16_t farRange   = 0;
  if (c4002.setDetectRange(clostRange, farRange)) {    // Max detect range(0-1100cm)
    Serial.println("Set detect range success!");
  } else {
    Serial.println("Set detect range failed!");
  }
  delay(50);

  // Set the light threshold to 0 lux.range: 0-50 lux
  if (c4002.setLightThresh(0)) {
    Serial.println("Set light threshold success!");
  } else {
    Serial.println("Set light threshold failed!");
  }
  delay(50);

  uint8_t gateState[15] = { C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE, C4002_ENABLE };
  // Resolution mode:eResolution20Cm,This means that the number of 'distance gates' we can operate is 25
  // uint8_t gateState[25] = {C4002_DISABLE,C4002_DISABLE,C4002_ENABLE,...,C4002_ENABLE,C4002_DISABLE};
  if (c4002.configureGate(eMotionDistGate, gateState)) {    // Operation motion distance gate
    Serial.println("Enable motion distance gate success!");
  }
  delay(50);
  if (c4002.configureGate(ePresenceDistGate, gateState)) {    // Operation presence distance gate
    Serial.println("Enable presence distance gate success!");
  }
  delay(50);

  // Set the target disappear delay time to 1s，range: 0-65535s
  if (c4002.setTargetDisappearDelay(1)) {
    Serial.println("Set target disappear delay time success!");
  } else {
    Serial.println("Set target disappear delay time failed!");
  }
  delay(50);

  // Set the report period to 10 * 0.1s = 1s
  if (c4002.setReportPeriod(10)) {
    Serial.println("Set report period success!");
  } else {
    Serial.println("Set report period failed!");
  }

}

void loop()
{
  // Get all the results of the C4002 sensor,Default loop execution
  sRetResult_t retResult = c4002.getNoteInfo();

  if (retResult.noteType == eResult) {
    Serial.println("------- Get all results --------");
    // get the light intensity
    float light = c4002.getLightIntensity();
    Serial.print("Light: ");
    Serial.print(light);
    Serial.println(" lux");

    // get Target state
    eTargetState_t targetState = c4002.getTargetState();
    Serial.print("Target state: ");
    if (targetState == eNoTarget) {
      Serial.println("No Target");
    } else if (targetState == ePresence) {
      Serial.println("Static Presence");
    } else if (targetState == eMotion) {
      Serial.println("Motion");
    }

    // get presence count down
    uint16_t presenceGateCount = c4002.getPresenceCountDown();
    Serial.print("Presence distance gate count down: ");
    Serial.print(presenceGateCount);
    Serial.println(" s");

    // get Presence distance gate target info
    sPresenceTarget_t presenceTarget = c4002.getPresenceTargetInfo();
    Serial.print("Presence distance: ");
    Serial.print(presenceTarget.distance);
    Serial.println(" m");
    Serial.print("Presence energy: ");
    Serial.println(presenceTarget.energy);

    // get motion distance gate index
    sMotionTarget_t motionTarget = c4002.getMotionTargetInfo();
    Serial.print("Motion distance: ");
    Serial.print(motionTarget.distance);
    Serial.println(" m");
    Serial.print("Motion energy: ");
    Serial.println(motionTarget.energy);
    Serial.print("Motion speed: ");
    Serial.print(motionTarget.speed);
    Serial.println(" m/s");
    Serial.print("Motion direction: ");
    if (motionTarget.direction == eAway) {
      Serial.println("Away!");
    } else if (motionTarget.direction == eNoDirection) {
      Serial.println("No Direction!");
    } else if (motionTarget.direction == eApproaching) {
      Serial.println("Approaching!");
    }
    Serial.println("--------------------------------");
  }
  delay(50);
}
*/
#include "DFRobot_C4002.h"
#include <WiFi.h>
#include <PubSubClient.h>

// Define the pins for ESP32-C5 (Change if your wiring differs)
DFRobot_C4002 c4002(&Serial1, 115200, /*RX*/ A3, /*TX*/ A4);

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

// Global variables for tracking state
eTargetState_t lastState = eNoTarget;
bool isPresent = false;
unsigned long timeLastChanged = 0;
WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastha = 0;

// SETUP mqtt discovery
void setup_mqtt() {
  client.setServer(mqtt_server, 1883);
  
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

  // Setup MQTT (Discovery for HA)
  setup_mqtt();

  // init c4002
  while (c4002.begin() != true) {
    Serial.println("C4002 begin failed!");
    delay(1000);
  }

  // Minimal Setup for Presence Detection
  c4002.setRunLedState(eLedOff);
  delay(50);
  c4002.setOutLedState(eLedOff);
  delay(50);
  c4002.setResolutionMode(eResolution80Cm);
  delay(50);
  c4002.setDetectRange(5, 100); // 10cm to 5m - adjust as needed
  delay(50);
  c4002.setTargetDisappearDelay(1); 
  delay(50);
  c4002.setReportPeriod(10); // 1s reports
  delay(50);
  c4002.setLightThresh(0); // ignore light
  delay(50);
  lastha = 0;

  Serial.println("Monitoring for Human Presence...");
  Serial.println("Time(s) | Event");
  Serial.println("-------------------------");
}

void loop() {
  sRetResult_t retResult = c4002.getNoteInfo();

  if (retResult.noteType == eResult) {
    eTargetState_t currentState = c4002.getTargetState();
    
    // Calculate running seconds
    unsigned long runningSeconds = millis() / 1000;

    // LOGIC: Transition from "None" to "Present" (Moving or Static)
    if (!isPresent && (currentState == eMotion || currentState == ePresence)) {
      int diff = (runningSeconds - timeLastChanged);
      isPresent = true;
      Serial.print("[");
      Serial.print(runningSeconds);
      Serial.print("s] STATUS: Human Detected (");
      Serial.print(currentState == eMotion ? "Motion" : "Static");
      Serial.print(") interval=");
      Serial.print(diff);
      Serial.println();
      timeLastChanged = runningSeconds;
      if (WiFi.status() == WL_CONNECTED) { 
        client.publish(state_topic, "ON");
        client.publish(timer_state_topic, String(diff).c_str());
        client.publish(timerabsent_state_topic, String(0).c_str());
      }
    } 
    
    // LOGIC: Transition from "Present" back to "None"
    else if (isPresent && currentState == eNoTarget) {
      int diff = (runningSeconds - timeLastChanged);
      isPresent = false;
      Serial.print("[");
      Serial.print(runningSeconds);
      Serial.print("s] STATUS: Area Vacated (No Target) interval=");
      Serial.print(diff);
      Serial.println();
      timeLastChanged = runningSeconds;

      // If WIFI UP we can do things
      if (WiFi.status() == WL_CONNECTED) { 
        client.publish(state_topic, "OFF");
        client.publish(timerabsent_state_topic, String(diff).c_str());
        client.publish(timer_state_topic, String(0).c_str());
      }
    }

    // update HA every 10 seconds of prescense
    
    if (WiFi.status() == WL_CONNECTED && (runningSeconds - lastha) >= 10) {
      int diff = (runningSeconds - timeLastChanged);
      if (currentState == eMotion || currentState == ePresence) {
        client.publish(timer_state_topic, String(diff).c_str());
        client.publish(state_topic, "ON");
        Serial.print("update HA occupied running=");
        Serial.print(runningSeconds);
        Serial.print(" lastha=");
        Serial.println(lastha);
      } else {
        client.publish(timerabsent_state_topic, String(diff).c_str());
        client.publish(state_topic, "OFF");
        Serial.print("update HA vacant running=");
        Serial.print(runningSeconds);
        Serial.print(" lastha=");
        Serial.println(lastha);
      }
      lastha = runningSeconds;
    }

  }
  
  display_refresh();
  delay(100); // Small delay to prevent CPU hammering
}