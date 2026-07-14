function doGet(e) {
  if (e && e.parameter && e.parameter.soc) {
    try {
      var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
      var timestamp = new Date();
      
      var soc = e.parameter.soc || "0";
      var voltage = e.parameter.totalVoltage || "0";
      var current = e.parameter.current || "0";
      var power = e.parameter.power || "0";
      var remainAh = e.parameter.capacityRemain || "0";
      var fullAh = e.parameter.capacityFull || "0";
      var tempMos = e.parameter.mosTemp || "0";
      var temp1 = e.parameter.temp1 || "0";
      var temp2 = e.parameter.temp2 || "0";
      var maxV = e.parameter.maxV || "0";
      var minV = e.parameter.minV || "0";
      var deltaV = e.parameter.deltaV || "0";
      var balCurrent = e.parameter.balanceCurrent || "0";
      var cellV = e.parameter.cellV || "";
      var wireR = e.parameter.wireR || "";

      sheet.appendRow([
        timestamp, soc, voltage, current, power, remainAh, fullAh, 
        tempMos, temp1, temp2, maxV, minV, deltaV, balCurrent, cellV, wireR
      ]);

      return ContentService.createTextOutput("Success");
    } catch (error) {
      return ContentService.createTextOutput("Error: " + error.toString());
    }
  } else {
    return HtmlService.createTemplateFromFile('Index')
        .evaluate()
        .setTitle('BMS Smart Battery Dashboard')
        .addMetaTag('viewport', 'width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no');
  }
}

function include(filename) {
  return HtmlService.createHtmlOutputFromFile(filename).getContent();
}

function getBatteryData(timeRange) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var rows = sheet.getDataRange().getValues();
  
  if (rows.length < 2) return null;
  
  var latestRow = rows[rows.length - 1];
  var latestTimestamp = new Date(latestRow[0]).getTime();
  
  var latestData = {
    timestamp: latestTimestamp, 
    soc: Number(latestRow[1]) || 0,
    voltage: Number(latestRow[2]) || 0,
    current: Number(latestRow[3]) || 0,
    power: Number(latestRow[4]) || 0,
    remainAh: Number(latestRow[5]) || 0,
    fullAh: Number(latestRow[6]) || 0,
    tempMos: Number(latestRow[7]) || 0,
    temp1: Number(latestRow[8]) || 0,
    temp2: Number(latestRow[9]) || 0,
    maxV: Number(latestRow[10]) || 0,
    minV: Number(latestRow[11]) || 0,
    diffV: Number(latestRow[12]) || 0,
    balCurrent: Number(latestRow[13]) || 0,
    cellVoltages: latestRow[14] || "" 
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
         voltage: Number(matchedRows[j][2]) || 0
       });
       lastAddedTime = t;
     }
  }
  
  return {
    latest: latestData,
    history: historyData
  };
}

function autoDeleteOldData() {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
  var maxRows = 172800; 
  var numRows = sheet.getLastRow();
  if (numRows > maxRows) {
    sheet.deleteRows(2, numRows - maxRows); 
  }
}