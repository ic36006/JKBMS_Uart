#include <Arduino.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"

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
float remainCap = 0;
float cellAve = 0, diffVtg = 0;

// แก้ไขจาก String เป็น Static Char Array เพื่อป้องกัน Memory Leak
char cellsString[150] = ""; 

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

SemaphoreHandle_t bmsMutex;
bool bmsDataReady = false;
int bmsCycleCounter = 0;           // ใช้ป้องกันส่งข้อมูลก่อนอ่านครบรอบแรก

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 15000;

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
      int activeCells = 0;
      
      // ป้องกัน Heap Fragmentation โดยใช้ snprintf เขียนลง Array คงที่
      int offset = 0;
      cellsString[0] = '\0'; 

      for (int i = 0; i < CELL_COUNT; i++) {
        float cellV = ((data[i * 2] << 8) | data[(i * 2) + 1]) / 1000.0;
        
        if (i > 0) {
          offset += snprintf(cellsString + offset, sizeof(cellsString) - offset, ",");
        }
        offset += snprintf(cellsString + offset, sizeof(cellsString) - offset, "%.3f", cellV);

        if (cellV > 0) {
          sumV += cellV;
          if (cellV > maxV) maxV = cellV;
          if (cellV < minV) minV = cellV;
          activeCells++;
        }
      }
      if (activeCells > 0) {
        cellAve = sumV / activeCells;
        diffVtg = maxV - minV;
      }
      break;
    }
  }
}

// ==================== Task อ่าน BMS (Core 0) ====================
int currentReq = 0;
uint8_t buf[128];
int bufIdx = 0;
unsigned long requestStartTime = 0;
bool waitingForResponse = false;

void Task_ReadBMS(void *pvParameters) {
  esp_task_wdt_add(NULL);

  for (;;) {
    if (currentReq >= NUM_REGS) {
      if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
        bmsDataReady = true;
        bmsCycleCounter++;           
        xSemaphoreGive(bmsMutex);
      }
      currentReq = 0;
      bufIdx = 0;
      vTaskDelay(30 / portTICK_PERIOD_MS);
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
          uint16_t calc = modbusCRC16(buf, expected-2);
          uint16_t rec  = buf[expected-2] | (buf[expected-1] << 8);

          if (calc == rec) {
            if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
              parseBMSData(currentReq, &buf[3]);
              xSemaphoreGive(bmsMutex);
            }
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
    vTaskDelay(8 / portTICK_PERIOD_MS);
  }
}

// ==================== Task ส่งข้อมูล (Core 1) ====================
void Task_SendData(void *pvParameters) {
  esp_task_wdt_add(NULL);

  for (;;) {
    if (bmsDataReady && bmsCycleCounter >= 1 && (millis() - lastSend > SEND_INTERVAL) && WiFi.status() == WL_CONNECTED) {
      
      // 1. สร้างตัวแปร Local มารองรับการ Copy ข้อมูลอย่างรวดเร็ว (ลดระยะเวลาถือ Mutex)
      float l_totalVoltage, l_current, l_power, l_temp1, l_temp2, l_mosTemp, l_remainCap, l_cellAve, l_diffVtg;
      int l_soc, l_cycleCount;
      char l_cellsString[150];

      if (xSemaphoreTake(bmsMutex, portMAX_DELAY)) {
        l_totalVoltage = totalVoltage; 
        l_current = current; 
        l_power = power;
        l_soc = soc; 
        l_temp1 = temp1; 
        l_temp2 = temp2; 
        l_mosTemp = mosTemp;
        l_remainCap = remainCap; 
        l_cycleCount = cycleCount;
        l_cellAve = cellAve; 
        l_diffVtg = diffVtg;
        strlcpy(l_cellsString, cellsString, sizeof(l_cellsString));
        
        bmsDataReady = false; 
        xSemaphoreGive(bmsMutex); // 2. ปล่อย Mutex ทันทีให้ Core 0 วิ่งต่อ ไม่เกิดปัญหาคอขวด
      }

      // 3. ดำเนินการด้าน JSON ข้างนอก Mutex ป้องกัน Task อื่นค้าง
      StaticJsonDocument<768> doc;
      doc["total_v"] = l_totalVoltage;
      doc["current"] = l_current;
      doc["power"] = l_power;
      doc["soc"] = l_soc;
      doc["temp1"] = l_temp1;
      doc["temp2"] = l_temp2;
      doc["mos_temp"] = l_mosTemp;
      doc["remain_cap"] = l_remainCap;
      doc["cycle_count"] = l_cycleCount;
      doc["cell_ave"] = l_cellAve;
      doc["diff_v"] = l_diffVtg;
      doc["cells_v"] = l_cellsString;

      String json;
      serializeJson(doc, json);

      // 4. ตั้งค่า HTTP Client ให้ปลอดภัยต่อ Network Leak และดักการ Redirect
      HTTPClient http;
      http.setReuse(false); 
      http.setTimeout(15000); 
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // สำคัญมากสำหรับ Google Apps Script!

      if (http.begin(appScriptUrl)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.POST(json);
        Serial.printf("[Send] HTTP Code: %d | Cycle: %d\n", code, bmsCycleCounter);
        
        if (code > 0) {
           String response = http.getString(); // เคลียร์บัฟเฟอร์การตอบรับ
        }
        http.end(); // ปิด Connection ป้องกันพอร์ตเต็ม
      } else {
        Serial.println("[Send] HTTP Begin Failed!");
      }

      lastSend = millis();
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
  static unsigned long buttonPressTime = 0;
  static bool buttonWasPressed = false;

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!buttonWasPressed) {
      buttonPressTime = millis();
      buttonWasPressed = true;
    } else if (millis() - buttonPressTime > 3000) {
      wm.resetSettings();
      preferences.clear();
      ESP.restart();
    }
  } else {
    buttonWasPressed = false;
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  preferences.begin("bms_config", false);
  appScriptUrl = preferences.getString("url", "");

  custom_gscript.setValue(appScriptUrl.c_str(), 300);
  wm.addParameter(&custom_gscript);
  wm.setSaveParamsCallback(saveConfigCallback);

  if (wm.autoConnect("JK_BMS_Setup")) {
    if (appScriptUrl == "") appScriptUrl = custom_gscript.getValue();
    Serial2.begin(115200, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);
    Serial.println("WiFi Connected - BMS Started");
  }

  bmsMutex = xSemaphoreCreateMutex();
  delay(1500);

  // ขยาย Stack ของ SendData เป็น 8192 เพื่อให้สามารถทำงานกับ HTTPS (SSL) ได้อย่างปลอดภัย
  xTaskCreatePinnedToCore(Task_ReadBMS, "ReadBMS", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_SendData, "SendData", 8192, NULL, 1, NULL, 1);

  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 30000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
}

// ==================== LOOP ====================
void loop() {
  checkFactoryReset();
  wm.process();

  // --- เช็คสถานะการเชื่อมต่อ WiFi และ Auto-Reboot เมื่อหลุดเกิน 5 นาที ---
  static unsigned long lastConnectedTime = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    lastConnectedTime = millis(); 
  } else {
    if (millis() - lastConnectedTime > 300000) {
      Serial.println("[System] WiFi disconnected for 5 mins. Rebooting...");
      delay(1000);
      ESP.restart(); 
    }
  }
  // ---------------------------------------------------------------

  esp_task_wdt_reset();
  vTaskDelay(10 / portTICK_PERIOD_MS);
}
