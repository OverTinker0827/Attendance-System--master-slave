#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <SoftwareSerial.h>

// SoftwareSerial soft(14,12); //D5, D6 RX, TX
SoftwareSerial soft(14,5); //D5, D1 RX, TX
// Debug output on Serial1 (TX1 = GPIO2/D4) - connect USB-TTL RX to D4
#define DEBUG_BAUD 115200
#define DEBUG Serial

// LED for status indication (built-in LED on most ESP8266 boards)
#define LED_PIN 16 // GPIO16 (D0) - safer than GPIO2 which is used for boot

// ==================== Configuration ====================
#define SLAVE_ADDRESS "RVU101"              // This slave's address - change for each device
#define UART_BAUD 9600                  // UART baud rate
#define SSID "RV_CLASS_1"
#define PASSWORD "123456789"
#define DEVICE_IP "192.168.0.10"
#define SUBNET_MASK "255.255.255.0"
#define GATEWAY "192.168.0.10"
#define ACTIVE_DURATION 1.2 * 60 * 1000  // 45 minutes in milliseconds
#define JSON_BUFFER_SIZE 512

// UART Protocol characters
#define START_CHAR '<'
#define END_CHAR '>'
#define SEPARATOR '|'

// ==================== State Definitions ====================
enum DeviceState {
  HALT,
  ACTIVE,
  SEND
};

// ==================== Global Variables ====================
DeviceState currentState = HALT;
ESP8266WebServer server(80);
std::vector<std::string> receivedUSNs;
std::vector<uint8_t> markedAttendance;
unsigned long activeStartTime = 0;
String uartBuffer = "";
bool messageStarted = false;

// Add a testing flag to bypass UART receive
bool testing = false;

// Forward declarations
void setupHTTPServer();
void parseUARTMessage(String message);
void sendAttendanceResponse();
void blinkLED(int times, int onTime, int offTime);

// ==================== LED Functions ====================
void blinkLED(int times, int onTime = 100, int offTime = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);   // LED ON (active low on most ESP8266)
    delay(onTime);
    digitalWrite(LED_PIN, HIGH);  // LED OFF
    if (i < times - 1) delay(offTime);
  }
}

// ==================== UART Functions ====================
void processUARTInput() {
  while (soft.available()) {
    char c = soft.read();
    
    if (c == START_CHAR) {
      // Start of new message
      uartBuffer = "";
      messageStarted = true;
    } else if (c == END_CHAR && messageStarted) {
      // End of message - process it
      messageStarted = false;
      parseUARTMessage(uartBuffer);
      uartBuffer = "";
    } else if (messageStarted) {
      // Add character to buffer
      uartBuffer += c;
    }
  }
 
}

void parseUARTMessage(String message) {
  // Message format: address|usn1|usn2|usn3|...
  // First field is address, rest are USNs
  
  int firstSep = message.indexOf(SEPARATOR);
  String address;
  String usnData;
  
  if (firstSep == -1) {
    // No separator - entire message is address (no USNs)
    address = message;
    usnData = "";
  } else {
    address = message.substring(0, firstSep);
    usnData = message.substring(firstSep + 1);
  }
  
  address.trim();
  
  // Check if address matches this slave
  if (address != SLAVE_ADDRESS) {
    // Address doesn't match - ignore message
    DEBUG.print("[UART] Address mismatch. Expected: ");
    DEBUG.print(SLAVE_ADDRESS);
    DEBUG.print(", Got: ");
    DEBUG.println(address);
    return;
  }
  
  DEBUG.println("[UART] Address matched!");
  
  // Address matches - only process if in HALT state
  if (currentState != HALT) {
    DEBUG.println("[UART] Not in HALT state, ignoring");
    return;
  }
  
  // Parse USNs (separated by |)
  receivedUSNs.clear();
  markedAttendance.clear();
  
  int startIdx = 0;
  int sepIdx = 0;
  
  while ((sepIdx = usnData.indexOf(SEPARATOR, startIdx)) != -1) {
    String usn = usnData.substring(startIdx, sepIdx);
    usn.trim();
    if (usn.length() > 0) {
      receivedUSNs.push_back(usn.c_str());
      markedAttendance.push_back(0);
    }
    startIdx = sepIdx + 1;
  }
  
  // Add last USN
  String usn = usnData.substring(startIdx);
  usn.trim();
  if (usn.length() > 0) {
    receivedUSNs.push_back(usn.c_str());
    markedAttendance.push_back(0);
  }
  
  // Transition to ACTIVE state
  currentState = ACTIVE;
  activeStartTime = millis();
  soft.println(usnData);
  setupHTTPServer();
  
  // Blink LED 3 times - got data from master
  blinkLED(3, 200, 200);
  
  DEBUG.println("[STATE] Transitioned to ACTIVE");
  DEBUG.print("[STATE] Received ");
  DEBUG.print(receivedUSNs.size());
  DEBUG.println(" USNs:");
  for (size_t i = 0; i < receivedUSNs.size(); i++) {
    DEBUG.print("  - ");
    DEBUG.println(receivedUSNs[i].c_str());
  }
  DEBUG.println("[HTTP] Server started on port 80");
}

void sendAttendanceResponse() {
  // Send format: <address|usn1|usn2|...>
  // Only send USNs that were marked as present (attendance = 1)
  
  DEBUG.println("[STATE] Sending attendance response");
  
  String response = "";
  response += START_CHAR;
  response += SLAVE_ADDRESS;
  
  int markedCount = 0;
  for (size_t i = 0; i < markedAttendance.size(); i++) {
    if (markedAttendance[i] == 1) {
      response += SEPARATOR;
      response += receivedUSNs[i].c_str();
      markedCount++;
    }
  }
  
  response += END_CHAR;
  
  DEBUG.print("[UART] Sending: ");
  DEBUG.println(response);
  DEBUG.print("[STATE] Marked attendance count: ");
  DEBUG.println(markedCount);
  
  soft.print(response);
  
  // Transition back to HALT
  currentState = HALT;
  DEBUG.println("[STATE] Transitioned to HALT");
  
  // Clear data for next session
  receivedUSNs.clear();
  markedAttendance.clear();
}

// ==================== USN Functions ====================
bool isUSNInList(const std::string& usn) {
  for (const auto& receivedUSN : receivedUSNs) {
    if (receivedUSN == usn) {
      return true;
    }
  }
  return false;
}

void markAttendance(const std::string& usn) {
  for (size_t i = 0; i < receivedUSNs.size(); i++) {
    if (receivedUSNs[i] == usn) {
      markedAttendance[i] = 1;
      break;
    }
  }
}

// ==================== HTTP Server Handlers ====================
void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

void handleAttendance() {
  sendCORSHeaders();
  
  DEBUG.println("[HTTP] POST /attendance received");
  
  if (currentState != ACTIVE) {
    DEBUG.println("[HTTP] Error: Not in ACTIVE state");
    server.send(400, "application/json", "{\"error\": \"Device not in active state\"}");
    return;
  }
  
  if (server.method() != HTTP_POST) {
    DEBUG.println("[HTTP] Error: Method not allowed");
    server.send(405, "application/json", "{\"error\": \"Method not allowed\"}");
    return;
  }
  
  String body = server.arg("plain");
  DEBUG.print("[HTTP] Body: ");
  DEBUG.println(body);
  
  // Parse JSON
  StaticJsonDocument<JSON_BUFFER_SIZE> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    DEBUG.println("[HTTP] Error: Invalid JSON");
    server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
    return;
  }
  
  std::string usn = doc["usn"].as<std::string>();
  std::string status = doc["status"].as<std::string>();
  
  DEBUG.print("[HTTP] USN: ");
  DEBUG.print(usn.c_str());
  DEBUG.print(", Status: ");
  DEBUG.println(status.c_str());
  
  String response;
  
  // Check attendance eligibility
  if (status == "success" && isUSNInList(usn)) {
    markAttendance(usn);
    response = "{\"response\": \"attendance marked\"}";
    DEBUG.print("[HTTP] Attendance MARKED for: ");
    DEBUG.println(usn.c_str());
    // Blink LED 1 time - HTTP attendance marked
    blinkLED(1, 100, 0);
  } else {
    response = "{\"response\": \"you are not from this class\"}";
    DEBUG.print("[HTTP] Attendance REJECTED for: ");
    DEBUG.println(usn.c_str());
  }
  
  server.send(200, "application/json", response);
}

void handleNotFound() {
  sendCORSHeaders();
  server.send(404, "application/json", "{\"error\": \"Endpoint not found\"}");
}

// ==================== AP Setup ====================
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 0, 10),
                    IPAddress(192, 168, 0, 10),
                    IPAddress(255, 255, 255, 0));
  bool success = WiFi.softAP(SSID, PASSWORD);
  
  DEBUG.print("[WIFI] AP Setup: ");
  DEBUG.println(success ? "SUCCESS" : "FAILED");
  DEBUG.print("[WIFI] SSID: ");
  DEBUG.println(SSID);
  DEBUG.print("[WIFI] IP: ");
  DEBUG.println(WiFi.softAPIP());
}

// ==================== HTTP Server Setup ====================
void setupHTTPServer() {
  server.on("/attendance", HTTP_POST, handleAttendance);
  server.on("/attendance", HTTP_OPTIONS, handleOptions);  // CORS preflight
  server.onNotFound(handleNotFound);
  server.begin();
}

// ==================== State Machine ====================
void handleStateMachine() {
  static DeviceState lastState = HALT;
  
  if (currentState != lastState) {
    DEBUG.print("[STATE] Changed to: ");
    switch(currentState) {
      case HALT: DEBUG.println("HALT"); break;
      case ACTIVE: DEBUG.println("ACTIVE"); break;
      case SEND: DEBUG.println("SEND"); break;
    }
    lastState = currentState;
  }
  
  switch (currentState) {
    case HALT:
      // Process incoming UART messages
      processUARTInput();
      break;
      
    case ACTIVE:
      // Handle HTTP clients
      server.handleClient();
      
      // Also check for UART input (in case master sends new message)
      //processUARTInput();
      
      // Check if 45 minutes have passed
      if (millis() - activeStartTime >= ACTIVE_DURATION) {
        DEBUG.println("[STATE] Time expired, moving to SEND");
        // Blink LED 5 times fast - timer expired
        blinkLED(5, 50, 50);
        currentState = SEND;
      }
      break;
      
    case SEND:
      // Send attendance response via UART
      sendAttendanceResponse();
      // sendAttendanceResponse() sets state back to HALT
      break;
  }
}

// ==================== Setup ====================
void setup() {
  // Initialize LED pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // LED OFF initially (active low)
  
  // Initialize UART for communication with master
  soft.begin(UART_BAUD);
  
  // Initialize debug serial on GPIO2/D4 (TX only)
  Serial.begin(DEBUG_BAUD);
  delay(1000);
  
  DEBUG.println("\n\n==============================");
  DEBUG.println("   SLAVE DEVICE STARTING");
  DEBUG.println("==============================");
  DEBUG.print("[CONFIG] Address: ");
  DEBUG.println(SLAVE_ADDRESS);
  DEBUG.print("[CONFIG] UART Baud: ");
  DEBUG.println(UART_BAUD);
  DEBUG.print("[CONFIG] Active Duration: ");
  DEBUG.print(ACTIVE_DURATION / 60000);
  DEBUG.println(" minutes");
  
  // Start Access Point
  setupAP();
  
  DEBUG.println("[STATE] Initial state: HALT");
  DEBUG.println("[STATE] Waiting for UART message...");
  DEBUG.println("==============================\n");
  
  // if (testing) {
  //   // Add some test USNs for testing mode
  //   receivedUSNs.push_back("1RV17CS001");
  //   receivedUSNs.push_back("1RV17CS002");
  //   receivedUSNs.push_back("1RV17CS003");
  //   markedAttendance.push_back(0);
  //   markedAttendance.push_back(0);
  //   markedAttendance.push_back(0);
    
  //   currentState = ACTIVE;
  //   activeStartTime = millis();
  //   setupHTTPServer();
  // }
}

// ==================== Main Loop ====================
void loop() {
  handleStateMachine();
  delay(10);
}




