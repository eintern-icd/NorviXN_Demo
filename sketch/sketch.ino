#include <Arduino.h>

// Buffer to store incoming serial characters
String inputString = "";
bool stringComplete = false;

void processAtCommand(String cmd);

void setup() {
  // Initialize USB CDC Serial (Native USB / programming port)
  Serial.begin(115200);
  delay(500);

  // Clear input buffer
  inputString.reserve(200);

  Serial.println(F("\r\n========================================"));
  Serial.println(F("NORVI X2 AT Command Processor Initialized"));
  Serial.println(F("Type AT commands and press Enter (NL/CR)."));
  Serial.println(F("========================================"));
  Serial.println(F("OK"));
}

void loop() {
  // Read incoming serial data
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    
    // Add to input string if not newline/carriage return
    if (inChar == '\n' || inChar == '\r') {
      if (inputString.length() > 0) {
        stringComplete = true;
      }
    } else {
      inputString += inChar;
    }
  }

  // Process completed command line
  if (stringComplete) {
    processAtCommand(inputString);
    inputString = "";
    stringComplete = false;
  }
}

// Function to parse and handle AT commands
void processAtCommand(String cmd) {
  // Convert command to uppercase for reliable matching
  cmd.trim();
  String upperCmd = cmd;
  upperCmd.toUpperCase();

  // Echo command back if desired, or handle specific AT commands
  if (upperCmd == "AT") {
    Serial.println(F("OK"));
  }
  else if (upperCmd == "AT+RST") {
    Serial.println(F("OK"));
    delay(100);
    ESP.restart();
  }
  else if (upperCmd == "AT+GMR") {
    Serial.println(F("NORVI-X2-ESP32S3 Firmware v1.0.0"));
    Serial.println(F("OK"));
  }
  else if (upperCmd == "AT+INFO") {
    Serial.println(F("Device: NORVI X2 (ESP32-S3-WROOM-1U-N16R2)"));
    Serial.print(F("Free Heap: "));
    Serial.println(ESP.getFreeHeap());
    Serial.print(F("Chip Rev: "));
    Serial.println(ESP.getChipRevision());
    Serial.println(F("OK"));
  }
  else if (upperCmd.startsWith("AT+")) {
    // Generic handler for custom or unrecognized AT commands
    Serial.print(F("Received custom command: "));
    Serial.println(cmd);
    Serial.println(F("OK"));
  }
  else {
    Serial.println(F("ERROR"));
  }
}
