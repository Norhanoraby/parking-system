#include <STM32FreeRTOS.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

// ===================== RFID PINS =====================
#define ENTRY_SS_PIN PA4
#define EXIT_SS_PIN  PA11  // FIXED: Shifted off PB15 to clear SPI2 MOSI hardware data tracks
#define RST_PIN      -1    // FIXED: Set to -1 because RST is hardwired directly to the 3.3V power rail!

// ===================== OUTPUTS =====================
#define LED_PIN PC13
#define ENTRY_SERVO_PIN PA0
#define EXIT_SERVO_PIN  PA15
#define BUZZER_PIN PB12

// ===================== GATE IR SENSORS =====================
#define ENTRY_GATE_IR_PIN PA8
#define EXIT_GATE_IR_PIN  PB5

// ===================== SLOT IR SENSORS =====================
#define SLOT_A1_IR_PIN   PB3
#define SLOT_A2_IR_PIN   PB4
#define SLOT_B1_IR_PIN   PB9
#define SLOT_B2_IR_PIN   PB8
#define SLOT_VIP_IR_PIN  PA12  // FIXED: Shifted off PB14 to clear SPI2 MISO hardware data tracks

// ===================== OTHER SENSORS =====================
#define FLAME_DO_PIN PB13
#define FLAME_AO_PIN PA3

#define IR_DETECTED_STATE    LOW
#define FLAME_DETECTED_STATE HIGH

// Ultrasonic sensors
#define ENTRY_TRIG_PIN PB1
#define ENTRY_ECHO_PIN PB0
#define EXIT_TRIG_PIN  PA1
#define EXIT_ECHO_PIN  PA2

#define DETECT_DISTANCE       6
#define TOTAL_SLOTS           4
#define PRICE_PER_MINUTE      5
#define PENALTY_FEE           10

#define ENTRY_GATE_OPEN_ANGLE  0
#define ENTRY_GATE_CLOSE_ANGLE 90
#define EXIT_GATE_OPEN_ANGLE   0
#define EXIT_GATE_CLOSE_ANGLE  90

#define FLAME_CLEAR_DELAY      10000
#define GATE_CLOSE_DELAY        2000
#define IR_MAX_WAIT_TIME        5000

#define UBER_ALERT_TIME       180000UL
#define UBER_ALLOWED_TIME     300000UL
#define UBER_FEE_PER_MINUTE        5
#define ADMIN_LCD_CLEAR_DELAY   2000

#define ULTRASONIC_INTERVAL    500
#define RFID_COOLDOWN         2000
#define SAFETY_CHECK_INTERVAL  300
#define SLOT_CHECK_INTERVAL     50    // poll the slot IR sensors every 50 ms (fast UI)
#define SLOT_DEBOUNCE_SAMPLES    2    // consecutive equal reads before committing a change
#define SLOT_UPDATE_MIN_INTERVAL 200  // ms; coalesces rapid changes so link doesn't back up
#define SLOT_CONFIRM_TIMEOUT  30000UL   // 30 seconds to confirm

// ---- FreeRTOS helpers ----
#define DLY(ms) vTaskDelay(pdMS_TO_TICKS(ms))   // yielding delay for task code

HardwareSerial SerialESP(PA10, PA9);
MFRC522 entryRFID(ENTRY_SS_PIN, RST_PIN);
MFRC522 exitRFID(EXIT_SS_PIN, RST_PIN);   
Servo entryServo;
Servo exitServo;
hd44780_I2Cexp lcd;

// ===================== FreeRTOS OBJECTS =====================
TaskHandle_t      controlTaskHandle = NULL;
TaskHandle_t      slotTaskHandle    = NULL;
TaskHandle_t      safetyTaskHandle  = NULL;
SemaphoreHandle_t lcdMutex          = NULL;
SemaphoreHandle_t serialMutex       = NULL;
SemaphoreHandle_t stateMutex        = NULL;   // RECURSIVE

volatile bool fireActive = false;   // set by TaskSafety, read by all tasks

// ===================== CARD UIDs =====================
byte card1[4]      = {0xD3, 0x01, 0xD3, 0x2A};
byte card2[4]      = {0x03, 0x10, 0x93, 0xAA};
byte card3[4]      = {0x63, 0x8E, 0xCC, 0x29};
byte card4[4]      = {0x4A, 0xF5, 0x94, 0x97};
byte adminCard[4]  = {0xD3, 0x9C, 0xD7, 0x2A};
byte manualCard[4] = {0x9B, 0x01, 0xEC, 0x05};
byte vipCard[4]    = {0x60, 0x52, 0xF2, 0x61};

// ===================== SLOT NAMES =====================
const char* slotNames[4]  = {"A1", "A2", "B1", "B2"};
const int   slotIRPins[4] = {SLOT_A1_IR_PIN, SLOT_A2_IR_PIN, SLOT_B1_IR_PIN, SLOT_B2_IR_PIN};

// ===================== CAR MODE DEFINITIONS (FIXED POSITION) =====================
enum CarMode { NO_CAR, ENTRY_CAR, EXIT_CAR };
CarMode currentMode = NO_CAR;

// ===================== SHARED PARKING STATE (guard with stateMutex) =========
int  slotAssignedTo[4] = {-1, -1, -1, -1};
int  cardSlot[4]       = {-1, -1, -1,-1};
bool virtualSlot[4]    = {false, false, false, false};
bool physicalSlot[4]   = {false, false, false, false};

bool          cardInside[4]     = {false, false, false, false}; 
unsigned long entryTime[4]      = {0, 0, 0, 0}; 
unsigned long exitTime[4]       = {0, 0, 0, 0}; 
unsigned long parkingDuration[4]= {0, 0, 0, 0}; 
int           parkingFee[4]     = {0, 0, 0, 0}; 
int           penaltyFee[4]     = {0, 0, 0, 0}; 

// ===================== SLOT CONFIRM STATE =====================
bool          waitingForSlotConfirm = false;
int           confirmCardIndex       = -1;
int           confirmSlotIndex       = -1;
unsigned long confirmStart           = 0;
bool          wrongSlotBuzzing       = false;

// ===================== VIP STATE =====================
bool          vipInside     = false;
unsigned long vipEntryTime  = 0;
bool          slotOccupied_VIP = false;

// ===================== UBER STATE =====================
bool          uberGateOpen     = false;
bool          uberInside       = false;
bool          uberAlertSent    = false;
bool          uberOvertimeSent = false;
unsigned long uberEntryTime    = 0;
bool          uberUsedEntryGate = false;

// ===================== SYSTEM STATE =====================
bool waitingForPayment = false;
int  paymentCardIndex  = -1;
int  availableSlots    = 4;

unsigned long lastUltrasonicCheck = 0;
unsigned long lastRFIDScan        = 0;

bool lcdBusy = false;   // set by TaskControl while it owns an interaction

// ===================== LCD LOCK HELPERS =====================
static inline void lcdLock()   { xSemaphoreTake(lcdMutex, portMAX_DELAY); }
static inline void lcdUnlock() { xSemaphoreGive(lcdMutex); }

// Simple two-line message. Holds the mutex only for the write.
void lcdMsg(const char* l0, const char* l1) {
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0); if (l0) lcd.print(l0);
  lcd.setCursor(0, 1); if (l1) lcd.print(l1);
  lcdUnlock();
}

// ===================== UID HELPERS =====================
bool compareUID(byte *a, byte *b) {
  for (byte i = 0; i < 4; i++) if (a[i] != b[i]) return false;
  return true;
}
bool isAdminCard(byte *uid)  { return compareUID(uid, adminCard); }
bool isManualCard(byte *uid) { return compareUID(uid, manualCard); }
bool isVipCard(byte *uid)    { return compareUID(uid, vipCard); }

int getCardIndex(byte *uid) {
  if (compareUID(uid, card1)) return 0;
  if (compareUID(uid, card2)) return 1;
  if (compareUID(uid, card3)) return 2;
  if (compareUID(uid, card4)) return 3;
  return -1;
}

bool isIRDetected(int irPin) { return digitalRead(irPin) == IR_DETECTED_STATE; }
bool isFlameDetected()       { return digitalRead(FLAME_DO_PIN) == FLAME_DETECTED_STATE; }

long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);   // busy-wait: required for HC-SR04
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 25000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void buzzerBeep(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH); DLY(200);
    digitalWrite(BUZZER_PIN, LOW);  DLY(200);
  }
}
void blink(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);  DLY(300);
    digitalWrite(LED_PIN, HIGH); DLY(300);
  }
}
void fastBlink() {
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED_PIN, LOW);  DLY(80);
    digitalWrite(LED_PIN, HIGH); DLY(80);
  }
}

// ===================== SLOT HELPERS (stateMutex is RECURSIVE) =====================
static inline bool slotTakenByCar(int i) {
  return physicalSlot[i] && slotAssignedTo[i] >= 0 && slotAssignedTo[i] < 3;
}

static void recountAvailable_locked() {   // caller must hold stateMutex
  availableSlots = 0;
  for (int i = 0; i < 4; i++) if (!slotTakenByCar(i)) availableSlots++;
}

int getFirstFreeSlot() {
  int r = -1;
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  for (int i = 0; i < 4; i++)
    if (!physicalSlot[i] && slotAssignedTo[i] == -1) { r = i; break; }
  if (r == -1)
    for (int i = 0; i < 4; i++)
      if (!physicalSlot[i]) { r = i; break; }
  xSemaphoreGiveRecursive(stateMutex);
  return r;
}

void assignSlot(int cardIndex, int slotIndex) {
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  if (cardIndex >= 0 && cardIndex < 3) cardSlot[cardIndex] = slotIndex;
  slotAssignedTo[slotIndex] = cardIndex;
  virtualSlot[slotIndex]    = true;
  recountAvailable_locked();
  xSemaphoreGiveRecursive(stateMutex);
}

void freeSlot(int cardIndex) {
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  if (cardIndex >= 0 && cardIndex < 3) {
    int s = cardSlot[cardIndex];
    if (s >= 0 && s < 4) { slotAssignedTo[s] = -1; virtualSlot[s] = false; }
    cardSlot[cardIndex] = -1;
  }
  recountAvailable_locked();
  xSemaphoreGiveRecursive(stateMutex);
}

void readSlotIRSensors() {
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  physicalSlot[0]  = isIRDetected(SLOT_A1_IR_PIN);
  physicalSlot[1]  = isIRDetected(SLOT_A2_IR_PIN);
  physicalSlot[2]  = isIRDetected(SLOT_B1_IR_PIN);
  physicalSlot[3]  = isIRDetected(SLOT_B2_IR_PIN);
  slotOccupied_VIP = isIRDetected(SLOT_VIP_IR_PIN);
  recountAvailable_locked();          // count follows the sensors, not reservations
  xSemaphoreGiveRecursive(stateMutex);
}

void readSlotRaw(bool s[4], bool &vip) {
  s[0] = isIRDetected(SLOT_A1_IR_PIN);
  s[1] = isIRDetected(SLOT_A2_IR_PIN);
  s[2] = isIRDetected(SLOT_B1_IR_PIN);
  s[3] = isIRDetected(SLOT_B2_IR_PIN);
  vip  = isIRDetected(SLOT_VIP_IR_PIN);
}

// Returns true if ANY IR sensor (both gate IRs + 4 slots + VIP slot)
// currently sees a car. Used during fire evacuation to hold the gates open.
bool anyCarDetectedByIR() {
  if (isIRDetected(ENTRY_GATE_IR_PIN)) return true;
  if (isIRDetected(EXIT_GATE_IR_PIN))  return true;
  for (int i = 0; i < 4; i++)
    if (isIRDetected(slotIRPins[i])) return true;
  if (isIRDetected(SLOT_VIP_IR_PIN)) return true;
  return false;
}

// ===================== SEND EVENT (serialMutex) =====================
void sendEventToESP(const char* eventType, int cardIndex, int fee, int penalty, unsigned long durationMin = 0) {
  char uidStr[12] = "none";
  if      (cardIndex >= 0 && cardIndex < 4) {
byte* card = (cardIndex == 0) ? card1 : (cardIndex == 1) ? card2 : (cardIndex == 2) ? card3 : card4; 
    sprintf(uidStr, "%02X%02X%02X%02X", card[0], card[1], card[2], card[3]);
  }
  else if (cardIndex == -2) sprintf(uidStr, "%02X%02X%02X%02X", manualCard[0], manualCard[1], manualCard[2], manualCard[3]);
  else if (cardIndex == -3) sprintf(uidStr, "%02X%02X%02X%02X", adminCard[0],  adminCard[1],  adminCard[2],  adminCard[3]);
  else if (cardIndex == -4) sprintf(uidStr, "%02X%02X%02X%02X", vipCard[0],    vipCard[1],    vipCard[2],    vipCard[3]);

  xSemaphoreTake(serialMutex, portMAX_DELAY);
  SerialESP.print("{\"event\":\"");   SerialESP.print(eventType);
  SerialESP.print("\",\"uid\":\"");   SerialESP.print(uidStr);
  SerialESP.print("\",\"fee\":");     SerialESP.print(fee);
  SerialESP.print(",\"penalty\":");   SerialESP.print(penalty);
  SerialESP.print(",\"duration\":");  SerialESP.print(durationMin);
  SerialESP.print(",\"slotsLeft\":"); SerialESP.print(availableSlots);
  SerialESP.print(",\"slotStates\":{\"A1\":"); SerialESP.print(slotTakenByCar(0) ? "true" : "false");
  SerialESP.print(",\"A2\":");  SerialESP.print(slotTakenByCar(1) ? "true" : "false");
  SerialESP.print(",\"B1\":");  SerialESP.print(slotTakenByCar(2) ? "true" : "false");
  SerialESP.print(",\"B2\":");  SerialESP.print(slotTakenByCar(3) ? "true" : "false");
  
  // FIXED: Dashboard will only mark VIP slot as fully occupied if it reads TRUE AND the VIP is actually inside.
  SerialESP.print(",\"VIP\":"); SerialESP.print((slotOccupied_VIP && vipInside) ? "true" : "false");
  
  SerialESP.println("}}");
  xSemaphoreGive(serialMutex);
}

// ===================== LCD READY =====================
void showReadyMessage() {
  lcdLock();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Slots: "); lcd.print(availableSlots);
  lcd.setCursor(0, 1);
  if (availableSlots > 0) lcd.print("Waiting Car...");
  else                    lcd.print("Parking FULL");
  lcdUnlock();
}

// ===================== GATE IR WAIT =====================
void waitForIRBeforeClosing(Servo &servoMotor, int irPin, int cardIndex, bool addPenalty, int openAngle) {
  unsigned long irStartTime = 0;
  bool timerStarted = false;
  bool penaltyAdded = false;

  lcdMsg("Gate Open", "Move Forward");

  while (true) {
    if (fireActive) return;            // safety task owns the gates now
    servoMotor.write(openAngle);

    if (isIRDetected(irPin)) {
      if (!timerStarted) {
        timerStarted = true; irStartTime = millis();
        lcdMsg("Car Under Gate", "Please Move");
      }
      if (addPenalty && !penaltyAdded && millis() - irStartTime >= IR_MAX_WAIT_TIME) {
        penaltyAdded = true;
        if (cardIndex >= 0) {
          penaltyFee[cardIndex] += PENALTY_FEE;
          sendEventToESP("PENALTY", cardIndex, parkingFee[cardIndex], penaltyFee[cardIndex]);
        }
        lcdLock();
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("MOVE CAR NOW!");
        lcd.setCursor(0, 1); lcd.print("Penalty +"); lcd.print(PENALTY_FEE);
        lcdUnlock();
        buzzerBeep(5);          // penalty: car blocking the gate too long
      }
    } else {
      DLY(GATE_CLOSE_DELAY);
      return;
    }
    DLY(200);
  }
}

void reinitRFID() {
  entryRFID.PCD_Init(); DLY(20);
  exitRFID.PCD_Init();  DLY(20);
  entryRFID.PCD_AntennaOn();
  exitRFID.PCD_AntennaOn();
  lastRFIDScan = 0;
}

void openEntryGate(int cardIndex) {
  lcdBusy = true;
  lcdMsg("Entry Gate", "Opening");
  entryServo.write(ENTRY_GATE_OPEN_ANGLE); DLY(2000);
  waitForIRBeforeClosing(entryServo, ENTRY_GATE_IR_PIN, cardIndex, true, ENTRY_GATE_OPEN_ANGLE);
  if (fireActive) { lcdBusy = false; return; }
  lcdMsg("Entry Closing", NULL);
  entryServo.write(ENTRY_GATE_CLOSE_ANGLE); DLY(1000);
  reinitRFID();                 // recover readers after the servo current spike
  lcdBusy = false;
}

void openExitGate(int cardIndex) {
  lcdBusy = true;
  lcdMsg("Exit Gate", "Opening");
  exitServo.write(EXIT_GATE_OPEN_ANGLE); DLY(2000);
  // No penalty at the exit gate: a car sitting under the exit servo is not charged.
  waitForIRBeforeClosing(exitServo, EXIT_GATE_IR_PIN, cardIndex, false, EXIT_GATE_OPEN_ANGLE);
  if (fireActive) { lcdBusy = false; return; }
  lcdMsg("Exit Closing", NULL);
  exitServo.write(EXIT_GATE_CLOSE_ANGLE); DLY(1000);
  reinitRFID();                 // recover readers after the servo current spike
  lcdBusy = false;
}

void haltRFID(MFRC522 &reader) { reader.PICC_HaltA(); reader.PCD_StopCrypto1(); }

// ===================== SLOT CONFIRM HANDLER =====================
void handleSlotConfirm() {
  if (millis() - confirmStart > SLOT_CONFIRM_TIMEOUT) {
    freeSlot(confirmCardIndex);
    cardInside[confirmCardIndex] = false;
    waitingForSlotConfirm = false;
    confirmCardIndex = -1;
    confirmSlotIndex = -1;
    lcdBusy = false;
    currentMode = NO_CAR;
    lcdMsg("Confirm Timeout", "Please Retry");
    DLY(2000);
    showReadyMessage();
    return;
  }

  if (millis() - lastRFIDScan < 800) return;

  if (entryRFID.PICC_IsNewCardPresent() && entryRFID.PICC_ReadCardSerial()) {
    lastRFIDScan = millis();
    byte *uid = entryRFID.uid.uidByte;
    int scannedCard = getCardIndex(uid);

    if (scannedCard == confirmCardIndex) {
      waitingForSlotConfirm = false;
      sendEventToESP("ENTRY", confirmCardIndex, 0, 0);

      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Confirmed!");
      lcd.setCursor(0, 1); lcd.print("Slot: "); lcd.print(slotNames[confirmSlotIndex]);
      lcdUnlock();
      blink(2);
      DLY(1000);

      openEntryGate(confirmCardIndex);

      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Car Entered");
      lcd.setCursor(0, 1); lcd.print("Slots: "); lcd.print(availableSlots);
      lcdUnlock();
      DLY(2000);

      confirmCardIndex = -1;
      confirmSlotIndex = -1;
      lcdBusy = false;
      currentMode = NO_CAR;
      showReadyMessage();
    }
    haltRFID(entryRFID);
  }
}

// ===================== ULTRASONICS =====================
void checkUltrasonics() {
  if (lcdBusy) return;
  long entryDist = getDistance(ENTRY_TRIG_PIN, ENTRY_ECHO_PIN);
  long exitDist  = getDistance(EXIT_TRIG_PIN,  EXIT_ECHO_PIN);

  if (entryDist < DETECT_DISTANCE) {
    if (currentMode != ENTRY_CAR) {
      currentMode = ENTRY_CAR;
      if (availableSlots > 0) lcdMsg("Entry Detected", "Scan ENTRY RFID");
      else                    lcdMsg("FULL - VIP OK",  "Scan ENTRY RFID");
    }
  } else if (exitDist < DETECT_DISTANCE) {
    if (currentMode != EXIT_CAR) {
      currentMode = EXIT_CAR;
      lcdMsg("Exit Detected", "Scan EXIT RFID");
    }
  } else {
    if (currentMode != NO_CAR && !waitingForPayment && !waitingForSlotConfirm) {
      currentMode = NO_CAR; showReadyMessage();
    }
  }
}

// ===================== CARD HANDLER =====================
void handleScannedCard(MFRC522 &reader, bool scannedAtExitGate) {
  lastRFIDScan = millis();
  lcdBusy = true;
  byte *uid = reader.uid.uidByte;

  // ── ADMIN ──────────────────────────────────────────────
  if (isAdminCard(uid)) {
    sendEventToESP("ADMIN_ACCESS", -3, 0, 0);
    lcdMsg("ADMIN ACCESS", "Opening Admin...");
    buzzerBeep(2);
    DLY(ADMIN_LCD_CLEAR_DELAY);
    lcdBusy = false; currentMode = NO_CAR;
    haltRFID(reader); showReadyMessage(); return;
  }

  // ── VIP ────────────────────────────────────────────────
  if (isVipCard(uid)) {
    if (currentMode == ENTRY_CAR && !scannedAtExitGate) {
      if (vipInside) {
        lcdMsg("VIP Already", "Inside!");
        fastBlink(); DLY(2000);
        lcdBusy = false; showReadyMessage(); haltRFID(reader); return;
      }
      vipInside = true; vipEntryTime = millis();
      sendEventToESP("VIP_ENTRY", -4, 0, 0);
      lcdMsg("VIP - Go to VIP", "Scan to Open");
      blink(2); DLY(1000);
      openEntryGate(-4);
      lcdMsg("VIP Parked", "No Fees");
      DLY(2000);
      lcdBusy = false; currentMode = NO_CAR;
      haltRFID(reader); showReadyMessage(); return;
    }
    if (currentMode == EXIT_CAR && scannedAtExitGate) {
      if (!vipInside) {
        lcdMsg("VIP Not Inside", "Exit Denied");
        fastBlink(); DLY(2000);
        lcdBusy = false; showReadyMessage(); haltRFID(reader); return;
      }
      vipInside = false;
      unsigned long dur = (millis() - vipEntryTime) / 60000;
      if (dur == 0) dur = 1;
      sendEventToESP("VIP_EXIT", -4, 0, 0, dur);
      lcdMsg("VIP Exit", "No Charge!");
      blink(2);
      openExitGate(-4);
      DLY(1500);
      lcdBusy = false; currentMode = NO_CAR;
      haltRFID(reader); showReadyMessage(); return;
    }
    lcdMsg("Wrong Scanner", "Check Gate");
    DLY(1500); lcdBusy = false; haltRFID(reader); return;
  }

  // ── UBER/MANUAL ────────────────────────────────────────
  if (isManualCard(uid)) {
    lcdMsg("UBER ACCESS", "Gate Opening...");
    buzzerBeep(1);
    if (scannedAtExitGate) { sendEventToESP("UBER_EXIT",  -2, 0, 0); openExitGate(-2); }
    else                   { sendEventToESP("UBER_ENTRY", -2, 0, 0); openEntryGate(-2); }
    lcdBusy = false; currentMode = NO_CAR;
    haltRFID(reader); showReadyMessage(); return;
  }

  // ── REGULAR CARDS ───────────────────────────────────────
  int cardIndex = getCardIndex(uid);

  if (currentMode == NO_CAR && !waitingForPayment) {
    lcdMsg("No Car Detected", "Wait Vehicle");
    DLY(1500); lcdBusy = false; showReadyMessage(); haltRFID(reader); return;
  }

  if (cardIndex == -1) {
    sendEventToESP("UNKNOWN_CARD", -1, 0, 0);
    lcdMsg("ACCESS DENIED", "Unknown Card");
    fastBlink(); DLY(2000); lcdBusy = false;
    if (waitingForPayment) lcdMsg("Scan Same Card", "At EXIT Gate");
    else                   showReadyMessage();
    haltRFID(reader); return;
  }

  // ── PAYMENT ──
  if (waitingForPayment) {
    if (!scannedAtExitGate) {
      lcdMsg("Use EXIT RFID", "To Pay/Exit");
      DLY(1500); lcdBusy = false; haltRFID(reader); return;
    }
    if (cardIndex == paymentCardIndex) {
      int totalFee = parkingFee[paymentCardIndex] + penaltyFee[paymentCardIndex];
      sendEventToESP("PAYMENT_DONE", paymentCardIndex, parkingFee[paymentCardIndex],
                     penaltyFee[paymentCardIndex], parkingDuration[paymentCardIndex]);
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Payment Done");
      lcd.setCursor(0, 1); lcd.print("Total:"); lcd.print(totalFee); lcd.print("EGP");
      lcdUnlock();
      blink(2); DLY(1500);
      openExitGate(paymentCardIndex);
      freeSlot(paymentCardIndex);
      cardInside[paymentCardIndex] = false;
      penaltyFee[paymentCardIndex] = 0;
      waitingForPayment = false; paymentCardIndex = -1;
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Car Exited");
      lcd.setCursor(0, 1); lcd.print("Slots: "); lcd.print(availableSlots);
      lcdUnlock();
      DLY(2000);
      lcdBusy = false; currentMode = NO_CAR;
      showReadyMessage(); haltRFID(reader); return;
    } else {
      sendEventToESP("PAYMENT_DENIED", cardIndex, 0, 0);
      lcdMsg("Wrong Card", "Pay Denied");
      fastBlink(); DLY(2000);
      lcdMsg("Scan Same Card", "At EXIT Gate");
      lcdBusy = false; haltRFID(reader); return;
    }
  }

  // ── ENTRY ──
  if (currentMode == ENTRY_CAR) {
    if (scannedAtExitGate) {
      lcdMsg("Wrong Scanner", "Use ENTRY RFID");
      DLY(1500); lcdBusy = false; haltRFID(reader); return;
    }
    if (availableSlots <= 0) {
      lcdMsg("Parking FULL", "Entry Denied");
      DLY(1500); lcdBusy = false; haltRFID(reader); return;
    }
    if (cardInside[cardIndex]) {
      sendEventToESP("ENTRY_DENIED_ALREADY_INSIDE", cardIndex, 0, 0);
      lcdMsg("Already Inside", "Entry Denied");
      fastBlink(); DLY(2000);
      lcdBusy = false; currentMode = NO_CAR;
      showReadyMessage(); haltRFID(reader); return;
    }

    int freeSlotIdx = getFirstFreeSlot();
    if (freeSlotIdx < 0) {                    
      lcdMsg("Parking FULL", "Entry Denied");
      DLY(1500); lcdBusy = false; haltRFID(reader); return;
    }
    assignSlot(cardIndex, freeSlotIdx);
    cardInside[cardIndex] = true;
    entryTime[cardIndex]  = millis();

    confirmCardIndex      = cardIndex;
    confirmSlotIndex      = freeSlotIdx;
    confirmStart          = millis();
    wrongSlotBuzzing      = false;
    waitingForSlotConfirm = true;

    lcdLock();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Go to Slot "); lcd.print(slotNames[freeSlotIdx]);
    lcd.setCursor(0, 1); lcd.print("Scan again=Open");
    lcdUnlock();

    haltRFID(reader);
    return;
  }

  // ── EXIT ──
  else if (currentMode == EXIT_CAR) {
    if (!scannedAtExitGate) {
      lcdMsg("Wrong Scanner", "Use EXIT RFID");
      DLY(1500); lcdBusy = false; haltRFID(reader); return;
    }
    if (!cardInside[cardIndex]) {
      sendEventToESP("EXIT_DENIED_NOT_INSIDE", cardIndex, 0, 0);
      lcdMsg("Not Inside", "Exit Denied");
      fastBlink(); DLY(2000);
      lcdBusy = false; currentMode = NO_CAR; showReadyMessage(); haltRFID(reader); return;
    } else {
      exitTime[cardIndex]        = millis();
      unsigned long stayedTime   = exitTime[cardIndex] - entryTime[cardIndex];
      parkingDuration[cardIndex] = stayedTime / 60000;
      if (parkingDuration[cardIndex] == 0) parkingDuration[cardIndex] = 1;
      parkingFee[cardIndex] = parkingDuration[cardIndex] * PRICE_PER_MINUTE;
      int totalFee          = parkingFee[cardIndex] + penaltyFee[cardIndex];
      sendEventToESP("EXIT_PENDING_PAYMENT", cardIndex, parkingFee[cardIndex],
                     penaltyFee[cardIndex], parkingDuration[cardIndex]);
      lcdLock();
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Fee:"); lcd.print(parkingFee[cardIndex]);
      lcd.print(" Pen:"); lcd.print(penaltyFee[cardIndex]);
      lcd.setCursor(0, 1); lcd.print("Total:"); lcd.print(totalFee); lcd.print("EGP");
      lcdUnlock();
      DLY(3000);
      lcdMsg("Scan Again", "At EXIT Gate");
      waitingForPayment = true; paymentCardIndex = cardIndex;
      lcdBusy = false; haltRFID(reader); return;
    }
  }
}

void checkRFIDReaders() {
  if (millis() - lastRFIDScan < RFID_COOLDOWN) return;
  if (entryRFID.PICC_IsNewCardPresent() && entryRFID.PICC_ReadCardSerial()) {
    handleScannedCard(entryRFID, false); return;
  }
  if (exitRFID.PICC_IsNewCardPresent() && exitRFID.PICC_ReadCardSerial()) {
    handleScannedCard(exitRFID, true); return;
  }
}

// ============================================================================
//                                   TASKS
// ============================================================================

void TaskControl(void *pv) {
  (void)pv;
  for (;;) {
    if (fireActive) { DLY(50); continue; }  

    if (waitingForSlotConfirm) {
      handleSlotConfirm();
      DLY(20);
      continue;
    }

    if (millis() - lastUltrasonicCheck >= ULTRASONIC_INTERVAL) {
      lastUltrasonicCheck = millis();
      if (!waitingForPayment) checkUltrasonics();
    }

    checkRFIDReaders();
    DLY(20);   
  }
}

void TaskSlots(void *pv) {
  (void)pv;
  uint8_t stable[5] = {0, 0, 0, 0, 0};
  unsigned long lastSlotSend = 0;
  bool slotDirty = false;

  for (;;) {
    if (fireActive) { DLY(SLOT_CHECK_INTERVAL); continue; }

    bool raw[4]; bool rawVIP;
    readSlotRaw(raw, rawVIP);          

    bool stateChanged         = false;
    bool wrongParkingDetected = false;

    xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);

    for (int i = 0; i < 4; i++) {
      if (raw[i] == physicalSlot[i]) {
        stable[i] = 0;                 
        continue;
      }
      if (++stable[i] < SLOT_DEBOUNCE_SAMPLES) continue;   

      bool wasOccupied = physicalSlot[i];
      physicalSlot[i]  = raw[i];
      stable[i]        = 0;
      stateChanged     = true;

      if (physicalSlot[i] && !wasOccupied && slotAssignedTo[i] == -1) {
        wrongParkingDetected = true;
        int culpritCard = -1;
        for (int c = 0; c < 3; c++) {
          if (cardInside[c] && cardSlot[c] != -1 && !physicalSlot[cardSlot[c]]) {
            if (waitingForSlotConfirm && c == confirmCardIndex) continue;
            culpritCard = c; break;
          }
        }
        if (culpritCard != -1) {
          freeSlot(culpritCard);
          assignSlot(culpritCard, i);
        } else {
          slotAssignedTo[i] = 99;      
          virtualSlot[i]    = true;
        }
      }

      if (!physicalSlot[i] && wasOccupied && slotAssignedTo[i] == 99) {
        slotAssignedTo[i] = -1;
        virtualSlot[i]    = false;
      }
    }

    if (rawVIP == slotOccupied_VIP) {
      stable[4] = 0;
    } else if (++stable[4] >= SLOT_DEBOUNCE_SAMPLES) {
      slotOccupied_VIP = rawVIP;
      stable[4]        = 0;
      stateChanged     = true;
    }

    if (stateChanged) recountAvailable_locked();   
    xSemaphoreGiveRecursive(stateMutex);

    if (stateChanged) slotDirty = true;
    if (slotDirty && millis() - lastSlotSend >= SLOT_UPDATE_MIN_INTERVAL) {
      sendEventToESP("SLOT_UPDATE", -1, 0, 0);
      lastSlotSend = millis();
      slotDirty    = false;
    }

    // ── FIXED: VIP UNAUTHORIZED PARKING HANDLER ────────────────────────────
    bool vipViolation = false;
    xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
    vipViolation = (slotOccupied_VIP && !vipInside);
    xSemaphoreGiveRecursive(stateMutex);

    if (vipViolation) {
      // 1) Non-blocking buzzer toggle (50ms * 5 = 250ms half-period)
      static uint8_t beepCount = 0;
      beepCount++;
      if (beepCount % 10 < 5) digitalWrite(BUZZER_PIN, HIGH);
      else                    digitalWrite(BUZZER_PIN, LOW);

      // 2) Show message on LCD
      if (!lcdBusy && currentMode == NO_CAR && !waitingForSlotConfirm && !waitingForPayment) {
        lcdMsg("VIP SLOT TAKEN", "Car Must Leave");
      }
    } else {
      // 3) Default State: Make sure buzzer turns off when violation clears
      digitalWrite(BUZZER_PIN, LOW);

      if (wrongParkingDetected) {
        buzzerBeep(2);
        if (!lcdBusy && currentMode == NO_CAR && !waitingForSlotConfirm) {
          lcdMsg("SLOT OVERRIDDEN", "Warning Logged");
          DLY(3000);
          showReadyMessage();
        }
      } else if (stateChanged && !lcdBusy && currentMode == NO_CAR
                 && !waitingForSlotConfirm && !waitingForPayment) {
        showReadyMessage();
      }
    }
    // ───────────────────────────────────────────────────────────────────────

    DLY(SLOT_CHECK_INTERVAL);
  }
}

void TaskSafety(void *pv) {
  (void)pv;
  for (;;) {
    if (isFlameDetected()) {
      if (!fireActive) {
        fireActive = true;                 
        digitalWrite(BUZZER_PIN, HIGH);
        digitalWrite(LED_PIN, LOW);
        entryServo.write(ENTRY_GATE_OPEN_ANGLE);
        exitServo.write(EXIT_GATE_OPEN_ANGLE);
        sendEventToESP("FIRE_ALARM", -1, 0, 0);
        lcdMsg("!!! FIRE ALERT", "Both Gates Open");
      }
      entryServo.write(ENTRY_GATE_OPEN_ANGLE);
      exitServo.write(EXIT_GATE_OPEN_ANGLE);
      DLY(SAFETY_CHECK_INTERVAL);
      continue;
    }

    if (fireActive) {
      lcdMsg("Fire Stopped", "Keep Open");
      entryServo.write(ENTRY_GATE_OPEN_ANGLE);
      exitServo.write(EXIT_GATE_OPEN_ANGLE);

      unsigned long startDelay = millis();
      bool reignited = false;
      while (millis() - startDelay < FLAME_CLEAR_DELAY) {
        if (isFlameDetected()) { reignited = true; break; }
        entryServo.write(ENTRY_GATE_OPEN_ANGLE);
        exitServo.write(EXIT_GATE_OPEN_ANGLE);
        DLY(300);
      }
      if (reignited) continue;   

      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(LED_PIN, HIGH);

      xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
      for (int i = 0; i < 4; i++) { slotAssignedTo[i] = -1; virtualSlot[i] = false; }
      for (int i = 0; i < 3; i++) { cardInside[i] = false; cardSlot[i] = -1; }
      recountAvailable_locked();    
      waitingForSlotConfirm = false;
      waitingForPayment     = false;
      paymentCardIndex      = -1;
      currentMode           = NO_CAR;
      xSemaphoreGiveRecursive(stateMutex);

      // Keep BOTH gates open until EVERY IR sensor is clear of cars.
      // fireActive stays TRUE here so TaskControl/TaskSlots remain paused
      // and don't fight TaskSafety for the servos while we wait.
      lcdMsg("Emergency End", "Wait: Cars Out");
      bool reignitedDuringWait = false;
      while (anyCarDetectedByIR()) {
        if (isFlameDetected()) { reignitedDuringWait = true; break; }
        entryServo.write(ENTRY_GATE_OPEN_ANGLE);
        exitServo.write(EXIT_GATE_OPEN_ANGLE);
        DLY(200);
      }
      if (reignitedDuringWait) continue;   // fire came back: top of loop re-arms it

      lcdMsg("Emergency End", "Gates Closing");
      entryServo.write(ENTRY_GATE_CLOSE_ANGLE);
      exitServo.write(EXIT_GATE_CLOSE_ANGLE);
      DLY(1000);

      reinitRFID();

      readSlotIRSensors();
      sendEventToESP("FIRE_CLEARED", -1, 0, 0);

      fireActive = false;      // resume normal operation only after gates are shut
      lcdBusy = false;
      showReadyMessage();
    }

    DLY(SAFETY_CHECK_INTERVAL);
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(9600);
  SerialESP.begin(9600);   

  pinMode(LED_PIN, OUTPUT);    digitalWrite(LED_PIN, HIGH);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);

  pinMode(ENTRY_GATE_IR_PIN, INPUT);
  pinMode(EXIT_GATE_IR_PIN,  INPUT);
  pinMode(SLOT_A1_IR_PIN,    INPUT);
  pinMode(SLOT_A2_IR_PIN,    INPUT);
  pinMode(SLOT_B1_IR_PIN,    INPUT);
  pinMode(SLOT_B2_IR_PIN,    INPUT);
  pinMode(SLOT_VIP_IR_PIN,   INPUT);
  pinMode(FLAME_DO_PIN, INPUT);
  pinMode(FLAME_AO_PIN, INPUT_ANALOG);
  pinMode(ENTRY_TRIG_PIN, OUTPUT); pinMode(ENTRY_ECHO_PIN, INPUT);
  pinMode(EXIT_TRIG_PIN,  OUTPUT); pinMode(EXIT_ECHO_PIN,  INPUT);

  Wire.setSDA(PB7); Wire.setSCL(PB6); Wire.begin();
  int status = lcd.begin(16, 2);
  if (status) { while (1); }

  entryServo.attach(ENTRY_SERVO_PIN);
  exitServo.attach(EXIT_SERVO_PIN);
  entryServo.write(ENTRY_GATE_CLOSE_ANGLE);
  exitServo.write(EXIT_GATE_CLOSE_ANGLE);

  SPI.setMOSI(PA7); SPI.setMISO(PA6); SPI.setSCLK(PA5); SPI.begin();
  pinMode(ENTRY_SS_PIN, OUTPUT); digitalWrite(ENTRY_SS_PIN, HIGH);
  pinMode(EXIT_SS_PIN,  OUTPUT); digitalWrite(EXIT_SS_PIN,  HIGH);

  entryRFID.PCD_Init(); delay(50);
  exitRFID.PCD_Init();  delay(50);

  lcdMutex    = xSemaphoreCreateMutex();
  serialMutex = xSemaphoreCreateMutex();
  stateMutex  = xSemaphoreCreateRecursiveMutex();
  if (!lcdMutex || !serialMutex || !stateMutex) {
    lcd.clear(); lcd.print("Mutex alloc fail"); while (1);
  }

  readSlotIRSensors();
  delay(500);
  sendEventToESP("SLOT_UPDATE", -1, 0, 0);
  showReadyMessage();
  delay(1000);

  BaseType_t ok = pdPASS;
  ok &= xTaskCreate(TaskSafety,  "Safety",  224, NULL, 3, &safetyTaskHandle);
  ok &= xTaskCreate(TaskControl, "Control", 384, NULL, 2, &controlTaskHandle);
  ok &= xTaskCreate(TaskSlots,   "Slots",   192, NULL, 1, &slotTaskHandle);
  if (ok != pdPASS) {
    lcd.clear(); lcd.print("Task alloc fail");
    lcd.setCursor(0, 1); lcd.print("Raise heap size"); while (1);
  }

  vTaskStartScheduler();
  lcd.clear(); lcd.print("Scheduler failed"); while (1);
}

void loop() {
  // Empty
}