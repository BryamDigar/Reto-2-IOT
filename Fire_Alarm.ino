#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MQUnifiedsensor.h>
#include <WiFi.h>
#include "WebServer.h"
#include "WebPage.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DHTPIN 4
#define DHTTYPE DHT11
#define LED_PIN 27
#define BUZZER_PIN 26
#define FLAME_PIN 25
#define Board "ESP-32"
#define Pin 34  // Usamos un pin ADC adecuado para el ESP32

#define Type "MQ-2"
#define Voltage_Resolution 3.3
#define ADC_Bit_Resolution 12 
#define RatioMQ2CleanAir 9.83

#define RED_PIN 5
#define GREEN_PIN 18
#define BLUE_PIN 19

#define TEMP_LOW 8.4
#define TEMP_HIGH 13
#define HUMI_LOW 75
#define HUMI_HIGH 80
#define FIRE_THRESHOLD LOW

#define I2C_ADDR 0x27
#define LCD_COLUMNS 16
#define LCD_LINES 2

#define HISTORY_SIZE 60 // Guardar 60 lecturas (5 minutos con lecturas cada 5 segundos)

WebServer server(HTTP_PORT);

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(I2C_ADDR, LCD_COLUMNS, LCD_LINES);
MQUnifiedsensor MQ2(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);

const int numReadings = 10;
float tempReadings[numReadings] = {0};
float humiReadings[numReadings] = {0};
int coReadings[numReadings] = {0};
int index_gas = 0;

volatile bool alarmTriggered = false;
volatile bool dataReady = false;

// Variables para histórico
float tempHistory[HISTORY_SIZE] = {0};
float humiHistory[HISTORY_SIZE] = {0};
int coHistory[HISTORY_SIZE] = {0};
String statusHistory[HISTORY_SIZE] = {""};
unsigned long timestampHistory[HISTORY_SIZE] = {0};
int historyIndex = 0;
unsigned long lastHistoryUpdate = 0;
const unsigned long historyInterval = 5000; // Actualizar cada 5 segundos

// Variables para notificaciones y alarmas
volatile bool alarmActive = false;
String notifications[5] = {"", "", "", "", ""};
int notificationCount = 0;

struct SensorData {
  float temp;
  float humi;
  int co;
};

SensorData sensorData;

byte Alert0[8] = {0b00001, 0b00010, 0b00101, 0b00101, 0b01000, 0b10001, 0b10000, 0b01111};
byte Alert1[8] = {0b00000, 0b10000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00010, 0b11100};

void IRAM_ATTR triggerAlarm() {
    alarmTriggered = true;
}

void readSensorsTask(void *pvParameters) {
  while (true) {
    sensorData.temp = dht.readTemperature();
    sensorData.humi = dht.readHumidity();
    MQ2.update();
    sensorData.co = analogRead(Pin);
    sensorData.co = map(sensorData.co, 0, 4095, 0, 100);
    dataReady = true;
    vTaskDelay(500 / portTICK_PERIOD_MS); // Leer sensores cada 500ms
  }
}

void handleRoot() {
  server.send_P(200, "text/html", MAIN_page);
}

void handleData() {
  String status = "Monitoreo OK";
  if (sensorData.temp > TEMP_HIGH) status = "Temp alta";
  else if (sensorData.temp < TEMP_LOW) status = "Temp baja";
  else if (sensorData.humi < HUMI_LOW) status = "Humedad Baja";
  else if (alarmActive) status = "ALERTA INCENDIO!";

  String json = "{";
  json += "\"temp\":" + String(sensorData.temp) + ",";
  json += "\"humi\":" + String(sensorData.humi) + ",";
  json += "\"co\":" + String(sensorData.co) + ",";
  json += "\"status\":\"" + status + "\",";
  json += "\"alarmActive\":" + String(alarmActive ? "true" : "false") + ",";
  json += "\"lcd\":\"" + status + "\\nTemp: " + String(sensorData.temp) + " 'C\\nHumi: " + String(sensorData.humi) + " %\\nMQ2 = " + String(sensorData.co) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleHistory() {
  String json = "[";
  int count = 0;
  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (historyIndex - i - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if (timestampHistory[idx] == 0) continue;
    if (count > 0) json += ",";
    json += "{";
    json += "\"timestamp\":" + String(timestampHistory[idx]) + ",";
    json += "\"temp\":" + String(tempHistory[idx]) + ",";
    json += "\"humi\":" + String(humiHistory[idx]) + ",";
    json += "\"co\":" + String(coHistory[idx]) + ",";
    json += "\"status\":\"" + statusHistory[idx] + "\"";
    json += "}";
    count++;
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleNotifications() {
  String json = "[";
  for (int i = 0; i < notificationCount; i++) {
    if (i > 0) json += ",";
    json += "\"" + notifications[i] + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleDisableAlarm() {
  alarmTriggered = false;
  alarmActive = false;
  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);
  addNotification("Alarma desactivada manualmente");
  server.send(200, "text/plain", "Alarma desactivada");
}

void addNotification(String message) {
  for (int i = 4; i > 0; i--) notifications[i] = notifications[i-1];
  unsigned long currentTime = millis();
  String timeString = String(currentTime / 60000) + "m " + String((currentTime / 1000) % 60) + "s";
  notifications[0] = "[" + timeString + "] " + message;
  if (notificationCount < 5) notificationCount++;
}

void addToHistory(float temp, float humi, int co, String status) {
  tempHistory[historyIndex] = temp;
  humiHistory[historyIndex] = humi;
  coHistory[historyIndex] = co;
  statusHistory[historyIndex] = status;
  timestampHistory[historyIndex] = millis();
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
}

void setRGB(int red, int green, int blue) {
  analogWrite(RED_PIN, red);
  analogWrite(GREEN_PIN, green);
  analogWrite(BLUE_PIN, blue);
}

void setup() {
  Serial.begin(115200);
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLAME_PIN, INPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  MQ2.setRegressionMethod(1);
  MQ2.setA(36974); 
  MQ2.setB(-3.109);
  MQ2.init(); 

  lcd.init();
  lcd.backlight();
  lcd.setCursor(4, 0);
  lcd.print("----*----");
  lcd.setCursor(2, 1);
  lcd.print("Alarm System");
  delay(1000);
  lcd.clear();

  Serial.print("Calibrating MQ2...");
  float calcR0 = 0;
  for(int i = 1; i <= 10; i++) {
    MQ2.update();
    calcR0 += MQ2.calibrate(RatioMQ2CleanAir);
    Serial.print(".");
  }
  MQ2.setR0(calcR0 / 10);
  Serial.println("\nCalibration complete. R0 = " + String(calcR0 / 10));

  attachInterrupt(digitalPinToInterrupt(FLAME_PIN), triggerAlarm, FALLING);
  lcd.createChar(0, Alert0);
  lcd.createChar(1, Alert1);

  
  Serial.println("Conectando a WiFi...");
  lcd.setCursor(0, 0);
  lcd.print("Conectando a");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    WiFi.begin(SSID, PASSWORD);
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado a WiFi! IP: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Conectado");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println("\nFallo al conectar a WiFi.");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Fallo WiFi");
    while (true) delay(1000);
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/notifications", handleNotifications);
  server.on("/disableAlarm", handleDisableAlarm);
  server.begin();
  Serial.println("Servidor web iniciado.");

  xTaskCreate(readSensorsTask, "ReadSensorsTask", 2048, NULL, 1, NULL);
}

void loop() {
  server.handleClient();

  if (dataReady) {
    dataReady = false;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Monitoreo OK");
    lcd.setCursor(0, 1);
    lcd.print("Temp: ");
    lcd.print(sensorData.temp);
    lcd.print("'C");
    delay(1000);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Monitoreo OK");
    lcd.setCursor(0, 1);
    lcd.print("Humi: ");
    lcd.print(sensorData.humi);
    lcd.print(" %");
    delay(1000);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Monitoreo OK");
    lcd.setCursor(0, 1);
    lcd.print("MQ2 = ");
    lcd.print(sensorData.co);
    delay(1000);
    lcd.clear();
    Serial.println("Temp: " + String(sensorData.temp) + " Humi: " + String(sensorData.humi) + " CO: " + String(sensorData.co));

    tempReadings[index_gas] = sensorData.temp;
    humiReadings[index_gas] = sensorData.humi;
    coReadings[index_gas] = sensorData.co;
    index_gas = (index_gas + 1) % numReadings;

    float tempDiff = tempReadings[index_gas] - tempReadings[(index_gas + 1) % numReadings];
    float humiDiff = humiReadings[index_gas] - humiReadings[(index_gas + 1) % numReadings];
    int coDiff = coReadings[index_gas] - coReadings[(index_gas + 1) % numReadings];

    bool fireDetected = digitalRead(FLAME_PIN) == FIRE_THRESHOLD;
    bool highTemp = sensorData.temp > TEMP_HIGH;
    bool lowTemp = sensorData.temp < TEMP_LOW;
    bool lowHumi = sensorData.humi < HUMI_LOW;
    bool highCO = sensorData.co > 30;
    bool rapidChange = (tempDiff > 2) || (humiDiff < -5) || (coDiff > 5);

    String status = "Monitoreo OK";
    if (highTemp) {
      status = "Temp alta";
    	lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Monitoreo OK");
      lcd.setCursor(0, 1);
      lcd.print("Temp alta");
      lcd.setCursor(14,1);
      lcd.write(byte(0));
      lcd.setCursor(15,1);
      lcd.write(byte(1));
      delay(1000);
      setRGB(255, 0, 0);
      lcd.clear();
    } else if (lowTemp) {
      status = "Temp baja";
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Monitoreo OK");
      lcd.setCursor(0, 1);
      lcd.print("Temp baja");
      lcd.setCursor(14,1);
      lcd.write(byte(0));
      lcd.setCursor(15,1);
      lcd.write(byte(1));
      delay(1000);
      setRGB(255, 0, 0);
      lcd.clear();  
    } else if (lowHumi) {
      status = "Humedad Baja";
      digitalWrite(LED_PIN, HIGH);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Monitoreo OK");
      lcd.setCursor(0, 1);
      lcd.print("Humedad Baja");
      lcd.setCursor(14,1);
      lcd.write(byte(0));
      lcd.setCursor(15,1);
      lcd.write(byte(1));
      delay(1000);
    } else {
      setRGB(0, 255, 0);
      digitalWrite(LED_PIN, LOW);
    }

    if (fireDetected || (highTemp && lowHumi) || (rapidChange && highCO)) {
      triggerAlarm();
    }

    if (alarmTriggered && !alarmActive) {
      alarmActive = true;
      addNotification("¡ALARMA DE INCENDIO ACTIVADA!");
      tone(BUZZER_PIN, 1000);
      digitalWrite(LED_PIN, HIGH);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Alerta incendio!!");
      lcd.setCursor(0, 1);
      delay(1000);
    }

    if (alarmActive) {
      status = "ALERTA INCENDIO!";
    }

    if (millis() - lastHistoryUpdate >= historyInterval) {
      addToHistory(sensorData.temp, sensorData.humi, sensorData.co, status);
      lastHistoryUpdate = millis();
    }
  }
}