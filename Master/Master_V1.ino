#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>        
#include <FirebaseESP8266.h>    
#include <SoftwareSerial.h>   
#include <time.h>

ESP8266WebServer server(80);

// RS485 Pins
#define RS485_RX_PIN    D5
#define RS485_TX_PIN    D6
#define RS485_DERE_PIN  D7

// ultra version
// Object Instances
// Fixed: Pass pins to constructor here
SoftwareSerial rs485Serial(RS485_RX_PIN, RS485_TX_PIN); 
FirebaseData fbdoStream;
FirebaseAuth auth;
FirebaseConfig fbConfig;
FirebaseData fbdoWrite;

// Fixed: Renamed from 'Config' to 'DeviceConfig' to avoid SoftwareSerial conflict
struct DeviceConfig {
  char ap_ssid[20]; 
  char ap_pass[20];
  char sta_ssid[20];
  char sta_pass[20];  
  char api_key[50];
  char db_url[100];  
  char fb_email[64];      
  char fb_pass[32];        
  char fb_auth_mode[16];
};
DeviceConfig devData; 

// Function declarations
void ProcessCmd(String input);
void readSerial();
void setupWifi();
void setupServer();   
void setCORSHeaders();  
void streamCallback(StreamData data);  
void streamTimeoutCallback(bool timeout); 
void setupFb();
void Tcommunication(String jsonData);
void parseTime(String iso, uint8_t *out);
void initTime(); 
void readFromSlave();
void sendRealTimeToSlave();

bool isFbInitialStreamCompleated = false;

void setup(){
  Serial.begin(115200); 
  Serial.println("\n--- SMART SWITCH CONTROL ---");
  
  rs485Serial.begin(9600); 

  pinMode(RS485_DERE_PIN, OUTPUT);
  digitalWrite(RS485_DERE_PIN, LOW);

  EEPROM.begin(sizeof(DeviceConfig)); 
  EEPROM.get(0, devData);
  
  if(!(strlen(devData.ap_ssid) > 0)){
    strncpy(devData.ap_ssid, "msrv", sizeof(devData.ap_ssid) - 1);
    strncpy(devData.ap_pass, "12345678", sizeof(devData.ap_pass) - 1);
    EEPROM.put(0, devData);
    EEPROM.commit(); 
  }

  setupWifi();
  
  if(MDNS.begin("ssc")) {
    Serial.println("mDNS started: http://ssc.local");
  }
  
  setupServer();
  
  if(WiFi.status() == WL_CONNECTED){
    setupFb();
    initTime();
    sendRealTimeToSlave();
  } 
}

void loop(){
  readSerial();
  readFromSlave();
  server.handleClient();
  MDNS.update();

  // Keep Firebase stream alive
  if (Firebase.ready()) {
    Firebase.readStream(fbdoStream);

    if (!fbdoStream.httpConnected()) {
      Serial.println("⚠️ Stream disconnected. Reconnecting...");
      Firebase.beginStream(fbdoStream, "/Stream");
    }
  }
}

void readFromSlave() {
  // Check if there is at least something in the buffer
  if (rs485Serial.available() > 0) {
    // 1. Look for Start Byte
    if (rs485Serial.peek() == 0x7E) {  

      unsigned long startWait = millis();
      while (rs485Serial.available() < 7 && millis() - startWait < 50) {
        delay(1); 
      }

      if (rs485Serial.available() >= 7) {
        rs485Serial.read(); // Actually consume the 0x7E we peeked at
        uint8_t type = rs485Serial.read();
        uint8_t high = rs485Serial.read();
        uint8_t low  = rs485Serial.read();
        uint8_t chk  = rs485Serial.read();
        uint8_t end  = rs485Serial.read();
        uint8_t pad  = rs485Serial.read(); // Read the padding byte too
        uint8_t calcChk = type ^ high ^ low; // 3. Verify Checksum (Type ^ High ^ Low)
        if (type == 0x03 && chk == calcChk && end == 0x7F) {
          int receivedCode = (high << 8) | low;
          Serial.printf("📩 Received System Code: %d\n", receivedCode);
          String codeStr = String(receivedCode);

          if(receivedCode == 888) { sendRealTimeToSlave(); } // for real time

          if (codeStr[0] == '6' && codeStr.length() == 5) {
              int b = codeStr.substring(1, 3).toInt(); // Board ID
              int c = codeStr.substring(3, 4).toInt(); // Controller ID
              int a = codeStr.substring(4).toInt();    // Action/State

              Serial.println("✅ ACK Received");
              Serial.printf("b: %d, c: %d, a: %d\n", b, c, a);

              // Dynamic Path build cheyadam: "/BoardID/ControllerID/a"
              String path = "/" + String(b) + "/" + String(c) + "/a";

              if (Firebase.ready()) {
                  // Ikkada path "/12/3/a" format lo veltundi, value 'a' update avtundi
                  if (Firebase.setInt(fbdoWrite, path, a)) {
                      Serial.print("✅ Firebase updated at: ");
                      Serial.println(path);
                  } else {
                      Serial.println("❌ Firebase write failed");
                      Serial.println(fbdoWrite.errorReason());
                  }
              }
          }

        } else {
          Serial.println("❌ Data Corrupted or Checksum Failed");
        }
      }
    } else {
      // If the first byte isn't 0x7E, clear it to find the next valid start
      rs485Serial.read(); 
    }
  }
}

void ProcessCmd(String input){ 
  if(input == "data"){
    Serial.println("----- DeviceConfig Data -----");
    Serial.printf("Mode: %s\n", (WiFi.getMode() == WIFI_STA) ? "STA" : "AP");
    Serial.printf("IP: %s\n", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());

    Serial.printf("AP SSID      : %s\n", strlen(devData.ap_ssid) > 0 ? devData.ap_ssid : "undefined");
    Serial.printf("AP Password  : %s\n", strlen(devData.ap_pass) > 0 ? devData.ap_pass : "undefined");

    Serial.printf("STA SSID     : %s\n", strlen(devData.sta_ssid) > 0 ? devData.sta_ssid : "undefined");
    Serial.printf("STA Password : %s\n", strlen(devData.sta_pass) > 0 ? devData.sta_pass : "undefined");

    Serial.printf("API Key      : %s\n", strlen(devData.api_key) > 0 ? devData.api_key : "undefined");
    Serial.printf("DB URL       : %s\n", strlen(devData.db_url) > 0 ? devData.db_url : "undefined");

    Serial.printf("FB Email     : %s\n", strlen(devData.fb_email) > 0 ? devData.fb_email : "undefined");
    Serial.printf("FB Password  : %s\n", strlen(devData.fb_pass) > 0 ? devData.fb_pass : "undefined");
    Serial.printf("Auth Mode    : %s\n", strlen(devData.fb_auth_mode) > 0 ? devData.fb_auth_mode : "undefined");

    Serial.println("---------------- END ---------------");
    
  } else if(input == "clearEEPROM"){
      DeviceConfig empty = {}; 
      EEPROM.put(0, empty); 
      EEPROM.commit(); 
      Serial.println("**EEPROM clear - Restarting**");
      delay(500);
      ESP.restart();
      
  } else if(input == "restart"){ 
    ESP.restart();
  } else if(input == "time"){
    sendRealTimeToSlave();
    delay(1000);
  } else if(input == "fb"){
    Serial.println("----- Firebase Status -----");
    // 🔹 WiFi Status
    Serial.printf("WiFi: %s\n", (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected");
    // 🔹 Firebase Ready
    Serial.printf("Firebase Ready: %s\n", Firebase.ready() ? "YES" : "NO");
    // 🔹 Token Status
    Serial.printf("Auth Token: %s\n", (fbConfig.signer.tokens.status == token_status_ready) ? "Valid" : "Not Ready");
    // 🔹 Token Expiry
    Serial.printf("Token Expires In: %d sec\n", fbConfig.signer.tokens.expires);
    // 🔹 Stream Status
    Serial.printf("Stream Active: %s\n", fbdoStream.httpConnected() ? "YES" : "NO");
    // 🔹 Last Error (if any)
    if(fbdoStream.httpCode() != 200){
      Serial.printf("Last Error Code: %d\n", fbdoStream.httpCode());
      Serial.printf("Error Reason: %s\n", fbdoStream.errorReason().c_str());
    } else {
      Serial.println("No Stream Errors");
    }
    // 🔹 Heap (important for Firebase stability)
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("----------- END -----------");
  } else if(input == "autoOF"){
    String jsonString = "{\"t\":1,\"o\":13,\"b\":10,\"c\":2,\"f\":0,\"m\":4,}";
    Tcommunication(jsonString);
  }
}

void readSerial(){
  if(Serial.available() > 0){
    String input = Serial.readStringUntil('\n');  
    input.trim();  
    if(input.length() > 0) ProcessCmd(input);
  }
}

void setupWifi(){ 
  if(strlen(devData.sta_ssid) > 0){
    WiFi.mode(WIFI_STA);
    WiFi.begin(devData.sta_ssid, devData.sta_pass); 
    Serial.println("Connecting to WiFi...");
    
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis() - start < 15000){
      delay(500);
      Serial.print("."); 
    }
  }

  if(WiFi.status() == WL_CONNECTED){
    Serial.println("\n✅ Connected IP: " + WiFi.localIP().toString());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(devData.ap_ssid, devData.ap_pass);
    Serial.println("\n✅ AP Started: " + WiFi.softAPIP().toString());
  }
}
 
void setCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void setupServer(){
  server.on("/", [](){
    setCORSHeaders();
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  }); 

  // Route to fetch current configuration
  server.on("/ssc_config", HTTP_GET, []() {
    setCORSHeaders();
    
    // Ensure we have the latest data from EEPROM
    EEPROM.get(0, devData);
    StaticJsonDocument<1024> doc;
    doc["ap_ssid"] = devData.ap_ssid;
    doc["ap_pass"] = devData.ap_pass;
    doc["sta_ssid"] = devData.sta_ssid;
    doc["sta_pass"] = devData.sta_pass;
    doc["api_key"] = devData.api_key;
    doc["db_url"] = devData.db_url;
    doc["fb_email"] = devData.fb_email;
    doc["fb_pass"] = devData.fb_pass;
    doc["fb_auth_mode"] = devData.fb_auth_mode;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });

  // Handle pre-flight OPTIONS request for CORS
  server.on("/ssc_config", HTTP_OPTIONS, []() {
    setCORSHeaders();
    server.send(200, "text/plain", "OK");
  });

  server.on("/ssc_config", HTTP_POST, []() {
    setCORSHeaders();
    
    StaticJsonDocument<1024> doc; // Increased size to handle the full struct
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    
    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    // Load existing data first to ensure we don't wipe unrelated fields
    EEPROM.get(0, devData);

    // Safely update each field if it exists in the JSON
    if(doc.containsKey("ap_ssid"))  strncpy(devData.ap_ssid,  doc["ap_ssid"], 19);
    if(doc.containsKey("ap_pass"))  strncpy(devData.ap_pass,  doc["ap_pass"], 19);
    if(doc.containsKey("sta_ssid")) strncpy(devData.sta_ssid, doc["sta_ssid"], 19);
    if(doc.containsKey("sta_pass")) strncpy(devData.sta_pass, doc["sta_pass"], 19);
    if(doc.containsKey("api_key"))  strncpy(devData.api_key,  doc["api_key"], 49);
    if(doc.containsKey("db_url"))   strncpy(devData.db_url,   doc["db_url"], 99);
    if(doc.containsKey("fb_email")) strncpy(devData.fb_email, doc["fb_email"], 63);
    if(doc.containsKey("fb_pass"))  strncpy(devData.fb_pass,  doc["fb_pass"], 31);
    if(doc.containsKey("fb_auth_mode")) strncpy(devData.fb_auth_mode, doc["fb_auth_mode"], 15);
    
    // Ensure null termination for safety
    devData.ap_ssid[19] = devData.ap_pass[19] = '\0';
    devData.sta_ssid[19] = devData.sta_pass[19] = '\0';
    devData.api_key[49] = '\0';
    devData.db_url[99] = '\0';
    devData.fb_email[63] = '\0';
    devData.fb_pass[31] = '\0';
    devData.fb_auth_mode[15] = '\0';

    // Save to EEPROM
    EEPROM.put(0, devData);
    bool success = EEPROM.commit();
    
    if(success) {
      Serial.println("✅ All config saved to EEPROM. Restarting...");
      server.send(200, "application/json", "{\"status\":\"saved\",\"action\":\"restarting\"}");
      delay(1000); 
      ESP.restart();
    } else {
      server.send(500, "application/json", "{\"error\":\"EEPROM commit failed\"}");
    }
  });

  server.begin();
  Serial.println("🌐 Server listening on /ssc_config");
}

void streamCallback(StreamData data) {
  // If this isn't the first time, process the data
  if (isFbInitialStreamCompleated) {
    if (data.dataType() == "json") {
      String out = data.jsonString(); 
      
      Serial.println("📡 Stream → RS485: " + out);
      Tcommunication(out);
    }
  } 
  else {
    // This was the initial data burst. 
    // We skip it and set the flag to true for all future updates.
    isFbInitialStreamCompleated = true; 
    Serial.println("✅ Initial stream ignored. Ready for updates.");
  }
}

void streamTimeoutCallback(bool timeout) {
  if(timeout) Serial.println("⚠️ Stream timeout");
}

void setupFb() {
  // 1. Basic Validation: Only proceed if we have a DB URL and API Key
  if (strlen(devData.db_url) == 0 || strlen(devData.api_key) == 0) {
    Serial.println("⚠️ Firebase aborted: Missing DB URL or API Key in devData");
    return;
  }

  Serial.println("🔐 Initializing Firebase with DeviceConfig...");

  // 2. Assign values directly from your devData struct
  fbConfig.host = devData.db_url;
  fbConfig.api_key = devData.api_key;
  
  auth.user.email = devData.fb_email;
  auth.user.password = devData.fb_pass;

  // 3. Initialize Firebase
  Firebase.begin(&fbConfig, &auth);
  Firebase.reconnectWiFi(true);

  // Optional: Set buffer sizes to save RAM on ESP8266
  fbdoStream.setBSSLBufferSize(4096, 1024); 

  // 4. Start the Stream
  if (Firebase.beginStream(fbdoStream, "/Stream")) {
    Serial.println("✅ Firebase Stream Active on /Stream");
    Firebase.setStreamCallback(fbdoStream, streamCallback, streamTimeoutCallback);
  } else {
    Serial.printf("❌ Firebase Stream Error: %s\n", fbdoStream.errorReason().c_str());
  }
}

void parseTime(String iso, uint8_t *out) {
  if (iso.length() < 16) {
    memset(out, 0, 5);
    return;
  }

  out[0] = iso.substring(2, 4).toInt();  // year (26)
  out[1] = iso.substring(5, 7).toInt();  // month
  out[2] = iso.substring(8,10).toInt();  // day
  out[3] = iso.substring(11,13).toInt(); // hour
  out[4] = iso.substring(14,16).toInt(); // minute
}

void Tcommunication(String json) {
  // this function is from esp master
  StaticJsonDocument<256> doc;
  deserializeJson(doc, json);    

  if (doc.containsKey("a")) { 
    Serial.println("Key 'a' found -> Controller data");
    uint8_t packet[8];
    packet[0] = 0x7E;  // 🔹 1. START BYTE
    packet[1] = 0x01; // 🔹 2. DATA TYPE (controller = 1)
    packet[2] = doc["b"] | 0; // 🔹 3. b value
    packet[3] = doc["c"] | 0; // 🔹 4. c value
    packet[4] = doc["s"] | 0; // 🔹 5. s value
    uint8_t checksum = 0; // 🔹 6. CHECKSUM (XOR from index 1 → 4)
    for (int i = 1; i <= 4; i++) {
      checksum ^= packet[i];
    }
    packet[5] = checksum;
    packet[6] = 0x7F; // 🔹 7. END BYTE
    packet[7] = 0x00; // 🔹 8. (optional padding / reserved)
    // 🔥 SEND via RS485
    digitalWrite(RS485_DERE_PIN, HIGH);
    delayMicroseconds(100);
    rs485Serial.write(packet, 8);
    rs485Serial.flush();
    delay(2);
    digitalWrite(RS485_DERE_PIN, LOW);
    // 🔍 Debug print
    Serial.print("📤 Sent Packet: ");
    for (int i = 0; i < 8; i++) {
      Serial.print(packet[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  } 
  else if (doc.containsKey("t")) {
    Serial.println("Key 't' found -> Controller Timer data");

    uint8_t packet[20];
    int idx = 0;

    uint8_t mode = doc["m"] | 0;
    uint8_t t    = doc["t"] | 0; // timer state

    packet[idx++] = 0x7E;
    packet[idx++] = 0x02;         // TYPE = Timer
    packet[idx++] = mode;
    packet[idx++] = doc["b"] | 0;
    packet[idx++] = doc["c"] | 0;
    packet[idx++] = t;            // active state

    // MODE 1 → ONCE (single action)
    if (mode == 1) {
      uint8_t action = doc.containsKey("n") ? 1 : 0; // 1=ON, 0=OFF
      String timeStr = doc.containsKey("n") ? (doc["n"] | "") : (doc["f"] | "");
      packet[idx++] = action;
      packet[idx++] = timeStr.substring(2,  4).toInt();  // YY
      packet[idx++] = timeStr.substring(5,  7).toInt();  // MM
      packet[idx++] = timeStr.substring(8, 10).toInt();  // DD
      packet[idx++] = timeStr.substring(11,13).toInt();  // HH
      packet[idx++] = timeStr.substring(14,16).toInt();  // MIN
    }

    // MODE 2 → DAILY
    else if (mode == 2) {
      String n = doc["n"] | "";
      String f = doc["f"] | "";
      packet[idx++] = n.substring(0,2).toInt();  // ON hour
      packet[idx++] = n.substring(3,5).toInt();  // ON min
      packet[idx++] = f.substring(0,2).toInt();  // OFF hour
      packet[idx++] = f.substring(3,5).toInt();  // OFF min
    }

    // MODE 3 → CYCLE (both n and f in minutes)
    else if (mode == 3) {
      packet[idx++] = doc["n"] | 0;  // ON duration minutes
      packet[idx++] = doc["f"] | 0;  // OFF duration minutes
      packet[idx++] = doc["r"] | 0;  // repeat count
    }

    // MODE 4 → DELAYED SINGLE ACTION
    else if (mode == 4) {
      uint8_t action = doc.containsKey("n") ? 1 : 0; // 1=ON after off, 0=OFF after on
      uint8_t delay_min = doc.containsKey("n") ? (doc["n"] | 0) : (doc["f"] | 0);
      packet[idx++] = action;
      packet[idx++] = delay_min;
    }

    // CHECKSUM
    uint8_t checksum = 0;
    for (int i = 1; i < idx; i++) checksum ^= packet[i];
    packet[idx++] = checksum;
    packet[idx++] = 0x7F;

    // SEND
    digitalWrite(RS485_DERE_PIN, HIGH);
    delayMicroseconds(100);
    rs485Serial.write(packet, idx);
    rs485Serial.flush();
    delay(2);
    digitalWrite(RS485_DERE_PIN, LOW);

    Serial.print("📤 Timer Packet: ");
    for (int i = 0; i < idx; i++) { Serial.print(packet[i], HEX); Serial.print(" "); }
    Serial.println();
  }
  // else if (doc.containsKey("o")){
  //   Serial.print("Test String: ");
  //   Serial.println(json);

  //   uint8_t packet[8];
  //   int idx = 0;

  //   packet[idx++] = 0x7E;           // 1. START BYTE
  //   packet[idx++] = 0x04;           // 2. DATA TYPE (test = 4)
  //   packet[idx++] = doc["o"] | 0;   // 3. o value
  //   packet[idx++] = doc["b"] | 0;   // 4. b value
  //   packet[idx++] = doc["c"] | 0;   // 5. c value
  //   packet[idx++] = doc["f"] | 0;   // 6. f value

  //   // CHECKSUM (XOR from index 1 → 5)
  //   uint8_t checksum = 0;
  //   for (int i = 1; i < idx; i++) {
  //     checksum ^= packet[i];
  //   }
  //   packet[idx++] = checksum;
  //   packet[idx++] = 0x7F;           // END BYTE

  //   // SEND via RS485
  //   digitalWrite(RS485_DERE_PIN, HIGH);
  //   delayMicroseconds(100);
  //   rs485Serial.write(packet, idx);
  //   rs485Serial.flush();
  //   delay(2);
  //   digitalWrite(RS485_DERE_PIN, LOW);

  //   Serial.print("📤 Sent 'o' Packet: ");
  //   for (int i = 0; i < idx; i++) {
  //     Serial.print(packet[i], HEX);
  //     Serial.print(" ");
  //   }
  //   Serial.println();
  // }
  else { 
    Serial.println("Key 'a' not found.");
  }
}

void initTime() { 
  configTime(5.5 * 3600, 0, "pool.ntp.org"); // IST
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    now = time(nullptr);
  }
}
 
void sendRealTimeToSlave(){
  // 888 code - "requesting real time" 
  delay(666);
  Serial.println("Slave requesting RealTime");
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  uint8_t packet[12];
  int idx = 0;
  // 🔹 Extract time
  uint8_t yy  = timeinfo->tm_year % 100;
  uint8_t mm  = timeinfo->tm_mon + 1;
  uint8_t dd  = timeinfo->tm_mday;
  uint8_t hh  = timeinfo->tm_hour;
  uint8_t min = timeinfo->tm_min;
  uint8_t sec = timeinfo->tm_sec;
  // 🔹 ✅ Print readable time
  char timeStr[40];
  sprintf(timeStr, "%02d:%02d:%02d T %02d:%02d:%02d",
          yy, mm, dd, hh, min, sec);
  Serial.println("🕒 Current Time: " + String(timeStr));
  packet[idx++] = 0x7E;   // 🔹 1. START BYTE
  packet[idx++] = 0x03;  // 🔹 2. DATA TYPE
  packet[idx++] = yy;  // 🔹 3–8. TIME DATA
  packet[idx++] = mm;
  packet[idx++] = dd;
  packet[idx++] = hh;
  packet[idx++] = min;
  packet[idx++] = sec;
  uint8_t checksum = 0;   // 🔹 9. CHECKSUM
  for (int i = 1; i < idx; i++) {
    checksum ^= packet[i];
  }
  packet[idx++] = checksum;
  packet[idx++] = 0x7F;  // 🔹 10. END BYTE
  digitalWrite(RS485_DERE_PIN, HIGH);  // 🔥 SEND via RS485
  delayMicroseconds(100);
  rs485Serial.write(packet, idx);
  rs485Serial.flush();
  delay(2);
  digitalWrite(RS485_DERE_PIN, LOW);
  Serial.print("📤 Sent Time Packet: ");  // 🔍 Debug packet
  for (int i = 0; i < idx; i++) {
    Serial.print(packet[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}








