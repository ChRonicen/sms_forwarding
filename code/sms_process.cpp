#include "sms_process.h"
#include "web_handlers.h"
#include "modem.h"
#include "web_handlers.h"
#include "push.h"
#include "web_handlers.h"

enum SmsReceiveState {
  SMS_IDLE,
  SMS_WAIT_DIRECT_PDU,
  SMS_WAIT_CNMA_RESULT,
  SMS_WAIT_CMGR_HEADER,
  SMS_WAIT_STORED_PDU,
  SMS_WAIT_CMGR_RESULT
};

static SmsReceiveState smsReceiveState = SMS_IDLE;
static void setSmsReceiveState(SmsReceiveState state);
static int storedSmsIndex = -1;
static unsigned long lastStoredSmsPoll = 0;
static unsigned long smsStateSince = 0;
static unsigned long lastSmsTransactionAt = 0;

// 固定大小的删除等待列表：不使用 Flash 队列，只把删除操作推迟到当前 URC 交易结束后。
#define MAX_PENDING_SMS_DELETES 20
static int16_t pendingDeleteIndices[MAX_PENDING_SMS_DELETES];
static uint8_t pendingDeleteCount = 0;

// 仅保留最近成功投递的短信内容指纹，防止网络重建 PDU 后反复下发同一短信。
#define RECENT_DELIVERED_SMS_COUNT 6
#define SMS_DEDUP_WINDOW_MS (30UL * 60UL * 1000UL)
#define SMS_DEDUP_WINDOW_SECONDS (SMS_DEDUP_WINDOW_MS / 1000UL)
struct RecentDeliveredSms {
  uint32_t fingerprint;
  unsigned long deliveredAt;
};
static RecentDeliveredSms recentDeliveredSms[RECENT_DELIVERED_SMS_COUNT] = {};
static uint8_t recentDeliveredSmsNext = 0;
static uint32_t persistedDeliveredFingerprints[RECENT_DELIVERED_SMS_COUNT] = {};
static uint32_t persistedDeliveredAt[RECENT_DELIVERED_SMS_COUNT] = {};
static uint8_t persistedDeliveredNext = 0;
static uint32_t smsEventSequence = 0;
static unsigned long nextSmsDeleteAttemptAt = 0;

enum RecentDeliveryMatch {
  DELIVERY_NEW,
  DELIVERY_DUPLICATE_RAM,
  DELIVERY_DUPLICATE_NVS
};

static uint32_t fingerprintBytes(const char* value, uint32_t hash = 2166136261UL) {
  while (*value) {
    hash ^= (uint8_t)*value++;
    hash *= 16777619UL;
  }
  return hash;
}

static uint32_t pduFingerprint(const String& pduLine) {
  uint32_t hash = 2166136261UL;
  for (unsigned int i = 0; i < pduLine.length(); i++) {
    hash ^= (uint8_t)pduLine.charAt(i);
    hash *= 16777619UL;
  }
  return hash;
}

static uint32_t messageFingerprint(const char* sender, const char* text) {
  uint32_t hash = fingerprintBytes(sender);
  hash ^= '\n';
  hash *= 16777619UL;
  return fingerprintBytes(text, hash);
}

static void beginStoredSmsRead() {
  Serial1.println(String("AT+CMGR=") + String(storedSmsIndex));
  setSmsReceiveState(SMS_WAIT_CMGR_HEADER);
}

static uint32_t currentEpochSeconds() {
  time_t now = time(nullptr);
  return now > 1000000000 ? (uint32_t)now : 0;
}

void initSmsDeliveryGuard() {
  Preferences deliveryPreferences;
  if (!deliveryPreferences.begin("sms_dedup", true)) return;
  persistedDeliveredNext = deliveryPreferences.getUChar("next", 0) % RECENT_DELIVERED_SMS_COUNT;
  for (uint8_t i = 0; i < RECENT_DELIVERED_SMS_COUNT; i++) {
    char hashKey[4];
    char timeKey[4];
    snprintf(hashKey, sizeof(hashKey), "h%u", i);
    snprintf(timeKey, sizeof(timeKey), "t%u", i);
    persistedDeliveredFingerprints[i] = deliveryPreferences.getULong(hashKey, 0);
    persistedDeliveredAt[i] = deliveryPreferences.getULong(timeKey, 0);
  }
  deliveryPreferences.end();
}

static RecentDeliveryMatch recentDeliveryMatch(uint32_t fingerprint) {
  unsigned long now = millis();
  for (uint8_t i = 0; i < RECENT_DELIVERED_SMS_COUNT; i++) {
    if (recentDeliveredSms[i].fingerprint == fingerprint &&
        now - recentDeliveredSms[i].deliveredAt < SMS_DEDUP_WINDOW_MS) {
      return DELIVERY_DUPLICATE_RAM;
    }
  }
  uint32_t epoch = currentEpochSeconds();
  if (epoch == 0) return DELIVERY_NEW;
  for (uint8_t i = 0; i < RECENT_DELIVERED_SMS_COUNT; i++) {
    if (persistedDeliveredFingerprints[i] == fingerprint &&
        persistedDeliveredAt[i] <= epoch &&
        epoch - persistedDeliveredAt[i] < SMS_DEDUP_WINDOW_SECONDS) {
      return DELIVERY_DUPLICATE_NVS;
    }
  }
  return DELIVERY_NEW;
}

static const char* recentDeliveryMatchSource(RecentDeliveryMatch match) {
  return match == DELIVERY_DUPLICATE_NVS ? "NVS" : "RAM";
}

static void rememberDelivered(uint32_t fingerprint) {
  recentDeliveredSms[recentDeliveredSmsNext].fingerprint = fingerprint;
  recentDeliveredSms[recentDeliveredSmsNext].deliveredAt = millis();
  recentDeliveredSmsNext = (recentDeliveredSmsNext + 1) % RECENT_DELIVERED_SMS_COUNT;

  uint32_t epoch = currentEpochSeconds();
  if (epoch == 0) return;
  uint8_t slot = persistedDeliveredNext;
  persistedDeliveredFingerprints[slot] = fingerprint;
  persistedDeliveredAt[slot] = epoch;
  persistedDeliveredNext = (persistedDeliveredNext + 1) % RECENT_DELIVERED_SMS_COUNT;

  Preferences deliveryPreferences;
  if (!deliveryPreferences.begin("sms_dedup", false)) return;
  char hashKey[4];
  char timeKey[4];
  snprintf(hashKey, sizeof(hashKey), "h%u", slot);
  snprintf(timeKey, sizeof(timeKey), "t%u", slot);
  deliveryPreferences.putULong(hashKey, fingerprint);
  deliveryPreferences.putULong(timeKey, epoch);
  deliveryPreferences.putUChar("next", persistedDeliveredNext);
  deliveryPreferences.end();
}

static void setSmsReceiveState(SmsReceiveState state) {
  smsReceiveState = state;
  smsStateSince = state == SMS_IDLE ? 0 : millis();
}

static bool scheduleSmsDelete(int index) {
  if (index < 0) return true;
  for (uint8_t i = 0; i < pendingDeleteCount; i++) {
    if (pendingDeleteIndices[i] == index) return true;
  }
  if (pendingDeleteCount >= MAX_PENDING_SMS_DELETES) {
    logCaptureLn(String("删除等待队列已满，保留短信以便重试"));
    return false;
  }
  pendingDeleteIndices[pendingDeleteCount++] = index;
  return true;
}

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
      concatBuffer[i].storedIndices[j] = -1;
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      if (concatBuffer[i].totalParts != totalParts) {
        logCaptureLn(String("长短信参考号重复但分段数不一致，重建缓存"));
        clearConcatSlot(i);
        break;
      }
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
        concatBuffer[i].storedIndices[j] = -1;
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  logCaptureLn(String("⚠️ 长短信缓存已满，覆盖最老的槽位"));
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
    concatBuffer[oldestSlot].storedIndices[j] = -1;
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  int totalParts = min(concatBuffer[slot].totalParts, MAX_CONCAT_PARTS);
  for (int i = 0; i < totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
    concatBuffer[slot].storedIndices[j] = -1;
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        logCaptureLn(String("⏰ 长短信超时，保留已收分段等待重试"));
        logCaptureF("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 不转发不完整消息，防止延迟分段到达后产生重复或丢失。
        concatBuffer[i].firstPartTime = now;
      }
    }
  }
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 检查发送者是否在号码黑名单中
bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;

  String originalSender = String(sender);
  bool has86 = originalSender.startsWith("+86");
  String strippedSender = has86 ? originalSender.substring(3) : "";

  int listLen = (int)config.numberBlackList.length();

  int start = 0;
  while (start <= listLen) {
    int end = config.numberBlackList.indexOf('\n', start);
    if (end == -1) end = listLen;

    String line = config.numberBlackList.substring(start, end);
    line.trim();

    if (line.length() > 0 && (line.equals(originalSender) || (has86 && line.equals(strippedSender)))) {
      return true;
    }

    start = end + 1;
  }

  return false;
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  logCaptureLn(String("处理管理员命令: " + cmd));
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      
      targetPhone.trim();
      smsContent.trim();
      
      logCaptureLn(String("目标号码: " + targetPhone));
      logCaptureLn(String("短信内容: " + smsContent));
      
      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());
      
      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");
      
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      logCaptureLn(String("SMS命令格式错误"));
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    logCaptureLn(String("执行RESET命令"));
    
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    logCaptureLn(String("正在重启ESP32..."));
    delay(1000);
    ESP.restart();
  }
  else {
    logCaptureLn(String("未知命令: " + cmd));
  }
}

// 处理最终的短信内容（管理员命令检查和转发）
bool processSmsContent(const char* sender, const char* text, const char* timestamp) {
  logCaptureLn(String("处理短信，内容长度: ") + String(strlen(text)));

  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    logCaptureLn(String("发送者在号码黑名单中，忽略该短信"));
    return true;
  }

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    logCaptureLn(String("收到管理员短信，检查命令..."));
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return true;
    }
  }

  // 发送通知http（推送到所有启用的通道）
  bool pushSent = sendSMSToServer(sender, text, timestamp);
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  bool emailSent = sendEmailNotification(subject.c_str(), body.c_str());
  if (!pushSent && !emailSent) {
    logCaptureLn(String("所有通知通道失败，保留短信等待重试"));
    return false;
  }
  return true;
}

// 解码并处理一条 PDU。只有完成投递时才会将对应 SIM 索引加入删除等待列表。
bool processReceivedPdu(const String& line, int storedIndex, const char* source) {
  uint32_t eventId = ++smsEventSequence;
  uint32_t fingerprint = pduFingerprint(line);
  logCaptureF("短信事件#%lu: 来源=%s 索引=%d PDU=%u 指纹=%08lX\n",
              eventId, source, storedIndex, line.length(), fingerprint);

  if (!pdu.decodePDU(line.c_str())) {
    logCaptureLn(String("❌ PDU解析失败！"));
    return false;
  }

  logCaptureF("短信事件#%lu: PDU解析成功，模组时间=%s\n",
              eventId, pdu.getTimeStamp());
  uint32_t contentFingerprint = messageFingerprint(pdu.getSender(), pdu.getText());
  logCaptureF("短信事件#%lu: 内容指纹=%08lX\n", eventId, contentFingerprint);

  int* concatInfo = pdu.getConcatInfo();
  int refNumber = concatInfo[0];
  int partNumber = concatInfo[1];
  int totalParts = concatInfo[2];

  logCaptureF("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
  logCaptureLn(String("==============="));

  if (totalParts > MAX_CONCAT_PARTS || (totalParts > 1 && (partNumber < 1 || partNumber > totalParts))) {
    logCaptureLn(String("不支持的长短信分段数，保留在SIM中"));
    return false;
  }

  if (totalParts > 1) {
    logCaptureF("📧 收到长短信分段 %d/%d\n", partNumber, totalParts);
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;
    if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
      if (!concatBuffer[slot].parts[partIndex].valid) {
        concatBuffer[slot].parts[partIndex].valid = true;
        concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
        concatBuffer[slot].receivedParts++;
        if (concatBuffer[slot].receivedParts == 1) {
          concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
        }
        logCaptureF("  已缓存分段 %d，当前已收到 %d/%d\n",
                    partNumber, concatBuffer[slot].receivedParts, totalParts);
      } else {
        logCaptureF("  ⚠️ 分段 %d 已存在，跳过\n", partNumber);
      }
      if (storedIndex >= 0) concatBuffer[slot].storedIndices[partIndex] = storedIndex;
    }

    if (concatBuffer[slot].receivedParts >= totalParts) {
      logCaptureLn(String("✅ 长短信已收齐，开始合并转发"));
      String fullText = assembleConcatSms(slot);
      uint32_t fullFingerprint = messageFingerprint(concatBuffer[slot].sender.c_str(), fullText.c_str());
      RecentDeliveryMatch match = recentDeliveryMatch(fullFingerprint);
      if (match != DELIVERY_NEW) {
        logCaptureF("短信事件#%lu: 内容指纹=%08lX 命中%s去重，跳过重复推送\n",
                    eventId, fullFingerprint, recentDeliveryMatchSource(match));
        for (int i = 0; i < totalParts; i++) scheduleSmsDelete(concatBuffer[slot].storedIndices[i]);
        clearConcatSlot(slot);
        return true;
      }
      if (processSmsContent(concatBuffer[slot].sender.c_str(),
                            fullText.c_str(),
                            concatBuffer[slot].timestamp.c_str())) {
        rememberDelivered(fullFingerprint);
        for (int i = 0; i < totalParts; i++) scheduleSmsDelete(concatBuffer[slot].storedIndices[i]);
        clearConcatSlot(slot);
        return true;
      }
      return false;
    }
    return false;
  } else {
    RecentDeliveryMatch match = recentDeliveryMatch(contentFingerprint);
    if (match != DELIVERY_NEW) {
      logCaptureF("短信事件#%lu: 内容指纹=%08lX 命中%s去重，跳过重复推送\n",
                  eventId, contentFingerprint, recentDeliveryMatchSource(match));
      scheduleSmsDelete(storedIndex);
      return true;
    }
    bool delivered = processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
    if (delivered) {
      rememberDelivered(contentFingerprint);
      scheduleSmsDelete(storedIndex);
    }
    return delivered;
  }
}

// 处理短信 URC。兼容直接上报 +CMT 和存储索引上报 +CMTI 两种路径。
void checkSerial1URC() {
  const unsigned long SMS_STATE_TIMEOUT_MS = 10000;
  if (smsReceiveState != SMS_IDLE && millis() - smsStateSince >= SMS_STATE_TIMEOUT_MS) {
    logCaptureLn(String("短信读取超时，重置状态等待下次轮询"));
    storedSmsIndex = -1;
    setSmsReceiveState(SMS_IDLE);
  }

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  if (smsReceiveState == SMS_IDLE) {
    if (line.startsWith("+CMT:")) {
      logCaptureLn(String("检测到+CMT，等待PDU数据..."));
      setSmsReceiveState(SMS_WAIT_DIRECT_PDU);
    } else if (line.startsWith("+CMTI:")) {
      int commaPos = line.lastIndexOf(',');
      if (commaPos >= 0) {
        storedSmsIndex = line.substring(commaPos + 1).toInt();
        if (storedSmsIndex >= 0) {
          logCaptureLn(String("检测到+CMTI，先发送CNMA确认，索引: ") + String(storedSmsIndex));
          Serial1.println("AT+CNMA");
          setSmsReceiveState(SMS_WAIT_CNMA_RESULT);
        }
      }
    }
  } else if (smsReceiveState == SMS_WAIT_CNMA_RESULT) {
    if (line == "OK") {
      logCaptureLn(String("CNMA确认成功"));
      beginStoredSmsRead();
    } else if (line.indexOf("ERROR") >= 0) {
      // 某些网络在存储通知模式下不要求 CNMA；仍读取并处理短信，但保留结果供诊断。
      logCaptureLn(String("CNMA未被接受或不需要，继续读取SIM短信"));
      beginStoredSmsRead();
    }
  } else if (smsReceiveState == SMS_WAIT_CMGR_HEADER) {
    if (line.startsWith("+CMGR:")) {
      setSmsReceiveState(SMS_WAIT_STORED_PDU);
    } else if (line.indexOf("ERROR") >= 0) {
      logCaptureLn(String("❌ 读取SIM短信失败，索引: ") + String(storedSmsIndex));
      storedSmsIndex = -1;
      setSmsReceiveState(SMS_IDLE);
    }
  } else if (smsReceiveState == SMS_WAIT_DIRECT_PDU || smsReceiveState == SMS_WAIT_STORED_PDU) {
    if (isHexString(line)) {
      int currentStoredIndex = smsReceiveState == SMS_WAIT_STORED_PDU ? storedSmsIndex : -1;
      processReceivedPdu(line, currentStoredIndex,
                         currentStoredIndex >= 0 ? "CMTI/CMGR" : "CMT");
      storedSmsIndex = -1;
      lastSmsTransactionAt = millis();
      // CMGR 的结尾 OK 必须被明确消费，不能用固定延时猜测它已经到达。
      setSmsReceiveState(currentStoredIndex >= 0 ? SMS_WAIT_CMGR_RESULT : SMS_IDLE);
    } else if (line.indexOf("ERROR") >= 0) {
      logCaptureLn(String("收到非PDU数据，返回IDLE状态"));
      storedSmsIndex = -1;
      setSmsReceiveState(SMS_IDLE);
    }
  } else if (smsReceiveState == SMS_WAIT_CMGR_RESULT) {
    if (line == "OK") {
      setSmsReceiveState(SMS_IDLE);
    } else if (line.indexOf("ERROR") >= 0) {
      logCaptureLn(String("CMGR读取结束异常，保留短信等待重试"));
      setSmsReceiveState(SMS_IDLE);
    }
  }
}

// 主动扫描未读短信，弥补 URC 在初始化或网页 AT 操作期间被消费的极端情况。
void pollStoredSms() {
  const unsigned long POLL_INTERVAL_MS = 30000;
  if (!modemReady || smsReceiveState != SMS_IDLE) return;
  // 已投递的短信在等待删除确认时，不能再被 CMGL 扫描，否则同一条已读短信会被重复转发。
  if (pendingDeleteCount > 0) return;
  if (millis() - lastSmsTransactionAt < 100) return;
  if (lastStoredSmsPoll != 0 && millis() - lastStoredSmsPoll < POLL_INTERVAL_MS) return;
  lastStoredSmsPoll = millis();

  // CMGR 通常会将短信标为已读；为重试未成功投递的短信，扫描所有收件箱状态。
  String response = sendATCommand("AT+CMGL=4", 5000);
  if (response.indexOf("+CMGL:") < 0) return;

  int currentIndex = -1;
  int currentStatus = -1;
  int lineStart = 0;
  while (lineStart < response.length()) {
    int lineEnd = response.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = response.length();
    String line = response.substring(lineStart, lineEnd);
    line.trim();

    if (line.startsWith("+CMGL:")) {
      int colon = line.indexOf(':');
      int comma = line.indexOf(',', colon + 1);
      currentIndex = (comma > colon) ? line.substring(colon + 1, comma).toInt() : -1;
      int nextComma = comma >= 0 ? line.indexOf(',', comma + 1) : -1;
      currentStatus = nextComma > comma ? line.substring(comma + 1, nextComma).toInt() : -1;
    } else if (currentIndex >= 0 && isHexString(line)) {
      // 0=REC UNREAD, 1=REC READ；不解码已发送或草稿短信。
      if (currentStatus != 0 && currentStatus != 1) {
        currentIndex = -1;
        currentStatus = -1;
        lineStart = lineEnd + 1;
        continue;
      }
      // 给一条长短信留出最多 10 个删除位置，同时不让不可投递短信阻塞后续扫描。
      if (pendingDeleteCount <= MAX_PENDING_SMS_DELETES - MAX_CONCAT_PARTS) {
        processReceivedPdu(line, currentIndex, "CMGL轮询");
      } else {
        logCaptureLn(String("未读短信过多，剩余部分留待下次轮询"));
      }
      currentIndex = -1;
      currentStatus = -1;
    }
    lineStart = lineEnd + 1;
  }
}

void processPendingSmsDeletes() {
  // 仅在 CMGR 的结尾 OK 已明确消费后发送 CMGD，避免把前一条命令的 OK 误判为删除成功。
  if (smsReceiveState != SMS_IDLE || pendingDeleteCount == 0 ||
      millis() - lastSmsTransactionAt < 100 ||
      (nextSmsDeleteAttemptAt != 0 && (long)(millis() - nextSmsDeleteAttemptAt) < 0)) return;

  int16_t index = pendingDeleteIndices[0];
  for (uint8_t i = 1; i < pendingDeleteCount; i++) {
    pendingDeleteIndices[i - 1] = pendingDeleteIndices[i];
  }
  pendingDeleteCount--;

  String deleteCmd = "AT+CMGD=" + String(index);
  if (sendATandWaitOK(deleteCmd.c_str(), 2000)) {
    logCaptureLn(String("已确认删除SIM短信索引: ") + String(index));
    nextSmsDeleteAttemptAt = 0;
  } else {
    logCaptureLn(String("删除SIM短信失败，保留索引等待重试: ") + String(index));
    scheduleSmsDelete(index);
    nextSmsDeleteAttemptAt = millis() + 5000;
  }
}
