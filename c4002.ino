#include "DFRobot_C4002.h"


// Define the pins for ESP32-C5 (Change if your wiring differs)
DFRobot_C4002 c4002(&Serial1, 115200, /*RX*/ A3, /*TX*/ A4);

// initalise the c4002
void init_c4002() {
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
  c4002.setDetectRange(0, 1100); // 10cm to 5m - adjust as needed
  delay(50);
  c4002.setTargetDisappearDelay(1); 
  delay(50);
  c4002.setReportPeriod(10); // 1s reports
  delay(50);
  c4002.setLightThresh(0); // ignore light
  delay(50);
  c4002.setSensitivity(ePresenceDistGate, eMidThreshGroup); // set sensitivity high
  delay(50);
  c4002.setSensitivity(eMotionDistGate, eMidThreshGroup); // set sensitivity high
  delay(50);
  lastha = 0;
}

// start calibration
void c4002_start_calibration() {
  Serial.println("C4002 start event calibration");
  c4002.startEnvCalibration(10, 30);
  Serial.println("calibration done.");
}



// calibration loop
void calibration_loop() {
  //Obtain the calibration results
  sRetResult_t retResult = c4002.getNoteInfo();
  eResolutionMode_t resolutionMode;
  uint8_t           gateData[25] = { 0 };

  if (retResult.noteType == eCalibration) {
    Serial.print("Calibration countdown:");
    Serial.print(retResult.calibCountdown);
    Serial.println(" s");
    if (retResult.calibCountdown == 0) {
      resolutionMode = c4002.getResolutionMode();
      int n          = resolutionMode == eResolution80Cm ? 15 : 25;
      Serial.println("************Environmental Calibration Complete****************");
      if (c4002.getDistanceGateThresh(eMotionDistGate, gateData)) {
        Serial.println("Motion distance gate threshold:");
        printDoorThreshold(gateData, n);
      } else {
        Serial.println("Get motion distance failed!");
      }
      if (c4002.getDistanceGateThresh(ePresenceDistGate, gateData)) {
        Serial.println("Presence distance gate threshold:");
        printDoorThreshold(gateData, n);
      } else {
        Serial.println("Get presence distance failed!");
      }
      Serial.println("**************************************************************");
    }
  }
}

// printDoorThreshold function
void printDoorThreshold(uint8_t *gateData, uint8_t n)
{
  Serial.print("Index:\t");
  for (int i = 0; i < n; i++) {
    Serial.print(i + 1);
    Serial.print('\t');
  }
  Serial.println();
  Serial.print("Value:\t");
  for (int i = 0; i < n; i++) {
    Serial.print(gateData[i]);
    Serial.print('\t');
  }
  Serial.println();
}

// getNoteInfo
sRetResult_t c4002_getNoteInfo() {
  sRetResult_t retResult = c4002.getNoteInfo();
  return retResult;
}

// getTargetState
eTargetState_t c4002_getTargetState() {
  eTargetState_t currentState = c4002.getTargetState();
  return currentState;
}

sPresenceTarget_t c4002_getPresenceTargetInfo() {
  sPresenceTarget_t currentTarget = c4002.getPresenceTargetInfo();
  return currentTarget;
}