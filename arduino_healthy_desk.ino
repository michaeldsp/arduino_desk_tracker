#include "DFRobot_C4002.h"
#include <WiFi.h>
#include <PubSubClient.h>

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
bool calibrate = false;

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

  // Initalise the c4002
  init_c4002();

  // Calibrate
  if (calibrate) {
    c4002_start_calibration();
  }

}

void loop() {
  // c4002 sensor calibration happening
  if (calibrate) {
    calibration_loop();
    delay(100);
    return;
  }
  sRetResult_t retResult = c4002_getNoteInfo();

  if (retResult.noteType == eResult) {
    eTargetState_t currentState = c4002_getTargetState();
    sPresenceTarget_t target = c4002_getPresenceTargetInfo();
    
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
      Serial.print(") distance=(");
      Serial.print(target.distance);
      Serial.print(") energy=(");
      Serial.print(target.energy);
      Serial.print(") interval=");
      Serial.print(diff);
      Serial.println();
      // Light up screen
      screenOn();

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
      // Light up screen
      screenOff();

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
        Serial.print(") distance=(");
        Serial.print(target.distance);
        Serial.print(") energy=(");
        Serial.print(target.energy);
        Serial.print(") lastha=");
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