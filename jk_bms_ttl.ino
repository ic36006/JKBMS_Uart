// ==================== JK_BMS_Log_V0.2 ====================
// completeness check ก่อนส่งข้อมูล + HTTP server พอร์ต 8080
// + ปุ่ม Test บน Hotspot (แทน Update)
// + Google App Script URL รวมในหน้า Configure WiFi

#include <Arduino.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include <vector>

// ==================== กำหนดขา ====================
#define BMS_RX_PIN 16
#define BMS_TX_PIN 17
#define CELL_COUNT 16
#define BUTTON_PIN 0
#define LED_PIN 2

// ==================== ตัวแปร BMS ====================
float totalVoltage = 0, current = 0, power = 0;
float temp1 = 0, temp2 = 0, mosTemp = 0;
int soc = 0, cycleCount = 0;
float remainCap = 0, cellAve = 0, diffVtg = 0;
char cellsBuf[128] = "";

volatile unsigned long lastUpdateMillis = 0;

struct BMSRegister {
  uint16_t addr;
  uint16_t numRegs;
  const char* name;
};

BMSRegister bmsRegs[] = {
  {0x1290, 2, "Total Voltage"}, {0x1294, 2, "Power"}, {0x1298, 2, "Current"},
  {0x129C, 1, "Temp T1"}, {0x129E, 1, "Temp T2"}, {0x128A, 1, "MOS Temp"},
  {0x12A6, 1, "SOC"}, {0x12A8, 2, "Remaining Capacity"},
  {0x12B0, 2, "Cycle Count"}, {0x1200, 16, "Cells Voltage"}
};
const int NUM_REGS = sizeof(bmsRegs) / sizeof(bmsRegs[0]);

// ==================== ตัวแปรระบบ ====================
WiFiManager wm;
WiFiManagerParameter custom_gscript("gscript", "Google App Script URL", "", 300);
Preferences preferences;
String appScriptUrl = "";

WebServer dataServer(8080);

SemaphoreHandle_t bmsMutex;
bool bmsDataReady = false;
int bmsCycleCounter = 0;
bool hasValidData = false;

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 30000;

// ==================== ฟังก์ชัน BMS ====================
uint16_t modbusCRC16(uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; } else { crc >>= 1; }
    }
  }
  return crc;
}

void sendModbusRead(uint8_t slaveAddr, uint16_t regAddr, uint16_t numRegs) {
  uint8_t frame[8];
  frame[0] = slaveAddr; frame[1] = 0x03;
  frame[2] = (regAddr >> 8) & 0xFF; frame[3] = regAddr & 0xFF;
  frame[4] = (numRegs >> 8) & 0xFF; frame[5] = numRegs & 0xFF;
  uint16_t crc = modbusCRC16(frame, 6);
  frame[6] = crc & 0xFF; frame[7] = (crc >> 8) & 0xFF;
  while (Serial2.available()) Serial2.read();
  Serial2.write(frame, 8);
}

void parseBMSData(int reqIdx, uint8_t* data) {
  switch (reqIdx) {
    case 0: totalVoltage = (((uint32_t)data[0]<<24)|(uint32_t)data[1]<<16|(uint32_t)data[2]<<8|data[3])/1000.0; break;
    case 1: power = (((int32_t)data[0]<<24)|(int32_t)data[1]<<16|(int32_t)data[2]<<8|data[3])/1000.0; break;
    case 2: current = (((int32_t)data[0]<<24)|(int32_t)data[1]<<16|(int32_t)data[2]<<8|data[3])/1000.0; break;
    case 3: temp1 = (int16_t)((data[0]<<8)|data[1])/10.0; break;
    case 4: temp2 = (int16_t)((data[0]<<8)|data[1])/10.0; break;
    case 5: mosTemp = (int16_t)((data[0]<<8)|data[1])/10.0; break;
    case 6: soc = data[1]; break;
    case 7: remainCap = (((uint32_t)data[0]<<24)|(uint32_t)data[1]<<16|(uint32_t)data[2]<<8|data[3])/1000.0; break;
    case 8: cycleCount = (((uint32_t)data[0]<<24)|(uint32_t)data[1]<<16|(uint32_t)data[2]<<8|data[3]); break;
    case 9: {
      float sumV = 0, maxV = -1, minV = 10;
      int active = 0;
      cellsBuf[0] = '\0';
      char temp[10];
      for (int i = 0; i < CELL_COUNT; i++) {
        float cellV = ((data[i*2]<<8) | data[i*2+1]) / 1000.0;
        if (i > 0) strcat(cellsBuf, ",");
        dtostrf(cellV, 1, 3, temp);
        strcat(cellsBuf, temp);
        if (cellV > 0) {
          sumV += cellV;
          if (cellV > maxV) maxV = cellV;
          if (cellV < minV) minV = cellV;
          active++;
        }
      }
      if (active > 0) {
        cellAve = sumV / active;
        diffVtg = maxV - minV;
      }
      break;
    }
  }
}

// ----- ใช้เฉพาะตอนกดปุ่ม Test บน Hotspot -----
bool readBMSOnceBlocking(unsigned long timeoutPerReg = 300) {
  while (Serial2.available()) Serial2.read();
  int ok = 0;
  for (int req = 0; req < NUM_REGS; req++) {
    sendModbusRead(0x01, bmsRegs[req].addr, bmsRegs[req].numRegs);
    uint8_t rbuf[128];
    int ridx = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutPerReg) {
      while (Serial2.available() && ridx < (int)sizeof(rbuf)) {
        rbuf[ridx++] = Serial2.read();
      }
      if (ridx >= 3) {
        uint8_t expected = rbuf[2] + 5;
        if (ridx >= expected) {
          uint16_t calc = modbusCRC16(rbuf, expected - 2);
          uint16_t rec  = rbuf[expected - 2] | (rbuf[expected - 1] << 8);
          if (calc == rec) {
            parseBMSData(req, &rbuf[3]);
            ok++;
          }
          break;
        }
      }
      delay(5);
    }
    delay(20);
  }
  return (ok == NUM_REGS);
}

void handleBMSTest() {
  bool ok = readBMSOnceBlocking(300);

  String html;
  html.reserve(2500);
  html += F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>BMS Test</title>"
    "<style>"
    "body{font-family:monospace;background:#111;color:#0f0;margin:12px;font-size:14px;}"
    "pre{white-space:pre-wrap;word-break:break-all;line-height:1.5;}"
    ".ok{color:#0f0;}.fail{color:#f44;}"
    "a{display:inline-block;margin:8px 8px 0 0;padding:10px 16px;"
    "background:#0a0;color:#000;text-decoration:none;font-weight:bold;}"
    "</style></head><body><h2>JK BMS Test</h2>"
  );

  if (ok) {
    html += F("<p class='ok'>OK — อ่านครบ ต่อวงจรได้</p><pre>");
    html += "Timestamp       : " + String(millis()) + "\n";
    html += "Total Voltage   : " + String(totalVoltage, 3) + " V\n";
    html += "Current         : " + String(current, 3) + " A\n";
    html += "Power           : " + String(power, 2) + " W\n";
    html += "SOC             : " + String(soc) + " %\n";
    html += "Temp T1         : " + String(temp1, 1) + " C\n";
    html += "Temp T2         : " + String(temp2, 1) + " C\n";
    html += "MOS Temp        : " + String(mosTemp, 1) + " C\n";
    html += "Remain Cap      : " + String(remainCap, 3) + " Ah\n";
    html += "Cycle Count     : " + String(cycleCount) + "\n";
    html += "Cell Ave        : " + String(cellAve, 3) + " V\n";
    html += "Diff-Vtg        : " + String(diffVtg, 3) + " V\n";
    html += "Cells           :\n";

    {
      String cells = String(cellsBuf);
      int start = 0, n = 0;
      while (start < (int)cells.length()) {
        int comma = cells.indexOf(',', start);
        String one = (comma < 0) ? cells.substring(start) : cells.substring(start, comma);
        if (n % 4 == 0) html += "  ";
        html += one;
        n++;
        if (comma < 0) break;
        html += (n % 4 == 0) ? "\n" : ", ";
        start = comma + 1;
      }
      if (n % 4 != 0) html += "\n";
    }

    html += F("</pre>");
  } else {
    html += F("<p class='fail'>FAIL — อ่านไม่ครบ เช็คสาย RX/TX / baud / address</p>");
  }

  html += F("<br><a href='/test'>Test อีกครั้ง</a> <a href='/'>กลับ</a></body></html>");

  if (wm.server) wm.server->send(200, "text/html", html);
}

void webServerCallback() {
  wm.server->on("/test", HTTP_GET, handleBMSTest);
}

// ==================== HTTP handler สำหรับผู้มา pull ข้อมูล ====================
void handleBMSData() {
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    StaticJsonDocument<768> doc;
    doc["total_v"]         = totalVoltage;
    doc["current"]         = current;
    doc["power"]           = power;
    doc["soc"]             = soc;
    doc["temp1"]           = temp1;
    doc["temp2"]           = temp2;
    doc["mos_temp"]        = mosTemp;
    doc["remain_cap"]      = remainCap;
    doc["cycle_count"]     = cycleCount;
    doc["cell_ave"]        = cellAve;
    doc["diff_v"]          = diffVtg;
    doc["cells_v"]         = cellsBuf;
    doc["data_age_ms"]     = millis() - lastUpdateMillis;
    doc["has_valid_data"]  = hasValidData;
    xSemaphoreGive(bmsMutex);

    char jsonBuf[900];
    serializeJson(doc, jsonBuf, sizeof(jsonBuf));
    dataServer.send(200, "application/json", jsonBuf);
  } else {
    dataServer.send(503, "application/json", "{\"error\":\"busy\"}");
  }
}

// ==================== Task อ่าน BMS (Core 0) ====================
int currentReq = 0;
uint8_t buf[128];
int bufIdx = 0;
unsigned long requestStartTime = 0;
bool waitingForResponse = false;
int successCountThisCycle = 0;

void Task_ReadBMS(void *pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    if (currentReq >= NUM_REGS) {
      if (successCountThisCycle == NUM_REGS) {
        if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
          bmsDataReady = true;
          bmsCycleCounter++;
          hasValidData = true;
          lastUpdateMillis = millis();
          xSemaphoreGive(bmsMutex);
        }
      } else {
        Serial.printf("[BMS] Incomplete cycle: %d/%d OK - skip this round\n",
                      successCountThisCycle, NUM_REGS);
      }
      currentReq = 0;
      bufIdx = 0;
      successCountThisCycle = 0;
      vTaskDelay(50 / portTICK_PERIOD_MS);
      esp_task_wdt_reset();
      continue;
    }

    if (!waitingForResponse) {
      while (Serial2.available()) Serial2.read();
      sendModbusRead(0x01, bmsRegs[currentReq].addr, bmsRegs[currentReq].numRegs);
      bufIdx = 0;
      requestStartTime = millis();
      waitingForResponse = true;
    } else {
      while (Serial2.available() && bufIdx < sizeof(buf)) {
        buf[bufIdx++] = Serial2.read();
      }

      if (bufIdx >= 3) {
        uint8_t expected = buf[2] + 5;
        if (bufIdx >= expected) {
          uint16_t calc = modbusCRC16(buf, expected - 2);
          uint16_t rec = buf[expected - 2] | (buf[expected - 1] << 8);
          if (calc == rec) {
            if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
              parseBMSData(currentReq, &buf[3]);
              xSemaphoreGive(bmsMutex);
            }
            successCountThisCycle++;
          }
          currentReq++;
          waitingForResponse = false;
          bufIdx = 0;
        }
      }

      if (millis() - requestStartTime > 250) {
        currentReq++;
        waitingForResponse = false;
        bufIdx = 0;
      }

      if (bufIdx >= sizeof(buf)) {
        bufIdx = 0;
        waitingForResponse = false;
        currentReq++;
      }
    }
    esp_task_wdt_reset();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ==================== Task ส่งข้อมูล (Core 1) ====================
void Task_SendData(void *pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    if (hasValidData && bmsDataReady && bmsCycleCounter >= 1 &&
        (millis() - lastSend > SEND_INTERVAL)) {

      if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
        vTaskDelay(4000 / portTICK_PERIOD_MS);
      }

      if (WiFi.status() == WL_CONNECTED) {
        if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
          StaticJsonDocument<768> doc;
          doc["total_v"] = totalVoltage;
          doc["current"] = current;
          doc["power"] = power;
          doc["soc"] = soc;
          doc["temp1"] = temp1;
          doc["temp2"] = temp2;
          doc["mos_temp"] = mosTemp;
          doc["remain_cap"] = remainCap;
          doc["cycle_count"] = cycleCount;
          doc["cell_ave"] = cellAve;
          doc["diff_v"] = diffVtg;
          doc["cells_v"] = cellsBuf;

          char json[1024];
          serializeJson(doc, json, sizeof(json));
          xSemaphoreGive(bmsMutex);

          WiFiClientSecure client;
          client.setInsecure();

          HTTPClient http;
          http.begin(client, appScriptUrl);
          http.addHeader("Content-Type", "application/json");
          http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

          int code = http.POST(json);

          Serial.printf("[Send %d] Code:%d | Power:%.2f | Heap:%d\n",
                        bmsCycleCounter, code, power, ESP.getFreeHeap());
          http.end();

          lastSend = millis();
          bmsDataReady = false;
        }
      }
    }
    esp_task_wdt_reset();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ==================== ฟังก์ชันอื่น ๆ ====================
void saveConfigCallback() {
  appScriptUrl = custom_gscript.getValue();
  preferences.putString("url", appScriptUrl);
}

void checkFactoryReset() {
  static unsigned long t = 0;
  static bool pressed = false;
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!pressed) { t = millis(); pressed = true; }
    else if (millis() - t > 3000) {
      wm.resetSettings();
      preferences.clear();
      ESP.restart();
    }
  } else pressed = false;
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  // เปิด Serial2 ก่อน portal เพื่อให้ปุ่ม Test อ่าน BMS ได้
  Serial2.begin(115200, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 45000,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);

  preferences.begin("bms_config", false);
  appScriptUrl = preferences.getString("url", "");

  custom_gscript.setValue(appScriptUrl.c_str(), 300);
  wm.addParameter(&custom_gscript);
  wm.setSaveParamsCallback(saveConfigCallback);

  // App Script รวมในหน้า Configure WiFi — ไม่มี Setup แยก, ไม่มี Update
  wm.setParamsPage(false);
  std::vector<const char*> menu = {"wifi", "custom", "info", "exit"};
  wm.setMenu(menu);
  wm.setCustomMenuHTML(
    "<form action='/test' method='get'><button type='submit'>Test</button></form><br/>\n"
  );
  wm.setWebServerCallback(webServerCallback);

  Serial.println("Starting JK BMS...");

  if (wm.autoConnect("JK_BMS_Setup")) {
    Serial.println("WiFi Connected");
    if (appScriptUrl == "") appScriptUrl = custom_gscript.getValue();

    dataServer.on("/", HTTP_GET, handleBMSData);
    dataServer.on("/bms", HTTP_GET, handleBMSData);
    dataServer.begin();
    Serial.println("BMS data server started on port 8080");
  }

  bmsMutex = xSemaphoreCreateMutex();
  delay(1500);

  xTaskCreatePinnedToCore(Task_ReadBMS, "ReadBMS", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_SendData, "SendData", 8192, NULL, 1, NULL, 1);

  wdt_config.timeout_ms = 30000;
  esp_task_wdt_reconfigure(&wdt_config);

  Serial.println("System Ready.");

  esp_task_wdt_add(NULL);
}

// ==================== LOOP ====================
void loop() {
  checkFactoryReset();
  wm.process();
  dataServer.handleClient();
  esp_task_wdt_reset();
  vTaskDelay(10 / portTICK_PERIOD_MS);
}
