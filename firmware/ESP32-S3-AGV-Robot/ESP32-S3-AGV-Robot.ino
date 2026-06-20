// =================================================================
//   ESP32-S3 AGV Line-Follower + MQTT Dashboard + LCD V1.1
//   Added: Software E-STOP from dashboard
//   Board: ESP32-S3 N16R8 DevKitC
// =================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>

// ===================== PINS =====================
const int IR_LEFT  = 12;
const int IR_RIGHT = 13;

const int IN1 = 4;
const int IN2 = 5;
const int IN3 = 6;
const int IN4 = 7;

const int BUZZER_PIN = 14;

const int LCD_SDA = 16;
const int LCD_SCL = 15;

const int CARGO_SW1 = 17;
const int CARGO_SW2 = 18;

#define RGB_PIN     48
#define NUM_PIXELS  1

const int US_TRIG  = 46;
const int US_BACK  = 8;
const int US_FRONT = 9;
const int US_LEFT  = 10;
const int US_RIGHT = 11;

// ===================== OBJECTS =====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_NeoPixel rgbLed(NUM_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

WiFiClient espClient;
PubSubClient client(espClient);

// ===================== WIFI + MQTT =====================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

const char* mqtt_server = "io.adafruit.com";
const int   mqtt_port   = 1883;
const char* mqtt_user   = "YOUR_ADAFRUIT_IO_USERNAME";
const char* mqtt_key    = "YOUR_ADAFRUIT_IO_KEY";

const char* topic_status  = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-status";
const char* topic_counter = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-counter";
const char* topic_charge  = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-charge";
const char* topic_uptime  = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-uptime";
const char* topic_battery = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-battery";
const char* topic_estop   = "YOUR_ADAFRUIT_IO_USERNAME/feeds/agv-estop";

// ===================== VARIABLES =====================
int deliveryCount = 0;
int fixedBattery = 97;

bool estopActive = false;

String lastStatusPublished = "";
int lastCargoPublished = -1;

unsigned long startTime = 0;
unsigned long lastUptimeUpdate = 0;
unsigned long lastBatteryUpdate = 0;
unsigned long lastLCDUpdate = 0;

const unsigned long uptimeInterval  = 10000;
const unsigned long batteryInterval = 30000;
const unsigned long lcdInterval     = 300;

// ===================== PROTOTYPES =====================
void setup_wifi();
void reconnectMQTT();
void callback(char* topic, byte* payload, unsigned int length);

void moveForward();
void moveBackward();
void turnLeft();
void turnRight();
void stopMotors();

void playVictoryMelody();
void playSpinMelody();
void performTurnaround();

void setRGBColor(uint8_t r, uint8_t g, uint8_t b);
String getSystemUptime();

bool isCargoLoaded();
void publishStatusIfChanged(String status);
void publishCargoIfChanged(int cargo);

void printLCDLine(int row, String text);
void updateLCD(String status, int cargo);

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(300);

  rgbLed.begin();
  rgbLed.setBrightness(50);
  setRGBColor(255, 0, 0);

  Wire.begin(LCD_SDA, LCD_SCL);
  delay(500);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  printLCDLine(0, "VERSIGENT AGV");
  printLCDLine(1, "Booting System");

  pinMode(IR_LEFT, INPUT);
  pinMode(IR_RIGHT, INPUT);

  pinMode(CARGO_SW1, INPUT_PULLUP);
  pinMode(CARGO_SW2, INPUT_PULLUP);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(US_TRIG, OUTPUT);
  pinMode(US_BACK, INPUT);
  pinMode(US_FRONT, INPUT);
  pinMode(US_LEFT, INPUT);
  pinMode(US_RIGHT, INPUT);

  stopMotors();

  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  startTime = millis();

  printLCDLine(0, "MQTT Starting");
  printLCDLine(1, "Please Wait");
}

// ===================== LOOP =====================
void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();

  // ================= E-STOP LOCK =================
  if (estopActive) {
    stopMotors();
    setRGBColor(255, 0, 0);

    printLCDLine(0, "E-STOP ACTIVE");
    printLCDLine(1, "Press Clear");

    delay(50);
    return;
  }

  int leftSens  = digitalRead(IR_LEFT);
  int rightSens = digitalRead(IR_RIGHT);
  int cargo     = isCargoLoaded() ? 1 : 0;

  String currentAction = "IDLE";
  String mqttStatus    = "IDLE";

  // ================= LINE FOLLOWING =================
  if (leftSens == 0 && rightSens == 0) {
    moveForward();
    setRGBColor(0, 255, 0);
    currentAction = "CRUISE";
    mqttStatus = "CRUISE";
  }
  else if (leftSens == 1 && rightSens == 0) {
    turnRight();
    setRGBColor(0, 255, 0);
    currentAction = "FIX_R";
    mqttStatus = "CRUISE";
  }
  else if (leftSens == 0 && rightSens == 1) {
    turnLeft();
    setRGBColor(0, 255, 0);
    currentAction = "FIX_L";
    mqttStatus = "CRUISE";
  }
  else if (leftSens == 1 && rightSens == 1) {
    stopMotors();
    setRGBColor(255, 0, 0);

    deliveryCount++;

    client.publish(topic_status, "ARRIVED");
    client.publish(topic_counter, String(deliveryCount).c_str());
    client.publish(topic_charge, String(cargo).c_str());

    printLCDLine(0, "DELIVERY DONE");
    printLCDLine(1, "Count: " + String(deliveryCount));

    playVictoryMelody();

    for (int i = 5; i > 0; i--) {
      client.loop();

      if (estopActive) {
        stopMotors();
        printLCDLine(0, "E-STOP ACTIVE");
        printLCDLine(1, "Press Clear");
        return;
      }

      printLCDLine(0, "DELIVERY DONE");
      printLCDLine(1, "Unload in:" + String(i) + "s");

      delay(1000);
    }

    performTurnaround();

    lastStatusPublished = "";
    return;
  }

  // ================= MQTT PUBLISHING =================
  publishStatusIfChanged(mqttStatus);
  publishCargoIfChanged(cargo);

  if (millis() - lastUptimeUpdate >= uptimeInterval) {
    lastUptimeUpdate = millis();
    client.publish(topic_uptime, getSystemUptime().c_str());
  }

  if (millis() - lastBatteryUpdate >= batteryInterval) {
    lastBatteryUpdate = millis();
    client.publish(topic_battery, String(fixedBattery).c_str());
  }

  // ================= LCD UPDATE =================
  if (millis() - lastLCDUpdate >= lcdInterval) {
    lastLCDUpdate = millis();
    updateLCD(currentAction, cargo);
  }

  delay(15);
}

// ===================== MQTT CALLBACK =====================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  msg.trim();

  Serial.print("MQTT RX [");
  Serial.print(topic);
  Serial.print("] = ");
  Serial.println(msg);

  if (String(topic) == topic_estop) {
    if (msg == "1") {
      estopActive = true;

      stopMotors();
      setRGBColor(255, 0, 0);

      printLCDLine(0, "E-STOP ACTIVE");
      printLCDLine(1, "MOTORS LOCKED");

      client.publish(topic_status, "BUMP_ERR");
      lastStatusPublished = "BUMP_ERR";
    }

    else if (msg == "0") {
      estopActive = false;

      printLCDLine(0, "E-STOP CLEARED");
      printLCDLine(1, "Resuming...");

      client.publish(topic_status, "IDLE");
      lastStatusPublished = "IDLE";

      delay(1000);
    }
  }
}

// ===================== WIFI =====================
void setup_wifi() {
  printLCDLine(0, "Connect WiFi");
  printLCDLine(1, "Please Wait");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");

    if (millis() - startAttempt > 30000) {
      WiFi.disconnect();
      delay(500);
      WiFi.begin(ssid, password);
      startAttempt = millis();

      printLCDLine(0, "WiFi Retry");
      printLCDLine(1, "Please Wait");
    }
  }

  Serial.println();
  Serial.println("WiFi Connected");
  Serial.println(WiFi.localIP());

  printLCDLine(0, "WiFi Connected");
  printLCDLine(1, WiFi.localIP().toString());
  delay(2000);
}

// ===================== MQTT =====================
void reconnectMQTT() {
  while (!client.connected()) {
    printLCDLine(0, "MQTT Link");
    printLCDLine(1, "Connecting...");

    String clientId = "AGV_Client_" + String(random(0, 9999));

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_key)) {
      printLCDLine(0, "MQTT Connected");
      printLCDLine(1, "Dashboard Ready");
      delay(1000);

      client.subscribe(topic_estop);

      int cargo = isCargoLoaded() ? 1 : 0;

      client.publish(topic_status, "IDLE");
      client.publish(topic_counter, String(deliveryCount).c_str());
      client.publish(topic_charge, String(cargo).c_str());
      client.publish(topic_uptime, getSystemUptime().c_str());
      client.publish(topic_battery, String(fixedBattery).c_str());

      lastStatusPublished = "IDLE";
      lastCargoPublished = cargo;
    }
    else {
      printLCDLine(0, "MQTT Failed");
      printLCDLine(1, "Retrying...");
      delay(3000);
    }
  }
}

// ===================== HELPERS =====================
bool isCargoLoaded() {
  bool sw1 = digitalRead(CARGO_SW1) == LOW;
  bool sw2 = digitalRead(CARGO_SW2) == LOW;

  return sw1 || sw2;
}

void publishStatusIfChanged(String status) {
  if (status != lastStatusPublished) {
    client.publish(topic_status, status.c_str());
    lastStatusPublished = status;
  }
}

void publishCargoIfChanged(int cargo) {
  if (cargo != lastCargoPublished) {
    client.publish(topic_charge, String(cargo).c_str());
    lastCargoPublished = cargo;
  }
}

String getSystemUptime() {
  unsigned long totalSeconds = (millis() - startTime) / 1000;

  int seconds = totalSeconds % 60;
  int minutes = (totalSeconds / 60) % 60;
  int hours   = totalSeconds / 3600;

  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, seconds);

  return String(buffer);
}

// ===================== LCD =====================
void printLCDLine(int row, String text) {
  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  while (text.length() < 16) {
    text += " ";
  }

  lcd.setCursor(0, row);
  lcd.print(text);
}

void updateLCD(String status, int cargo) {
  printLCDLine(0, "VERSIGENT AGV");

  String row2 = status + " D:" + String(deliveryCount) + " C:" + String(cargo);
  printLCDLine(1, row2);
}

// ===================== RGB =====================
void setRGBColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
  rgbLed.show();
}

// ===================== MELODIES =====================
void playVictoryMelody() {
  int notes[] = {400, 500, 600, 800};

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 70; j++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delayMicroseconds(notes[i]);
      digitalWrite(BUZZER_PIN, LOW);
      delayMicroseconds(notes[i]);
    }
    delay(30);
  }
}

void playSpinMelody() {
  int waves[] = {900, 450, 900, 450};

  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 40; j++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delayMicroseconds(waves[i]);
      digitalWrite(BUZZER_PIN, LOW);
      delayMicroseconds(waves[i]);
    }
    delay(15);
  }
}

// ===================== TURNAROUND =====================
void performTurnaround() {
  client.publish(topic_status, "TURNING_AROUND");

  printLCDLine(0, "Turning Around");
  printLCDLine(1, "Please Wait");

  setRGBColor(0, 255, 0);
  playSpinMelody();

  turnRight();
  delay(500);

  while (digitalRead(IR_LEFT) == 0) {
    client.loop();

    if (estopActive) {
      stopMotors();
      printLCDLine(0, "E-STOP ACTIVE");
      printLCDLine(1, "Press Clear");
      return;
    }

    turnRight();
    delay(10);
  }

  stopMotors();
  setRGBColor(255, 0, 0);
  delay(200);
}

// ===================== MOTOR CONTROLS =====================
void moveForward() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void moveBackward() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void turnLeft() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void turnRight() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void stopMotors() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}