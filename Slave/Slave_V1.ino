#include <SoftwareSerial.h>
#include <EEPROM.h>

// RS485 Pins for Pro Mini
#define RS485_RX_PIN    11
#define RS485_TX_PIN    10
#define RS485_DERE_PIN  12
#define SYNC_INTERVAL_MS  600000UL  // 10 minutes

const uint8_t BOARD_ID    = 6;
const bool DEFAULT_FEEDBACK_VALUE = HIGH; 
const uint8_t CONTROLLER_TYPE = 1; // 1 = one-way, 2 = two-way

SoftwareSerial rs485(RS485_RX_PIN, RS485_TX_PIN);
uint8_t g_yy, g_mm, g_dd, g_hh, g_min, g_sec;
bool g_timeReady = false;
unsigned long lastTickMillis  = 0;
unsigned long lastSyncRequest = 0;
const uint8_t RELAY_COUNT = 6;
const uint8_t RELAY_PINS[RELAY_COUNT]    = {2, 3, 4, 5, 6, 7};
const uint8_t FEEDBACK_PINS[RELAY_COUNT] = {A0, A1, A2, A3, A4, A5};
bool prevFeedbackState[RELAY_COUNT];
bool feedbackInitialized = false;
bool relayState[RELAY_COUNT] = {false}; // track current relay states

unsigned long cycleLastMillis[6] = {0};
struct TimerEntry {
  uint8_t mode;       // 1=once, 2=daily, 3=cycle

  uint8_t ONyear;
  uint8_t ONmonth;
  uint8_t ONdate;
  uint8_t ONhour;
  uint8_t ONmin;

  uint8_t OFFyear;
  uint8_t OFFmonth;
  uint8_t OFFdate;
  uint8_t OFFhour;
  uint8_t OFFmin;

  uint8_t cycle;      // repeat count for mode 3
  bool    active;     // 1=active, 0=deactivated

  // uint8_t cyclePhase;       // mode3: 0=ON phase, 1=OFF phase
  // unsigned long cycleLastMillis;  // mode3/4: millis timestamp of last phase start
};

struct SlaveConfig {
  bool        relayStates[6];   // existing key
  TimerEntry  timer[6];         // R1→timer[0] ... R6→timer[5]
};

SlaveConfig slaveConfig;

void syncTime(uint8_t* buffer);
void tickClock();
void checkSerialCommands();
void sendSystemCode(String codeStr);
void handlePacket();
void initFeedback();
void readFeedback();
void initRelayPins();
void controlRelay(uint8_t board, uint8_t ctrl, uint8_t state);
void restoreRelayStates();
void saveRelayStates();
void executeTimers(); 

void setup() {
  Serial.begin(115200);
  rs485.begin(9600);

  pinMode(RS485_DERE_PIN, OUTPUT);
  digitalWrite(RS485_DERE_PIN, LOW); // Set to RECEIVE mode

  Serial.println("--- RS485 SLAVE READY ---");

  sendSystemCode("888");
  initFeedback();   initRelayPins();    restoreRelayStates();

}

void loop() {
  if (rs485.available() > 0) {
    if (rs485.read() == 0x7E) handlePacket();
  }
  checkSerialCommands();
  tickClock();  
  readFeedback();
}

void checkSerialCommands() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim(); // Remove spaces or \r
    if (input == "time") {
      if (!g_timeReady) {
        Serial.println("⚠️ Time not synced yet");
      } else {
        char timeStr[40];
        sprintf(timeStr, "20%02d-%02d-%02d %02d:%02d:%02d",
                g_yy, g_mm, g_dd, g_hh, g_min, g_sec);
        Serial.print("🕒 Current Time: ");
        Serial.println(timeStr);
      }
    }
    // Check if input starts with "c-"
    if (input.startsWith("c-")) {
      // Extract the code (everything after "c-")
      String codeValue = input.substring(2); 
      
      Serial.print("🛠 Command detected. Sending Code: ");
      Serial.println(codeValue);

      // Call the RS485 send function
      sendSystemCode(codeValue);
    }
    if (input.startsWith("t-")) {
      uint8_t ctrl = input.substring(2).toInt();
      if (ctrl < 1 || ctrl > RELAY_COUNT) {
        Serial.println("⚠️ Invalid controller number");
        return;
      }
      uint8_t slot = ctrl - 1;
      TimerEntry& t = slaveConfig.timer[slot];

      Serial.print("⏱ Timer R"); Serial.print(ctrl);
      Serial.print(" | Active: "); Serial.print(t.active ? "YES" : "NO");
      Serial.print(" | Mode: "); Serial.println(t.mode);

      if (!t.active) return;

      if (t.mode == 1) {
        Serial.print("  Action: "); Serial.println(t.cycle ? "ON" : "OFF");
        Serial.print("  Time: 20"); Serial.print(t.ONyear); Serial.print("-");
        Serial.print(t.ONmonth); Serial.print("-"); Serial.print(t.ONdate);
        Serial.print(" "); Serial.print(t.ONhour); Serial.print(":"); Serial.println(t.ONmin);
      }
      else if (t.mode == 2) {
        Serial.print("  ON:  "); Serial.print(t.ONhour); Serial.print(":"); Serial.println(t.ONmin);
        Serial.print("  OFF: "); Serial.print(t.OFFhour); Serial.print(":"); Serial.println(t.OFFmin);
      }
      else if (t.mode == 3) {
        Serial.print("  ON dur:  "); Serial.print(t.ONmin); Serial.println(" min");
        Serial.print("  OFF dur: "); Serial.print(t.OFFmin); Serial.println(" min");
        Serial.print("  Repeat:  "); Serial.println(t.cycle);
      }
      else if (t.mode == 4) {
        Serial.print("  Action: "); Serial.println(t.cycle ? "ON after OFF" : "OFF after ON");
        Serial.print("  Delay:  "); Serial.print(t.ONmin); Serial.println(" min");
      }
    }
  }
}

void syncTime(uint8_t* buffer) {
  g_yy  = buffer[1];
  g_mm  = buffer[2];
  g_dd  = buffer[3];
  g_hh  = buffer[4];
  g_min = buffer[5];
  g_sec = buffer[6];
  g_timeReady = true;

  char timeStr[40];
  sprintf(timeStr, "20%02d-%02d-%02d %02d:%02d:%02d",
          g_yy, g_mm, g_dd, g_hh, g_min, g_sec);
  Serial.print("🕒 Time Synced: ");
  Serial.println(timeStr);
}

void tickClock() {
  if (!g_timeReady) return;
  unsigned long now = millis();
  // --- Tick every 1 second ---
  if (now - lastTickMillis >= 1000) {
    lastTickMillis = now;

    g_sec++;
    if (g_sec >= 60) { g_sec = 0; g_min++; }
    if (g_min >= 60) { g_min = 0; g_hh++;  }
    if (g_hh  >= 24) { g_hh  = 0; g_dd++;  }
    // Note: skipping month/year overflow — master re-sync handles accuracy
    executeTimers();
  }
  // --- Request re-sync every 10 minutes ---
  if (now - lastSyncRequest >= SYNC_INTERVAL_MS) {
    lastSyncRequest = now;
    Serial.println("🔄 Requesting time re-sync from master...");
    sendSystemCode("888");
  }
}

void sendSystemCode(String codeStr) {
  int code = codeStr.toInt(); // String "100" becomes int 100
  
  uint8_t packet[7];
  packet[0] = 0x7E;          // Start Byte
  packet[1] = 0x03;          // Type 3: System/ACK Communication
  
  // High byte and Low byte (Split 16-bit int into two 8-bit bytes)
  packet[2] = (code >> 8) & 0xFF; 
  packet[3] = code & 0xFF;
  
  // Checksum (XOR Type, High Byte, Low Byte)
  uint8_t checksum = 0;
  for (int i = 1; i <= 3; i++) {
    checksum ^= packet[i];
  }
  packet[4] = checksum;
  packet[5] = 0x7F;          // End Byte
  packet[6] = 0x00;          // Padding

  // RS485 Send Logic
  digitalWrite(RS485_DERE_PIN, HIGH); // Switch to Transmit
  delayMicroseconds(100);
  
  rs485.write(packet, 7);
  rs485.flush();
  
  delayMicroseconds(100);
  digitalWrite(RS485_DERE_PIN, LOW);  // Switch back to Receive
  
  Serial.print("📤 Sent System Code: ");
  Serial.println(code);
}

void handlePacket() {
  uint8_t buffer[20];
  uint8_t idx = 0;
  delay(30);
  while (rs485.available() > 0 && idx < 20) {
    uint8_t b = rs485.read();
    if (b == 0x7F) break;  // End byte — stop, don't store it
    buffer[idx++] = b;
  }
  // Now buffer = [TYPE][DATA...][CHECKSUM]// idx = total bytes including checksum
  if (idx < 2) {
    Serial.println("❌ Packet too short");
    return;
  }
  uint8_t type             = buffer[0];
  uint8_t receivedChecksum = buffer[idx - 1];  // ✅ Last byte is checksum
  // XOR from TYPE to last DATA byte (exclude checksum itself)
  uint8_t calculatedChecksum = 0;
  for (int i = 0; i < idx - 1; i++) {
    calculatedChecksum ^= buffer[i];
  }
  if (calculatedChecksum != receivedChecksum) {
    Serial.println("❌ Checksum Error!");
    Serial.print("Expected: "); Serial.print(calculatedChecksum, HEX);
    Serial.print(" Got: "); Serial.println(receivedChecksum, HEX);
    return;
  }

  Serial.print("✅ Packet [Type 0x0");  Serial.print(type, HEX);  Serial.println("]");

  if (type == 0x01) {
    // [TYPE][B][C][S][CHKSUM] → idx=5
    Serial.print("Board: "); Serial.print(buffer[1]);
    Serial.print(" | Ctrl: "); Serial.print(buffer[2]);
    Serial.print(" | State: "); Serial.println(buffer[3]);
    controlRelay(buffer[1], buffer[2], buffer[3]); 
  }

  else if (type == 0x02) {
    // [TYPE][MODE][B][C][ACTIVE][...data...][CHKSUM]
    uint8_t mode   = buffer[1];
    uint8_t board  = buffer[2];
    uint8_t ctrl   = buffer[3];
    uint8_t active = buffer[4];

    Serial.print("⏱ Timer | Mode: "); Serial.print(mode);
    Serial.print(" | Board: ");       Serial.print(board);
    Serial.print(" | Ctrl: ");        Serial.print(ctrl);
    Serial.print(" | Active: ");      Serial.println(active);

    if (board != BOARD_ID) { Serial.println("⚠️ Not for this board"); return; }
    if (ctrl < 1 || ctrl > RELAY_COUNT) { Serial.println("⚠️ Invalid ctrl"); return; }

    uint8_t slot = ctrl - 1;

    // If active=0 → deactivate and save
    if (active == 0) {
      slaveConfig.timer[slot].active = false;
      saveRelayStates();
      Serial.println("🗑 Timer deactivated");
      return;
    }

    // Fill timer slot based on mode
    slaveConfig.timer[slot].mode   = mode;
    slaveConfig.timer[slot].active = true;

    if (mode == 1) {
      // [ACTION][YY][MM][DD][HH][MIN]
      slaveConfig.timer[slot].ONyear  = buffer[5];  // action stored in ONyear: 1=ON 0=OFF
      slaveConfig.timer[slot].ONmonth = buffer[6];
      slaveConfig.timer[slot].ONdate  = buffer[7];
      slaveConfig.timer[slot].ONhour  = buffer[8];
      slaveConfig.timer[slot].ONmin   = buffer[9];
      // Serial.print("Mode1 | Action: "); Serial.print(buffer[5] ? "ON" : "OFF");
      // Serial.printf(" | Time: 20%02d-%02d-%02d %02d:%02d\n", buffer[6], buffer[7], buffer[8], buffer[9], buffer[10]);
      Serial.print("Mode1 | Action: "); Serial.print(buffer[5] ? "ON" : "OFF");
      Serial.print(" | Time: 20"); Serial.print(buffer[6]); Serial.print("-");
      Serial.print(buffer[7]); Serial.print("-"); Serial.print(buffer[8]);
      Serial.print(" "); Serial.print(buffer[9]); Serial.print(":"); Serial.println(buffer[10]);

      // fix: shift — action=buffer[5], YY=buffer[6]...MIN=buffer[10]
      slaveConfig.timer[slot].ONyear  = buffer[6];
      slaveConfig.timer[slot].ONmonth = buffer[7];
      slaveConfig.timer[slot].ONdate  = buffer[8];
      slaveConfig.timer[slot].ONhour  = buffer[9];
      slaveConfig.timer[slot].ONmin   = buffer[10];
      slaveConfig.timer[slot].cycle   = buffer[5]; // reuse cycle field to store action(1/0)
    }
    else if (mode == 2) {
      // [ONhour][ONmin][OFFhour][OFFmin]
      slaveConfig.timer[slot].ONhour  = buffer[5];
      slaveConfig.timer[slot].ONmin   = buffer[6];
      slaveConfig.timer[slot].OFFhour = buffer[7];
      slaveConfig.timer[slot].OFFmin  = buffer[8];
      // Serial.printf("Mode2 | ON: %02d:%02d OFF: %02d:%02d\n", buffer[5], buffer[6], buffer[7], buffer[8]);
      Serial.print("Mode2 | ON: "); Serial.print(buffer[5]); Serial.print(":");
      Serial.print(buffer[6]); Serial.print(" OFF: "); Serial.print(buffer[7]);
      Serial.print(":"); Serial.println(buffer[8]);
    }
    // else if (mode == 3) {
    //   // [ONmin][OFFmin][repeat]
    //   slaveConfig.timer[slot].ONmin  = buffer[5];  // ON duration minutes
    //   slaveConfig.timer[slot].OFFmin = buffer[6];  // OFF duration minutes
    //   slaveConfig.timer[slot].cycle  = buffer[7];  // repeat count
    //   slaveConfig.timer[slot].cyclePhase     = 0;
    //   slaveConfig.timer[slot].cycleLastMillis = millis();
    //   controlRelay(BOARD_ID, ctrl, 1); // start by turning ON immediately
    //   startMode3(slot);
    //   Serial.print("Mode3 | ON_dur: "); Serial.print(buffer[5]);
    //   Serial.print(" OFF_dur: "); Serial.print(buffer[6]);
    //   Serial.print(" Repeat: "); Serial.println(buffer[7]);
    // }
    else if (mode == 4) {
      // [ACTION][DELAY_MIN]
      slaveConfig.timer[slot].cycle  = buffer[5]; // reuse: 1=ON after off, 0=OFF after on
      slaveConfig.timer[slot].ONmin  = buffer[6]; // delay in minutes
      cycleLastMillis[slot] = millis(); // start countdown now
      Serial.print("Mode4 | Action: "); Serial.print(buffer[5] ? "ON after OFF" : "OFF after ON");
      Serial.print(" | Delay: "); Serial.print(buffer[6]); Serial.println(" min");
    }

    saveRelayStates();
    Serial.println("✅ Timer saved");
  }

  else if (type == 0x03) {
    // [TYPE][YY][MM][DD][HH][MIN][SEC][CHKSUM] → idx=8
    if (idx < 8) {  Serial.println("❌ Invalid Time Packet"); return; }
    char timeStr[40];
    sprintf(timeStr, "20%02d-%02d-%02d %02d:%02d:%02d",
            buffer[1], buffer[2], buffer[3],
            buffer[4], buffer[5], buffer[6]);
    Serial.print("🕒 Time Sync: ");
    Serial.println(timeStr); 
    syncTime(buffer);
  }
  // else if (type == 0x04) {
  //   // [TYPE][O][B][C][F][CHKSUM]
  //   Serial.print("o: "); Serial.print(buffer[1]);
  //   Serial.print(" | Board: "); Serial.print(buffer[2]);
  //   Serial.print(" | Ctrl: "); Serial.print(buffer[3]);
  //   Serial.print(" | f: "); Serial.println(buffer[4]);
  // }
}
  
// first of all make a struct like
// {
//   bool relayStates[6]; // OLD KEY
//   timer: {
//     R1: { // controler(c)
//       Mode, // 1-once 2-daily 3-cycle
//       ONmin,  OFFmin, // two digit numer
//       ONhour, OFFhour, // two digit numer
//       ONdate, OFFdate, // two digit numer
//       ONmonth,  OFFmonth, // two digit numer
//       ONyear, OFFyear, // two digit numer
//       cycle, // two digit numer
//     }
//     R2:{} R3:{} R4:{}  R5:{}  R6:{}
//     ex - 
//     data recived form mode 1 - {"b":10,"c":1,"m":1,"n":"2026-04-02T14:40","t":1} - n means on timer
//     {"b":10,"c":1,"f":"2026-04-02T14:43","m":1,"t":1} - f means off timer
//     data recived from mode 2 - {"b":10,"c":1,"f":"18:39","m":2,"n":"17:39","t":1}
//     data recived form mode 3 - {"b":10,"c":1,"f":"YYYY-MM-DDTHH:25","m":3,"n":"YYYY-MM-DDTHH:57","r":3,"t":1}
//     data recived form mode 4 - "{\"t\":1,\"n\":13,\"b\":10,\"c\":2,\"f\":0,\"m\":4}"; 
//     b- board, c- contorler (relay), f- off time, n- on time, m- mode, r- repeate times (cycles), t- timer state(1 mean active, 0 mean diactivate)

//   }

// }


void initFeedback() {
  // Summary of initFeedback()
  // Type    DefaultHIGH       DefaultLOW
  // Type1   INPUT_PULLUP✅    External pull-down resistor needed ⚠️
  // Type2   INPUT✅           INPUT ✅
  for (int i = 0; i < RELAY_COUNT; i++) {

    if (CONTROLLER_TYPE == 1) {
      // Physical switch — need pull resistor to hold default state
      if (DEFAULT_FEEDBACK_VALUE == HIGH) {
        pinMode(FEEDBACK_PINS[i], INPUT_PULLUP);  // floating → stays HIGH naturally
      } else {
        pinMode(FEEDBACK_PINS[i], INPUT);  // Pro Mini has no INPUT_PULLDOWN
        // ⚠️ For LOW default on type 1 — you need external pull-down resistor on hardware
      }
    } else {
      // Optocoupler — always drives pin actively, no floating issue
      pinMode(FEEDBACK_PINS[i], INPUT);
    }

    prevFeedbackState[i] = digitalRead(FEEDBACK_PINS[i]);
  }
  feedbackInitialized = true;
}

void initRelayPins(){
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
    relayState[i] = false;
  }
}

void readFeedback() { 
  if (!feedbackInitialized) return;
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool currentState = digitalRead(FEEDBACK_PINS[i]);
    if (currentState != prevFeedbackState[i]) {
      prevFeedbackState[i] = currentState;
      int ctrlNumber = i + 1;                          // A0→1, A1→2 ...
      int stateValue = (currentState != DEFAULT_FEEDBACK_VALUE) ? 1 : 0;
      // Build code: 6 + ID(2digit) + ctrl(1digit) + state(1digit)
      char code[8];
      sprintf(code, "6%02d%d%d", BOARD_ID, ctrlNumber, stateValue);
      Serial.print("🔁 Feedback Changed → Code: ");
      Serial.println(code);
      if (CONTROLLER_TYPE == 1) {
        controlRelay(BOARD_ID, ctrlNumber, stateValue);
      }
      sendSystemCode(String(code));
    }
  }

}

void controlRelay(uint8_t board, uint8_t ctrl, uint8_t state) {

  if (board != BOARD_ID) {
    Serial.println("⚠️ Packet not for this board, ignoring.");
    return;
  }
  if (ctrl < 1 || ctrl > RELAY_COUNT) {
    Serial.println("⚠️ Invalid controller number.");
    return;
  }
  uint8_t idx = ctrl - 1; // convert to 0-based index
  if (CONTROLLER_TYPE == 1) {
    // One-way: directly set relay based on state value
    if (state == 1) {
      relayState[idx] = true;
    } else {
      relayState[idx] = false;
    }

  } else if (CONTROLLER_TYPE == 2) {
    // Two-way: always toggle regardless of state value
    relayState[idx] = !relayState[idx];
  }
  // Apply to physical pin
  digitalWrite(RELAY_PINS[idx], relayState[idx] ? HIGH : LOW);
  saveRelayStates();
  Serial.print("🔌 Relay ");
  Serial.print(ctrl);
  Serial.print(" → ");
  Serial.println(relayState[idx] ? "ON" : "OFF");
}

void saveRelayStates() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    slaveConfig.relayStates[i] = relayState[i];
  }
  EEPROM.put(0, slaveConfig);
}

void restoreRelayStates() {
  EEPROM.get(0, slaveConfig);
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayState[i] = slaveConfig.relayStates[i];
    digitalWrite(RELAY_PINS[i], relayState[i] ? HIGH : LOW);
    Serial.print("🔁 Relay ");
    Serial.print(i + 1);
    Serial.print(" restored → ");
    Serial.println(relayState[i] ? "ON" : "OFF");
  }
}

void executeTimers() {
  if (!g_timeReady) return;

  for (uint8_t slot = 0; slot < RELAY_COUNT; slot++) {
    TimerEntry& t = slaveConfig.timer[slot];
    if (!t.active) continue;

    uint8_t ctrl = slot + 1;

    // ─── MODE 1 → ONCE ───────────────────────────────────────────
    if (t.mode == 1) {
      if (g_yy == t.ONyear && g_mm == t.ONmonth && g_dd == t.ONdate &&
          g_hh == t.ONhour && g_min == t.ONmin && g_sec == 0) {

        uint8_t action = t.cycle; // 1=ON, 0=OFF
        controlRelay(BOARD_ID, ctrl, action);
        t.active = false; // fire once then deactivate
        saveRelayStates();
        Serial.print("✅ Mode1 fired → R"); Serial.print(ctrl);
        Serial.println(action ? " ON" : " OFF");
      }
    }
    // ─── MODE 2 → DAILY ──────────────────────────────────────────
    else if (t.mode == 2) {
      if (g_sec == 0) { // check only on minute boundary
        if (g_hh == t.ONhour && g_min == t.ONmin) {
          controlRelay(BOARD_ID, ctrl, 1);
          Serial.print("✅ Mode2 ON fired → R"); Serial.println(ctrl);
        }
        else if (g_hh == t.OFFhour && g_min == t.OFFmin) {
          controlRelay(BOARD_ID, ctrl, 0);
          Serial.print("✅ Mode2 OFF fired → R"); Serial.println(ctrl);
        }
      }
    }
    // ─── MODE 3 → CYCLE ──────────────────────────────────────────
    // if (t.mode == 3) {
    //   if (g_sec == 0) { // check only on minute boundary
    //     if (g_hh == t.ONhour && g_min == t.ONdate) { // ONdate reused as trigger min

    //       if (t.cyclePhase == 0) {
    //         // was ON → turn OFF, next wait is OFFmin
    //         controlRelay(BOARD_ID, ctrl, 0);
    //         t.cyclePhase = 1;
    //         Serial.print("🔄 Mode3 OFF → R"); Serial.println(ctrl);

    //         // decrement cycle after each OFF
    //         if (t.cycle > 0) t.cycle--;
    //         if (t.cycle == 0) {
    //           t.active = false;
    //           saveRelayStates();
    //           Serial.print("✅ Mode3 complete → R"); Serial.println(ctrl);
    //           continue;
    //         }

    //         // calculate next ON time
    //         uint8_t nextMin  = g_min + t.OFFmin;
    //         uint8_t nextHour = g_hh;
    //         while (nextMin >= 60) { nextMin -= 60; nextHour++; }
    //         if (nextHour >= 24) nextHour -= 24;
    //         t.ONhour = nextHour;
    //         t.ONdate = nextMin;

    //       } else {
    //         // was OFF → turn ON, next wait is ONmin
    //         controlRelay(BOARD_ID, ctrl, 1);
    //         t.cyclePhase = 0;
    //         Serial.print("🔄 Mode3 ON → R"); Serial.println(ctrl);

    //         // calculate next OFF time
    //         uint8_t nextMin  = g_min + t.ONmin;
    //         uint8_t nextHour = g_hh;
    //         while (nextMin >= 60) { nextMin -= 60; nextHour++; }
    //         if (nextHour >= 24) nextHour -= 24;
    //         t.ONhour = nextHour;
    //         t.ONdate = nextMin;
    //       }

    //       saveRelayStates();
    //     }
    //   }
    // }
    // ─── MODE 4 → DELAYED SINGLE ACTION ──────────────────────────
    else if (t.mode == 4) {
      unsigned long now = millis();
      unsigned long elapsed = millis() - cycleLastMillis[slot]; // now - t.cycleLastMillis;

      if (elapsed >= (unsigned long)t.ONmin * 60000UL) {
        uint8_t action = t.cycle; // 1=ON after OFF, 0=OFF after ON
        controlRelay(BOARD_ID, ctrl, action);
        t.active = false;
        saveRelayStates();
        Serial.print("✅ Mode4 fired → R"); Serial.print(ctrl);
        Serial.println(action ? " ON" : " OFF");
      }
    }
  }
}

// void startMode3(uint8_t slot) {
//   TimerEntry& t = slaveConfig.timer[slot];
//   uint8_t ctrl  = slot + 1;

//   bool currentLoad = relayState[slot]; // true=ON, false=OFF

//   // Calculate next trigger time based on current state
//   uint8_t waitMin;
//   if (currentLoad) {
//     // Load is ON → wait ONmin then turn OFF
//     t.cyclePhase = 0; // 0 = currently ON, next action is OFF
//     waitMin = t.ONmin;
//   } else {
//     // Load is OFF → wait OFFmin then turn ON
//     t.cyclePhase = 1; // 1 = currently OFF, next action is ON
//     waitMin = t.OFFmin;
//   }

//   // Calculate next trigger time
//   uint8_t trigMin = g_min + waitMin;
//   uint8_t trigHour = g_hh;
//   while (trigMin >= 60) { trigMin -= 60; trigHour++; }
//   if (trigHour >= 24) trigHour -= 24;

//   // Store next trigger time in OFFhour/OFFmin temporarily
//   t.ONhour  = trigHour;  // reuse ONhour as next trigger hour
//   t.ONdate  = trigMin;   // reuse ONdate as next trigger min

//   Serial.print("⏱ Mode3 started | Load: "); Serial.print(currentLoad ? "ON" : "OFF");
//   Serial.print(" | Next trigger: "); Serial.print(trigHour); Serial.print(":"); Serial.println(trigMin);

//   saveRelayStates();
// }

