#include "Arduino.h"
#include <PCF8574.h>

#define A16S_RS485_RX 32   // RX für RS485 (an MAX485 oder ähnliches)
#define A16S_RS485_TX 33   // TX für RS485

// PCF8574 Expander für Relais
PCF8574 pcf8574_1(0x24, 4, 5); // OUT1–OUT8
PCF8574 pcf8574_2(0x25, 4, 5); // OUT9–OUT11

// PCF8574 Expander für digitale Eingänge (IN1–IN8 auf 0x22, IN9–IN16 auf 0x23)
PCF8574 pcf8574_in(0x22, 4, 5);  // IN1–IN8
PCF8574 pcf8574_in2(0x23, 4, 5); // IN9–IN16

// Zustände für Flankenerkennung von IN16
bool initializedIn16 = false;
bool lastStateIn16 = HIGH;
unsigned long lastChangeTime = 0;
unsigned long stableTime = 0;
bool wasStable = false;
const unsigned long DEBOUNCE_DELAY = 100; // 100ms zwischen Zustandswechseln
const unsigned long STABLE_TIME = 1000;   // 1000ms stabil = Taster gedrückt

// 🔹 Digitale Eingänge prüfen (IN1–IN8)
void handleInputCheck(uint8_t idx) {
  int state = pcf8574_in.digitalRead(idx);
  if (state == LOW) {
    Serial2.print("on\r");
    Serial.printf("IN%d ist AKTIV (LOW)\n", idx + 1);
  } else {
    Serial2.print("off\r");
    Serial.printf("IN%d ist INAKTIV (HIGH)\n", idx + 1);
  }
}

// 🔹 Analoge Spannung messen (z. B. an CHA1 → GPIO36)
void handleAdcRequest() {
  int raw = analogRead(36);  // ADC1_CHANNEL_0 → GPIO36
  char result[6];
  snprintf(result, sizeof(result), "%04d\r", raw); // 4-stellig mit führenden Nullen
  Serial2.print(result);
  Serial.printf("ADC-Messung an CHA1 (GPIO36): %s", result);
}

// 🔹 Relaissteuerung, Input-Abfrage und ADC-Auswertung über RS485-Befehl
void handleCommand(String cmd) {
  cmd.trim();
  Serial.print("Empfange Befehl: ");
  Serial.println(cmd);

  // ---------- HARDCHECK BEFEHL ----------
  if (cmd == "##hardcheck") {
    Serial.println("##hardcheck empfangen - Tests werden ausgeführt...");
    return;
  }

  // ---------- RELAIS: $outxxon / $outxxoff ----------
  if (cmd.startsWith("$out") && cmd.length() >= 8) {
    String numStr = cmd.substring(4, 6);  // z. B. "03"
    int relayNr = numStr.toInt();         // z. B. 3

    if (relayNr >= 1 && relayNr <= 11) {
      bool relais_on = cmd.endsWith("on");  // prüfe, ob Befehl mit "on" endet
      int pcf_idx = relayNr - 1;

      if (pcf_idx < 8) {
        pcf8574_1.digitalWrite(pcf_idx, relais_on ? LOW : HIGH);
        Serial.printf("-> Relais %d %s (PCF1 P%d)\n", relayNr, relais_on ? "EIN" : "AUS", pcf_idx);
      } else {
        pcf8574_2.digitalWrite(pcf_idx - 8, relais_on ? LOW : HIGH);
        Serial.printf("-> Relais %d %s (PCF2 P%d)\n", relayNr, relais_on ? "EIN" : "AUS", pcf_idx - 8);
      }
    }
  }

  // ---------- DIGITALE EINGÄNGE: $inxx ----------
  else if (cmd.startsWith("$in") && cmd.length() >= 5) {
    String numStr = cmd.substring(3, 5); // z. B. "04"
    int inNr = numStr.toInt();

    if (inNr >= 1 && inNr <= 8) {
      Serial.printf("Prüfe digitalen Eingang IN%d...\n", inNr);
      handleInputCheck(inNr - 1);
    }
  }

  // ---------- ANALOGER EINGANG: $ad ----------
  else if (cmd == "$ad") {
    Serial.println("Starte ADC-Messung...");
    handleAdcRequest();
  }

  // ---------- UNBEKANNTER BEFEHL ----------
  else {
    Serial.println("Unbekannter Befehl oder ungültiges Format.");
  }
}

// 🔹 IN16 prüfen (an Adresse 0x23, Pin 7) → Wenn Taster gedrückt wird, ##hardcheck senden
void checkInput16ForHardcheck() {
  int state = pcf8574_in2.digitalRead(7); // IN16 = P7
  unsigned long currentTime = millis();

  if (!initializedIn16) {
    lastStateIn16 = state;
    initializedIn16 = true;
    lastChangeTime = currentTime;
    stableTime = currentTime;
    Serial.printf("IN16 initialisiert - Startzustand: %s\n", state == HIGH ? "HIGH" : "LOW");
    return;
  }

  // Prüfen ob sich der Zustand geändert hat
  if (state != lastStateIn16) {
    // Zustand hat sich geändert - Timer zurücksetzen
    if ((currentTime - lastChangeTime) > DEBOUNCE_DELAY) {
      lastChangeTime = currentTime;
      stableTime = currentTime;
      wasStable = false;
      // Debug-Ausgabe entfernt - zu viel Spam
    }
    lastStateIn16 = state;
    return;
  }

  // Zustand ist gleich geblieben - prüfen ob lange genug stabil
  if (!wasStable && (currentTime - stableTime) > STABLE_TIME) {
    // Pin ist jetzt stabil (Taster wurde gedrückt)
    wasStable = true;
    Serial2.write("\r");
    delay(50);
    Serial2.write("##hardcheck\r");
    Serial.println("##hardcheck gesendet (Taster gedrückt - Pin stabil)");
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(19200, SERIAL_8N1, A16S_RS485_RX, A16S_RS485_TX);

  analogReadResolution(12);

  // Expander starten
  pcf8574_1.begin();
  pcf8574_2.begin();
  pcf8574_in.begin();
  pcf8574_in2.begin();

  // Relais initialisieren (alles AUS)
  for (int i = 0; i < 8; i++) {
    pcf8574_1.pinMode(i, OUTPUT);
    pcf8574_2.pinMode(i, OUTPUT);
    pcf8574_1.digitalWrite(i, HIGH);
    pcf8574_2.digitalWrite(i, HIGH);
  }

  // Eingänge initialisieren
  for (int i = 0; i < 8; i++) {
    pcf8574_in.pinMode(i, INPUT);   // IN1–IN8
    pcf8574_in2.pinMode(i, INPUT);  // IN9–IN16
  }

  Serial.println("System gestartet – warte auf RS485-Befehle...");
}

void loop() {
  static String cmd = "";

  // RS485-Eingänge abfangen
  while (Serial2.available()) {
    char c = Serial2.read();

    if (c == '\r' || c == '\n') {
      if (cmd.length() > 0) {
        handleCommand(cmd);
        cmd = "";
      }
    } else {
      cmd += c;
    }
  }

  // IN16 überwachen
  checkInput16ForHardcheck();
}
