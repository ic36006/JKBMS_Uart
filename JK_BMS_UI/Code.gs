// ============ CONFIG / DEFAULTS ============
var DEFAULT_SETTINGS = {
  batteryName: "BATTERY",
  cellCount: 16,      // 4, 8, หรือ 16
  capacityAh: 280,
  retentionDays: 30   // 10, 15, หรือ 30
};

var TRIGGER_HANDLER = "autoDeleteOldData";

// ============ รับข้อมูลจาก ESP32 ============
function doPost(e) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  try {
    var data = JSON.parse(e.postData.contents);
    var rowData = [
      new Date(),
      data.total_v,
      data.current,
      data.power,
      data.soc,
      data.temp1,
      data.temp2,
      data.mos_temp,
      data.remain_cap,
      data.cycle_count,
      data.cell_ave,
      data.diff_v,
      data.cells_v
    ];
    sheet.appendRow(rowData);
    return ContentService.createTextOutput("Success").setMimeType(ContentService.MimeType.TEXT);
  } catch (error) {
    return ContentService.createTextOutput("Error: " + error.toString()).setMimeType(ContentService.MimeType.TEXT);
  }
}

// ============ doGet (รวมเป็นฟังก์ชันเดียว - แก้บั๊กซ้ำ) ============
function doGet(e) {
  // ESP32 ดึงข้อมูลล่าสุดผ่าน ?type=api
  if (e.parameter.type === 'api') {
    var payload = getBatteryData('1hr', 'voltage'); // ใช้ logic เดียวกับ Dashboard ดึงข้อมูลจริง
    return ContentService.createTextOutput(JSON.stringify({ latest: payload ? payload.latest : null }))
                         .setMimeType(ContentService.MimeType.JSON);
  }

  // หน้า Dashboard ปกติ
  return HtmlService.createTemplateFromFile('Index')
      .evaluate()
      .setTitle('BMS Smart Battery Dashboard')
      .addMetaTag('viewport', 'width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no')
      .setXFrameOptionsMode(HtmlService.XFrameOptionsMode.ALLOWALL);
}

function include(filename) {
  return HtmlService.createHtmlOutputFromFile(filename).getContent();
}

// ============ SETTINGS ============
function getSettings() {
  var props = PropertiesService.getScriptProperties();
  var stored = props.getProperties();
  return {
    batteryName: stored.batteryName || DEFAULT_SETTINGS.batteryName,
    cellCount: parseInt(stored.cellCount) || DEFAULT_SETTINGS.cellCount,
    capacityAh: parseFloat(stored.capacityAh) || DEFAULT_SETTINGS.capacityAh,
    retentionDays: parseInt(stored.retentionDays) || DEFAULT_SETTINGS.retentionDays
  };
}

function saveSettings(newSettings) {
  var props = PropertiesService.getScriptProperties();
  props.setProperties({
    batteryName: String(newSettings.batteryName || DEFAULT_SETTINGS.batteryName),
    cellCount: String(newSettings.cellCount || DEFAULT_SETTINGS.cellCount),
    capacityAh: String(newSettings.capacityAh || DEFAULT_SETTINGS.capacityAh),
    retentionDays: String(newSettings.retentionDays || DEFAULT_SETTINGS.retentionDays)
  });
  ensureDeleteTrigger(); // ตั้ง/เช็ค trigger รายวันให้อัตโนมัติ ไม่ต้องกังวลว่ามีอยู่แล้วหรือยัง
  return getSettings();
}

// ============ ดึงข้อมูลสำหรับ Dashboard ============
// metric: 'voltage' | 'current' | 'power' | 'soc' | 'temp1' | 'temp2' | 'mosTemp' | 'remainAh'
var METRIC_COLUMN = {
  voltage: 1, current: 2, power: 3, soc: 4,
  temp1: 5, temp2: 6, mosTemp: 7, remainAh: 8
};

function getBatteryData(timeRange, metric) {
  var settings = getSettings();
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var rows = sheet.getDataRange().getValues();
  if (rows.length < 2) return null;

  var latestRow = rows[rows.length - 1];
  var latestTimestamp = new Date(latestRow[0]).getTime();

  var cellVoltagesStr = latestRow[12] || "";
  var allCells = cellVoltagesStr.toString().split(',').map(Number);
  var activeCells = allCells.slice(0, settings.cellCount).filter(function(n) { return !isNaN(n) && n > 0; });

  var minV = activeCells.length ? Math.min.apply(null, activeCells) : 0;
  var maxV = activeCells.length ? Math.max.apply(null, activeCells) : 0;

  var latestData = {
    timestamp: latestTimestamp,
    voltage: Number(latestRow[1]) || 0,
    current: Number(latestRow[2]) || 0,
    power: Number(latestRow[3]) || 0,
    soc: Number(latestRow[4]) || 0,
    temp1: Number(latestRow[5]) || 0,
    temp2: Number(latestRow[6]) || 0,
    tempMos: Number(latestRow[7]) || 0,
    remainAh: Number(latestRow[8]) || 0,
    cycleCount: Number(latestRow[9]) || 0,
    cellAve: Number(latestRow[10]) || 0,
    diffV: Number(latestRow[11]) || 0,
    cellVoltages: allCells.slice(0, settings.cellCount).join(','),
    minV: minV,
    maxV: maxV,
    capacityAh: settings.capacityAh,
    batteryName: settings.batteryName,
    cellCount: settings.cellCount
  };

  var timeThreshold = 0;
  var intervalMs = 60000;
  if (timeRange === '1Day') {
    timeThreshold = latestTimestamp - (24 * 60 * 60 * 1000);
    intervalMs = 15 * 60000;
  } else if (timeRange === '7D') {
    timeThreshold = latestTimestamp - (7 * 24 * 60 * 60 * 1000);
    intervalMs = 60 * 60000;
  } else {
    timeThreshold = latestTimestamp - (60 * 60 * 1000);
  }

  var col = METRIC_COLUMN[metric] !== undefined ? METRIC_COLUMN[metric] : 1;

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
        value: Number(matchedRows[j][col]) || 0
      });
      lastAddedTime = t;
    }
  }

  return { latest: latestData, history: historyData, metric: metric };
}

// ============ ลบข้อมูลเก่าตามจำนวนวันที่ตั้งไว้ ============
function autoDeleteOldData() {
  var settings = getSettings();
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var lastRow = sheet.getLastRow();
  if (lastRow < 2) return;

  var threshold = new Date().getTime() - (settings.retentionDays * 24 * 60 * 60 * 1000);
  var timestamps = sheet.getRange(2, 1, lastRow - 1, 1).getValues();

  var deleteCount = 0;
  for (var i = 0; i < timestamps.length; i++) {
    var t = new Date(timestamps[i][0]).getTime();
    if (t < threshold) {
      deleteCount++;
    } else {
      break; // ข้อมูลเรียงตามเวลา เจอแถวแรกที่ยังไม่เก่าพอ หยุดได้เลย
    }
  }

  if (deleteCount > 0) {
    sheet.deleteRows(2, deleteCount);
  }
}

// ============ ตั้ง Trigger รายวันอัตโนมัติ (เช็คก่อนกันซ้ำ) ============
function ensureDeleteTrigger() {
  var triggers = ScriptApp.getProjectTriggers();
  var exists = triggers.some(function(t) { return t.getHandlerFunction() === TRIGGER_HANDLER; });
  if (!exists) {
    ScriptApp.newTrigger(TRIGGER_HANDLER)
      .timeBased()
      .everyDays(1)
      .atHour(3) // รันตอนตี 3 ของทุกวัน
      .create();
  }
}
