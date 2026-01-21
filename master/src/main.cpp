// LED pin (GPIO2, D4 on NodeMCU)
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <SoftwareSerial.h>

#define LED_PIN 2

// SoftwareSerial for both RVU101 and RVU102
SoftwareSerial softSerial(12, 14); // RX=GPIO12, TX=GPIO14
SoftwareSerial softSerial2(5,14); // RX=GPIO5, TX=GPIO14
//  I want sending to RVU001 and RVU002 to be through the same SoftwareSerial instance as they share the same TX line, but for recieving 
// I want separate SoftwareSerials to be used. The tasks given will have either RVU001 or RVU002 or both. So if only one rreciever is needed, only that SoftwareSerial will be used to recieve data., if both are needed, both SoftwareSerials will be used to recieve data.

// Helper: blink LED n times, normal brightness
void blinkLED(int times, int duration = 150) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);   // LED ON (active low)
    delay(duration);
    digitalWrite(LED_PIN, HIGH);  // LED OFF
    delay(duration);
  }
}

// Helper: blink LED n times, half brightness
void blinkLEDHalfBrightness(int times, int duration = 150) {
  for (int i = 0; i < times; i++) {
    analogWrite(LED_PIN, 512);    // Half brightness (range 0-1023)
    delay(duration);
    digitalWrite(LED_PIN, HIGH);  // LED OFF
    delay(duration);
  }
}

// ==================== CONFIGURATION ====================

// AP mode configuration
const char* WIFI_SSID = "Main_Wifi";
const char* WIFI_PASSWORD = "rvcecampus";
const char* RESULT_SERVER_IP = "192.168.4.2";  // IP to assign to the connecting client
const int RESULT_SERVER_PORT = 8080;
const char* RESULT_ENDPOINT = "/results";

// ESP8266 AP static IP
IPAddress apIP(192, 168, 4, 1); // ESP8266 AP IP
IPAddress netMsk(255, 255, 255, 0);
IPAddress clientIP(192, 168, 4, 2); // The only allowed client IP

// UART Configuration
#define UART_BAUD_RATE 9600

// Protocol markers
const char START_MARKER = '<';
const char END_MARKER = '>';
const char SEPARATOR = '|';

// ==================== STATE MACHINE ====================
enum State {
  HALT,    // Waiting for HTTP request with task
  ACTIVE,  // Sending USNs to slaves via UART
  WAIT     // Waiting for responses from slaves
};

State currentState = HALT;

// ==================== DATA STRUCTURES ====================
// Task: address (string) -> list of USNs to send
std::map<String, std::vector<String>> taskData;

// Responses: address (string) -> list of USNs received
std::map<String, std::vector<String>> responseData;

// Track which addresses we're waiting for
std::vector<String> pendingAddresses;

// UART receive buffers for each address
String uartBuffer101 = "";
bool receiving101 = false;
String uartBuffer102 = "";
bool receiving102 = false;

// Timeout for WAIT state (2 minutes = 120000 ms)
const unsigned long WAIT_TIMEOUT = 120000;
unsigned long waitStartTime = 0;
unsigned long lastStatusPrint = 0;

// ==================== WEB SERVER ====================
ESP8266WebServer server(80);

// ==================== FUNCTION DECLARATIONS ====================
void setupWiFi();
void setupWebServer();
void handleRoot();
void handleStartTask();
void handleStatus();
void sendUSNsToAddress(const String& address, const std::vector<String>& usns);
void processUARTData();
void parseReceivedMessage(const String& message);
void sendResultsToServer();
String buildJsonPayload(const std::map<String, std::vector<String>>& data);
void transitionToHalt();
void transitionToActive();
void transitionToWait();
void debugPrint(const String& msg);

// ==================== SETUP ====================
void setup() {
  // Initialize hardware Serial for debug only
  Serial.begin(115200);
  
  // Initialize SoftwareSerial instances
  softSerial.begin(UART_BAUD_RATE);   // For RVU101 receive and shared TX
  softSerial2.begin(UART_BAUD_RATE);  // For RVU102 receive
  delay(100);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED OFF

  debugPrint("\n\n=== ESP8266 UART Master Controller ===");

  setupWiFi();
  setupWebServer();

  debugPrint("System ready in HALT mode");
  debugPrint("Waiting for HTTP commands...");
}

// ==================== MAIN LOOP ====================
void loop() {
  server.handleClient();
  
  // Always check for incoming UART data for both possible addresses
  processUARTData();
  
  switch (currentState) {
    case HALT:
      // Just handle HTTP requests (done above)
      break;
      
    case ACTIVE:
      // Send USNs to all addresses via UART
      debugPrint("ACTIVE: Sending USNs via UART...");
      debugPrint("Total addresses to send to: " + String(taskData.size()));
      for (auto& entry : taskData) {
        String address = entry.first;
        Serial.println("[ACTIVE] Preparing to send to address: " + address);
        sendUSNsToAddress(address, entry.second);
        pendingAddresses.push_back(address);
        Serial.println("[ACTIVE] Added to pendingAddresses: " + address);
        Serial.println("[ACTIVE] pendingAddresses size now: " + String(pendingAddresses.size()));
        delay(50);  // Small delay between transmissions
      }
      Serial.println("[ACTIVE] All messages sent. Total pending: " + String(pendingAddresses.size()));
      Serial.print("[ACTIVE] Pending addresses list: ");
      for (const String& addr : pendingAddresses) {
        Serial.print(addr + ", ");
      }
      Serial.println();
      transitionToWait();
      break;
      
    case WAIT:
      // Print status every 5 seconds
      if (millis() - lastStatusPrint > 5000) {
        lastStatusPrint = millis();
        Serial.println("\\n[WAIT STATUS] ============================");
        Serial.println("[WAIT STATUS] Time elapsed: " + String((millis() - waitStartTime) / 1000) + " seconds");
        Serial.println("[WAIT STATUS] pendingAddresses.size() = " + String(pendingAddresses.size()));
        Serial.println("[WAIT STATUS] Pending addresses:");
        for (const String& addr : pendingAddresses) {
          Serial.println("  - '" + addr + "'");
        }
        Serial.println("[WAIT STATUS] responseData.size() = " + String(responseData.size()));
        Serial.println("[WAIT STATUS] Responses received:");
        for (const auto& entry : responseData) {
          Serial.println("  - '" + entry.first + "' -> " + String(entry.second.size()) + " USNs");
        }
        Serial.println("[WAIT STATUS] taskData.size() = " + String(taskData.size()));
        Serial.println("[WAIT STATUS] Task addresses:");
        for (const auto& entry : taskData) {
          Serial.println("  - '" + entry.first + "'");
        }
        Serial.println("[WAIT STATUS] ============================\\n");
      }
      
      // Check if timeout exceeded
      if (millis() - waitStartTime > WAIT_TIMEOUT) {
        debugPrint("WAIT timeout reached (120 seconds)!");
        debugPrint("Pending addresses: " + String(pendingAddresses.size()));
        if (!responseData.empty()) {
          debugPrint("Sending partial results...");
          sendResultsToServer();
        }
        transitionToHalt();
      }
      // Check if all responses received
      else if (pendingAddresses.empty()) {
        Serial.println("[WAIT] pendingAddresses is EMPTY - checking why...");
        Serial.println("[WAIT] responseData.size() = " + String(responseData.size()));
        Serial.println("[WAIT] Total responses in responseData: " + String(responseData.size()));
        for (const auto& entry : responseData) {
          Serial.println("  [WAIT] '" + entry.first + "' has " + String(entry.second.size()) + " USNs");
        }
        if (responseData.size() < taskData.size()) {
          Serial.println("[WAIT] WARNING: responseData has fewer entries than taskData!");
          Serial.println("[WAIT] Expected " + String(taskData.size()) + " responses, got " + String(responseData.size()));
        }
        debugPrint("All responses received!");
        sendResultsToServer();
        transitionToHalt();
      }
      break;
  }
}

// ==================== DEBUG OUTPUT ====================
// Note: Debug output goes to same UART - disable in production or use different method
void debugPrint(const String& msg) {
  // Only print debug in HALT state to avoid interfering with communication
  // if (currentState == HALT) {
    Serial.println("[DBG] " + msg);

}

// ==================== WIFI SETUP ====================
void setupWiFi() {
  Serial.println("[DBG] Setting up as WiFi AP (host mode)");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, 1, 0, 1); // channel 1, open, max 1 client
  delay(100);
  Serial.print("[DBG] AP IP address: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("[DBG] Waiting for client to connect and take IP: ");
  Serial.println(RESULT_SERVER_IP);
  // Note: The ESP cannot force the client to take a specific IP, but you can instruct the client to use RESULT_SERVER_IP as its static IP.
}

// ==================== WEB SERVER SETUP ====================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/start", HTTP_POST, handleStartTask);
  server.on("/status", HTTP_GET, handleStatus);
  
  server.begin();
  debugPrint("HTTP server started on port 80");
}

// ==================== HTTP HANDLERS ====================
void handleRoot() {
  String html = "<html><head><title>ESP8266 UART Master</title></head><body>";
  html += "<h1>ESP8266 UART Master Controller</h1>";
  html += "<p>State: " + String(currentState == HALT ? "HALT" : (currentState == ACTIVE ? "ACTIVE" : "WAIT")) + "</p>";
  html += "<h2>API Endpoints:</h2>";
  html += "<ul>";
  html += "<li>POST /start - Start task with JSON payload</li>";
  html += "<li>GET /status - Get current status</li>";
  html += "</ul>";
  html += "<h2>Example POST /start payload:</h2>";
  html += "<pre>{\"tasks\":[{\"address\":\"A1\",\"usns\":[\"USN001\",\"USN002\"]},{\"address\":\"B2\",\"usns\":[\"USN003\"]}]}</pre>";
  html += "<h2>UART Protocol:</h2>";
  html += "<p>Send: &lt;ADDRESS|USN1|USN2|...&gt;</p>";
  html += "<p>Receive: &lt;ADDRESS|USN1|USN2|...&gt;</p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleStartTask() {
  if (currentState != HALT) {
    server.send(400, "application/json", "{\"error\":\"Not in HALT state\"}");
    return;
  }
  
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body provided\"}");
    return;
  }
  
  String body = server.arg("plain");
  debugPrint("Received task: " + body);
  blinkLED(2); // Blink twice when HTTP POST /start received
  
  // Parse JSON
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  // Clear previous data
  taskData.clear();
  responseData.clear();
  pendingAddresses.clear();
  uartBuffer101 = "";
  uartBuffer102 = "";
  
  // Parse tasks array
  // Expected format: {"tasks":[{"address":"A1","usns":["USN1","USN2"]},{"address":"B2","usns":["USN3"]}]}
  JsonArray tasks = doc["tasks"].as<JsonArray>();

  bool has101 = false, has102 = false;
  for (JsonObject task : tasks) {
    String address = task["address"].as<String>();
    JsonArray usns = task["usns"].as<JsonArray>();

    std::vector<String> usnList;
    for (const char* usn : usns) {
      usnList.push_back(String(usn));
    }

    taskData[address] = usnList;
    debugPrint("Task added: Address " + address + " with " + String(usnList.size()) + " USNs");
    if (address == "RVU101") has101 = true;
    if (address == "RVU102") has102 = true;
  }
  // Always wait for both RVU101 and RVU102
  if (!has101) {
    taskData["RVU101"] = std::vector<String>();
    debugPrint("Task added: Address RVU101 (empty, forced wait)");
  }
  if (!has102) {
    taskData["RVU102"] = std::vector<String>();
    debugPrint("Task added: Address RVU102 (empty, forced wait)");
  }
  pendingAddresses.clear();
  pendingAddresses.push_back("RVU101");
  pendingAddresses.push_back("RVU102");

  if (taskData.empty()) {
    server.send(400, "application/json", "{\"error\":\"No valid tasks\"}");
    return;
  }

  server.send(200, "application/json", "{\"status\":\"Task accepted, transitioning to ACTIVE\"}");
  // Move to ACTIVE state only after response is sent and all logic is done
  transitionToActive();
}

void handleStatus() {
  StaticJsonDocument<1024> doc;
  
  doc["state"] = (currentState == HALT ? "HALT" : (currentState == ACTIVE ? "ACTIVE" : "WAIT"));
  doc["pending_addresses"] = pendingAddresses.size();
  doc["tasks_count"] = taskData.size();
  doc["responses_count"] = responseData.size();
  
  // List pending addresses
  JsonArray pending = doc.createNestedArray("pending");
  for (const String& addr : pendingAddresses) {
    pending.add(addr);
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

// ==================== STATE TRANSITIONS ====================
void transitionToHalt() {
  currentState = HALT;
  taskData.clear();
  responseData.clear();
  pendingAddresses.clear();
  uartBuffer101 = "";
  uartBuffer102 = "";
  debugPrint("==> Transitioned to HALT state");
}

void transitionToActive() {
  currentState = ACTIVE;
  debugPrint("==> Transitioned to ACTIVE state");
}

void transitionToWait() {
  currentState = WAIT;
  waitStartTime = millis();
  debugPrint("==> Transitioned to WAIT state");
  debugPrint("Waiting for responses from " + String(pendingAddresses.size()) + " addresses");
  debugPrint("WAIT timeout set to 120 seconds");
}

// ==================== UART COMMUNICATION ====================

// Send USNs to a specific address via UART
// Format: <ADDRESS|USN1|USN2|USN3|...>
void sendUSNsToAddress(const String& address, const std::vector<String>& usns) {
  blinkLEDHalfBrightness(4); // Blink four times at half brightness when sending via UART
  String message = "";
  message += START_MARKER;
  message += address;

  for (const String& usn : usns) {
    message += SEPARATOR;
    message += usn;
  }

  message += END_MARKER;

  // Send via SoftwareSerial
  softSerial.print(message);
  softSerial.flush();

  // Debug info (won't print during ACTIVE/WAIT states)
  // debugPrint("Sent to " + address + ": " + message);
}

// Process incoming UART data for both RVU101 (Serial) and RVU102 (Serial1)
void processUARTData() {
  // Only process if in WAIT state
  if (currentState != WAIT) {
    Serial.println("[processUARTData] Not in WAIT state, skipping. Current state: " + String(currentState));
    return;
  }

  Serial.println("[processUARTData] In WAIT state, checking for data...");

  // RVU101 (SoftwareSerial)
  if (taskData.count("RVU101")) {
    Serial.println("[processUARTData] RVU101 in task list, checking softSerial...");
    int available = softSerial.available();
    Serial.println("[processUARTData] softSerial.available() = " + String(available));
    
    while (softSerial.available() > 0) {
      char c = softSerial.read();
      Serial.print("[RVU101] Read char: ");
      Serial.println(c);
      
      if (c == START_MARKER) {
        Serial.println("[RVU101] START_MARKER detected!");
        receiving101 = true;
        uartBuffer101 = "";
      } else if (c == END_MARKER && receiving101) {
        Serial.println("[RVU101] END_MARKER detected!");
        Serial.println("[RVU101] Buffer contents: " + uartBuffer101);
        receiving101 = false;
        blinkLED(1); // Blink once when receiving from UART
        parseReceivedMessage(uartBuffer101);
        uartBuffer101 = "";
      } else if (receiving101) {
        uartBuffer101 += c;
        Serial.println("[RVU101] Buffer length: " + String(uartBuffer101.length()));
        if (uartBuffer101.length() > 1024) {
          Serial.println("[RVU101] Buffer overflow! Resetting...");
          uartBuffer101 = "";
          receiving101 = false;
        }
      }
    }
  } else {
    Serial.println("[processUARTData] RVU101 NOT in task list");
  }

  // RVU102 (SoftwareSerial2)
  if (taskData.count("RVU102")) {
    Serial.println("[processUARTData] RVU102 in task list, checking softSerial2...");
    int available = softSerial2.available();
    Serial.println("[processUARTData] softSerial2.available() = " + String(available));
    
    while (softSerial2.available() > 0) {
      char c = softSerial2.read();
      Serial.print("[RVU102] Read char: ");
      Serial.println(c);
      
      if (c == START_MARKER) {
        Serial.println("[RVU102] START_MARKER detected!");
        receiving102 = true;
        uartBuffer102 = "";
      } else if (c == END_MARKER && receiving102) {
        Serial.println("[RVU102] END_MARKER detected!");
        Serial.println("[RVU102] Buffer contents: " + uartBuffer102);
        receiving102 = false;
        blinkLED(1); // Blink once when receiving from UART
        parseReceivedMessage(uartBuffer102);
        uartBuffer102 = "";
      } else if (receiving102) {
        uartBuffer102 += c;
        Serial.println("[RVU102] Buffer length: " + String(uartBuffer102.length()));
        if (uartBuffer102.length() > 1024) {
          Serial.println("[RVU102] Buffer overflow! Resetting...");
          uartBuffer102 = "";
          receiving102 = false;
        }
      }
    }
  } else {
    Serial.println("[processUARTData] RVU102 NOT in task list");
  }
// End of processUARTData
}

// Parse received message and extract address and USNs
// Format: ADDRESS|USN1|USN2|USN3|...
void parseReceivedMessage(const String& message) {
  Serial.println("\n[parseReceivedMessage] ===== START PARSING =====");
  Serial.println("[parseReceivedMessage] Raw message: " + message);
  Serial.println("[parseReceivedMessage] Message length: " + String(message.length()));
  Serial.println("[parseReceivedMessage] Current state: " + String(currentState));
  
  if (currentState != WAIT) {
    Serial.println("[parseReceivedMessage] ERROR: Not in WAIT state, ignoring message!");
    return;  // Only process messages in WAIT state
  }
  
  Serial.println("[parseReceivedMessage] Pending addresses BEFORE parsing:");
  for (const String& addr : pendingAddresses) {
    Serial.println("  - " + addr);
  }
  
  std::vector<String> parts;
  int startIdx = 0;
  
  // Split by separator
  for (int i = 0; i <= message.length(); i++) {
    if (i == message.length() || message[i] == SEPARATOR) {
      if (i > startIdx) {
        parts.push_back(message.substring(startIdx, i));
      }
      startIdx = i + 1;
    }
  }
  
  Serial.println("[parseReceivedMessage] Split into " + String(parts.size()) + " parts");
  for (size_t i = 0; i < parts.size(); i++) {
    Serial.println("  Part[" + String(i) + "]: " + parts[i]);
  }
  
  if (parts.empty()) {
    Serial.println("[parseReceivedMessage] ERROR: No parts found, message empty!");
    return;
  }
  
  // First part is address
  String address = parts[0];
  Serial.println("[parseReceivedMessage] Extracted address: '" + address + "'");
  
  // Check if this address is in our pending list
  bool found = false;
  Serial.println("[parseReceivedMessage] Searching for address in pendingAddresses...");
  for (auto it = pendingAddresses.begin(); it != pendingAddresses.end(); ++it) {
    Serial.println("[parseReceivedMessage] Comparing '" + address + "' with '" + *it + "'");
    if (*it == address) {
      Serial.println("[parseReceivedMessage] MATCH FOUND! Removing from pending...");
      pendingAddresses.erase(it);
      found = true;
      break;
    }
  }
  
  if (!found) {
    Serial.println("[parseReceivedMessage] ERROR: Address '" + address + "' NOT found in pending list!");
    Serial.println("[parseReceivedMessage] This message will be IGNORED.");
    return;  // Unknown address, ignore
  }
  
  Serial.println("[parseReceivedMessage] Address '" + address + "' removed from pending.");
  
  // Rest are USNs
  std::vector<String> receivedUSNs;
  for (size_t i = 1; i < parts.size(); i++) {
    receivedUSNs.push_back(parts[i]);
  }
  
  Serial.println("[parseReceivedMessage] Extracted " + String(receivedUSNs.size()) + " USNs");
  
  responseData[address] = receivedUSNs;
  Serial.println("[parseReceivedMessage] Stored " + String(receivedUSNs.size()) + " USNs for address '" + address + "'");
  
  Serial.println("[parseReceivedMessage] Pending addresses AFTER parsing:");
  for (const String& addr : pendingAddresses) {
    Serial.println("  - " + addr);
  }
  Serial.println("[parseReceivedMessage] Pending count: " + String(pendingAddresses.size()));
  
  Serial.println("[parseReceivedMessage] Response data stored:");
  for (const auto& entry : responseData) {
    Serial.println("  Address: " + entry.first + " -> " + String(entry.second.size()) + " USNs");
  }
  
  Serial.println("[parseReceivedMessage] ===== END PARSING =====\n");
}

// ==================== HTTP CLIENT - SEND RESULTS ====================
void sendResultsToServer() {
  debugPrint("Sending results to server...");
  blinkLED(3); // Blink thrice when sending to /results

  String payload = buildJsonPayload(responseData);
  debugPrint("Payload: " + payload);

  WiFiClient client;
  HTTPClient http;

  String url = "http://" + String(RESULT_SERVER_IP) + ":" + String(RESULT_SERVER_PORT) + RESULT_ENDPOINT;
  debugPrint("URL: " + url);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    debugPrint("HTTP Response: " + String(httpCode));
    String response = http.getString();
    debugPrint("Response: " + response);
  } else {
    debugPrint("HTTP Error: " + http.errorToString(httpCode));
  }

  http.end();
}

String buildJsonPayload(const std::map<String, std::vector<String>>& data) {
  StaticJsonDocument<2048> doc;
  JsonArray results = doc.createNestedArray("results");
  
  for (const auto& entry : data) {
    JsonObject item = results.createNestedObject();
    item["address"] = entry.first;
    
    JsonArray usns = item.createNestedArray("usns");
    for (const String& usn : entry.second) {
      usns.add(usn);
    }
  }
  
  String output;
  serializeJson(doc, output);
  return output;
}