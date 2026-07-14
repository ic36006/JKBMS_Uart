// ฟังก์ชันสำหรับรับข้อมูลจาก ESP32 (ยิงมาเป็นแบบ POST JSON)
function doPost(e) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  
  try {
    var data = JSON.parse(e.postData.contents);
    
    // เรียงตาม: Timestamp, Total Voltage, Current, Power, SOC, Temp T1, Temp T2, MOS Temp, Remain Cap, Cycle Count, Cell Ave, Diff-Vtg, Cell
    var rowData = [
      new Date(),            // A: Timestamp
      data.total_v,          // B: Total Voltage
      data.current,          // C: Current
      data.power,            // D: Power
      data.soc,              // E: SOC
      data.temp1,            // F: Temp T1
      data.temp2,            // G: Temp T2
      data.mos_temp,         // H: MOS Temp
      data.remain_cap,       // I: Remain Capacity
      data.cycle_count,      // J: Cycle Count
      data.cell_ave,         // K: Cell Average
      data.diff_v,           // L: Diff Voltage
      data.cells_v           // M: Cells (ก้อนเดียวคั่นด้วยลูกน้ำ)
    ];
    
    sheet.appendRow(rowData);
    return ContentService.createTextOutput("Success").setMimeType(ContentService.MimeType.TEXT);
    
  } catch (error) {
    return ContentService.createTextOutput("Error: " + error.toString()).setMimeType(ContentService.MimeType.TEXT);
  }
}

// ฟังก์ชันสำหรับเปิดหน้าเว็บ UI Dashboard
function doGet(e) {
  return HtmlService.createTemplateFromFile('Index')
      .evaluate()
      .setTitle('BMS Smart Battery Dashboard')
      .addMetaTag('viewport', 'width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no');
}

// ฟังก์ชันดึงไฟล์ HTML แทรกเข้าไปใน UI
function include(filename) {
  return HtmlService.createHtmlOutputFromFile(filename).getContent();
}

// ฟังก์ชันที่ UI JavaScript เรียกใช้เพื่อดึงข้อมูลมาแสดงผล
function getBatteryData(timeRange) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var rows = sheet.getDataRange().getValues();
  
  if (rows.length < 2) return null;
  
  var latestRow = rows[rows.length - 1];
  var latestTimestamp = new Date(latestRow[0]).getTime();
  
  // ข้อมูลเซลล์แบตเตอรี่แบบ Comma Separated อยู่ในคอลัมน์ที่ 13 (Index 12)
  var cellVoltagesStr = latestRow[12] || "";
  var minV = 0;
  var maxV = 0;
  
  // คำนวณหาค่า Low Cell (minV) และ High Cell (maxV) เพื่อส่งให้ UI
  if (cellVoltagesStr) {
    var cells = cellVoltagesStr.toString().split(',').map(Number).filter(function(n) { return !isNaN(n) && n > 0; });
    if (cells.length > 0) {
      minV = Math.min.apply(null, cells);
      maxV = Math.max.apply(null, cells);
    }
  }
  
  // ดึงค่าตาม Index ให้ตรงกับข้อมูลที่บันทึกลงไป
  var latestData = {
    timestamp: latestTimestamp, 
    voltage: Number(latestRow[1]) || 0,    // B: Total Voltage
    current: Number(latestRow[2]) || 0,    // C: Current
    power: Number(latestRow[3]) || 0,      // D: Power
    soc: Number(latestRow[4]) || 0,        // E: SOC
    temp1: Number(latestRow[5]) || 0,      // F: Temp T1
    temp2: Number(latestRow[6]) || 0,      // G: Temp T2
    tempMos: Number(latestRow[7]) || 0,    // H: MOS Temp
    remainAh: Number(latestRow[8]) || 0,   // I: Remain Capacity
    cycleCount: Number(latestRow[9]) || 0, // J: Cycle Count
    cellAve: Number(latestRow[10]) || 0,   // K: Cell Average
    diffV: Number(latestRow[11]) || 0,     // L: Diff Voltage
    cellVoltages: cellVoltagesStr,         // M: Cells
    minV: minV,                            // คำนวณมาได้
    maxV: maxV                             // คำนวณมาได้
  };
  
  var timeThreshold = 0;
  var intervalMs = 60000; // ค่าเริ่มต้น: 1 นาที
  
  if (timeRange === '1Day') {
    timeThreshold = latestTimestamp - (24 * 60 * 60 * 1000);
    intervalMs = 15 * 60000; // 1 วัน ดึงทุกๆ 15 นาที
  } else if (timeRange === '7D') {
    timeThreshold = latestTimestamp - (7 * 24 * 60 * 60 * 1000);
    intervalMs = 60 * 60000; // 7 วัน ดึงทุกๆ 1 ชั่วโมง
  } else {
    timeThreshold = latestTimestamp - (60 * 60 * 1000); // 1hr ดึงทุกๆ 1 นาที
  }

  var matchedRows = [];
  for (var i = rows.length - 1; i >= 1; i--) {
    var rowTime = new Date(rows[i][0]).getTime();
    if (rowTime >= timeThreshold) {
      matchedRows.push(rows[i]);
    } else {
      break; 
    }
  }
  matchedRows.reverse(); 

  var historyData = [];
  var lastAddedTime = 0;
  
  for (var j = 0; j < matchedRows.length; j++) {
     var t = new Date(matchedRows[j][0]).getTime();
     if (t - lastAddedTime >= intervalMs || j === matchedRows.length - 1) {
       historyData.push({
         timestamp: t,
         voltage: Number(matchedRows[j][1]) || 0 // ดึงคอลัมน์ B (Total Voltage) ไปแสดงกราฟ
       });
       lastAddedTime = t;
     }
  }
  
  return {
    latest: latestData,
    history: historyData
  };
}

// ฟังก์ชันเคลียร์ข้อมูลเก่า
function autoDeleteOldData() {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var maxRows = 172800; 
  var numRows = sheet.getLastRow();
  if (numRows > maxRows) {
    sheet.deleteRows(2, numRows - maxRows); 
  }
}

// ฟังก์ชันนี้สร้างขึ้นมาใหม่สำหรับให้ ESP32 ดึงข้อมูลโดยเฉพาะ ไม่กระทบกับระบบหน้าแดชบอร์ดเดิม
function doGet(e) {
  // ตรวจสอบว่าถ้า ESP32 เรียกมาโดยส่งพารามิเตอร์ ?type=api
  if (e.parameter.type === 'api') {
    
    // ตรงนี้ให้คุณเขียนสคริปต์ดึงค่า "ล่าสุด" จากตารางหรือตัวแปรของคุณ (อ้างอิงตามที่คุณใช้ในหน้าหลัก)
    // ตัวอย่างการส่งค่ากลับ (ให้เปลี่ยนตัวเลขสมมุติตามตัวแปรจริงของคุณนะครับ):
    var dataForESP = {
      "latest": {
        "soc": 85,          // ค่าเปอร์เซ็นต์ SoC จริง
        "voltage": 13.12,   // แรงดันไฟรวม
        "current": -2.50,   // กระแสแอมป์
        "power": -32.8,     // กำลังวัตต์
        "minV": 3.250,
        "maxV": 3.290,
        "diffV": 0.040,
        "tempMos": 38.5,
        "temp1": 31.2,
        "temp2": 31.5,
        "cellVoltages": "3.250,3.260,3.270,3.280,3.290,3.280,3.270,3.260" // แรงดัน 8 เซลล์ คั่นด้วยเครื่องหมายจุลภาค
      }
    };
    
    return ContentService.createTextOutput(JSON.stringify(dataForESP))
                         .setMimeType(ContentService.MimeType.JSON);
  }
  
  // ⛔️ ถ้าเรียกมาปกติ (ไม่มีพารามิเตอร์) ให้ส่งหน้าเว็บเดิมกลับไป ไม่มีการแก้ไขใดๆ ทั้งสิ้น
  return HtmlService.createTemplateFromFile('Index').evaluate()
                    .setTitle('BMS Smart Battery Dashboard')
                    .setXFrameOptionsMode(HtmlService.XFrameOptionsMode.ALLOWALL);
}
