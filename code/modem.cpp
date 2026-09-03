#include "modem.h"
#include "web_handlers.h"

static bool waitRegistration(const char* command, const char* prefix);

static int terminalResult(const String& response) {
  int start = 0;
  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) return 0;
    String line = response.substring(start, end);
    line.trim();
    if (line == "OK") return 1;
    if (line == "ERROR" || line.startsWith("+CME ERROR:") || line.startsWith("+CMS ERROR:")) return -1;
    start = end + 1;
  }
  return 0;
}

bool isPdpContextActive(const String& response, int contextId) {
  int start = 0;
  while (start < response.length()) {
    int end = response.indexOf('\n', start);
    if (end < 0) end = response.length();
    String line = response.substring(start, end);
    line.trim();
    int colon = line.indexOf(':');
    int comma = line.indexOf(',', colon + 1);
    if (line.startsWith("+CGACT:") && comma > colon) {
      int cid = line.substring(colon + 1, comma).toInt();
      int state = line.substring(comma + 1).toInt();
      if (cid == contextId) return state == 1;
    }
    start = end + 1;
  }
  return false;
}

static bool disableDataConnection() {
  if (sendATandWaitOK("AT+CGACT=0,1", 5000)) {
    logCaptureLn(String("已禁用数据连接(CGACT=0,1)"));
    return true;
  }

  String state = sendATCommand("AT+CGACT?", 2000);
  bool inactive = state.indexOf("+CGACT:") >= 0 && !isPdpContextActive(state, 1);
  logCaptureLn(inactive ? String("数据连接已处于禁用状态")
                         : String("无法确认数据连接已禁用"));
  return inactive;
}

static bool sendATWithRetry(const char* command) {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (sendATandWaitOK(command, 3000)) return true;
    if (attempt < 2) delay(2000);
  }
  return false;
}

static bool configureOperator() {
  operatorApplyStatus = OPERATOR_STATUS_APPLYING;
  bool manual = config.operatorMode == 1;
  if (!manual) {
    bool success = sendATandWaitOK("AT+COPS=0", 300000);
    operatorApplyStatus = success ? OPERATOR_STATUS_APPLIED : OPERATOR_STATUS_FAILED;
    logCaptureLn(success ? String("自动选网成功") : String("自动选网失败"));
    return success;
  }

  bool valid = config.operatorCode.length() == 5 || config.operatorCode.length() == 6;
  for (unsigned int i = 0; valid && i < config.operatorCode.length(); i++) {
    valid = isDigit(config.operatorCode.charAt(i));
  }
  if (!valid) {
    operatorApplyStatus = OPERATOR_STATUS_FAILED;
    logCaptureLn(String("手动选网 PLMN 无效"));
    return false;
  }

  char command[32];
  snprintf(command, sizeof(command), "AT+COPS=1,2,\"%s\",%u",
           config.operatorCode.c_str(), config.operatorAct);
  if (!sendATandWaitOK(command, 300000)) {
    operatorApplyStatus = OPERATOR_STATUS_FAILED;
    logCaptureLn(String("手动选网超时或失败"));
    return false;
  }
  String state = sendATCommand("AT+COPS?", 3000);
  bool success = state.indexOf("+COPS: 1,") >= 0 && state.indexOf(config.operatorCode) >= 0;
  operatorApplyStatus = success ? OPERATOR_STATUS_APPLIED : OPERATOR_STATUS_FAILED;
  logCaptureLn(success ? String("手动选网成功") : String("手动选网状态校验失败"));
  return success;
}

static void configureSmsService() {
  const char* commands[] = {"AT+CEMODE=3", "AT*PSDC=0", "AT+CSMS=1", "AT+CIREG=1"};
  bool success = true;
  for (const char* command : commands) {
    if (!sendATWithRetry(command)) success = false;
  }
  if (!success) logCaptureLn(String("短信初始化失败"));
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (terminalResult(resp) != 0) return resp;
    }
    delay(1);
  }
  return resp;
}

// 新增"模组断电重启"函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn(String("EN 拉低：关闭模组"));
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  logCaptureLn(String("EN 拉高：开启模组"));
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  logCaptureLn(String("正在硬重启模组（EN 断电重启）..."));
  modemInitializing = true;
  modemReady = false;
  modemPowerCycle();
  modemInit();
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
void modemInit() {
  modemInitializing = true;
  modemReady = false;

  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();

  while (!sendATandWaitOK("AT", 1000)) {
    logCaptureLn(String("AT未响应，重试..."));
    blink_short();
  }
  logCaptureLn(String("模组AT响应正常"));

  //判断型号，做一些特定操作
  bool need_set_CGACT = true;
  String resp = sendATCommand("ATI", 2000);
  logCaptureLn(String("ATI响应: " + resp));
  if (resp.indexOf("OK") >= 0) {
    // 解析ATI响应
    String manufacturer = "未知";
    String model = "未知";
    String version = "未知";
    
    // 按行解析
    int lineStart = 0;
    int lineNum = 0;
    for (int i = 0; i < resp.length(); i++) {
      if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
        String line = resp.substring(lineStart, i);
        line.trim();
        if (line.length() > 0 && line != "ATI" && line != "OK") {
          lineNum++;
          if (lineNum == 1) manufacturer = line;
          else if (lineNum == 2) model = line;
          else if (lineNum == 3) version = line;
        }
        lineStart = i + 1;
      }
    }
    //这个模组这条命令有bug
    if(model == "ML307Y") need_set_CGACT = false;
  }

  bool requireIms = config.operatorMode == 1 && config.operatorAct == 7;
  if (requireIms) configureSmsService();

  // 先选网，再关闭数据连接，避免重新驻网时再次激活普通数据上下文。
  bool operatorReady = configureOperator();

  bool dataDisabled = true;
  if(need_set_CGACT) {
    dataDisabled = disableDataConnection();
  } else {
    logCaptureLn(String("该型号无法配置(AT+CGACT=0,1)，跳过该命令，会不会消耗流量？自求多福"));
  }
  sendATandWaitOK("AT+CPMS=\"SM\",\"SM\",\"SM\"", 2000);
  while (!sendATandWaitOK("AT+CNMI=2,1,0,0,0", 1000)) {
    logCaptureLn(String("设置CNMI失败，重试..."));
    blink_short();
  }
  logCaptureLn(String("CNMI参数设置完成"));
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    logCaptureLn(String("设置PDU模式失败，重试..."));
    blink_short();
  }
  logCaptureLn(String("PDU模式设置完成"));
  int ceregRetry = 0;
  while (!waitCEREG() && ceregRetry < 30) {
    logCaptureLn(String("等待网络注册..."));
    ceregRetry++;
    blink_short();
  }
  bool networkReady = ceregRetry < 30;
  if (networkReady) {
    logCaptureLn(String("网络已注册"));
  } else {
    logCaptureLn(String("⚠️ 网络注册超时（无SIM卡或信号差），模组功能不可用"));
  }

  int imsRetry = 0;
  while (requireIms && networkReady && imsRetry < 20 && !waitCIREG()) {
    imsRetry++;
    blink_short();
  }
  bool imsReady = !requireIms || (networkReady && imsRetry < 20);
  if (requireIms) logCaptureLn(imsReady ? String("IMS已注册") : String("IMS注册超时"));
  // 自动选网或注册过程可能重新激活 PDP，注册完成后再次关闭以保证默认不用流量。
  if (need_set_CGACT) dataDisabled = disableDataConnection() && dataDisabled;
  modemReady = operatorReady && networkReady && imsReady && dataDisabled;
  modemInitializing = false;
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      int result = terminalResult(resp);
      if (result != 0) return result > 0;
    }
    delay(1);
  }
  return false;
}

// 检测网络注册状态（LTE/4G）
// CEREG状态: 1=已注册本地, 5=已注册漫游
static bool waitRegistration(const char* command, const char* prefix) {
  Serial1.println(command);
  unsigned long start = millis();
  String resp = "";
  bool registrationSeen = false;
  bool registered = false;
  while (millis() - start < 2000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      int prefixPos = resp.indexOf(prefix);
      int lineEnd = prefixPos >= 0 ? resp.indexOf('\n', prefixPos) : -1;
      if (prefixPos >= 0 && lineEnd >= 0) {
        int comma = resp.indexOf(',', prefixPos);
        if (comma < lineEnd) {
          int stat = resp.substring(comma + 1, lineEnd).toInt();
          registrationSeen = true;
          registered = stat == 1 || stat == 5;
        }
      }
      // 不能在看到 +CEREG/+CIREG 行后立即返回：必须消费本命令的终止 OK，
      // 否则下一条 AT 会把这个旧 OK 误认为自己的响应。
      int result = terminalResult(resp);
      if (result != 0) return result > 0 && registrationSeen && registered;
    }
    delay(1);
  }
  return false;
}

bool waitCEREG() {
  return waitRegistration("AT+CEREG?", "+CEREG:");
}

bool waitCIREG() {
  return waitRegistration("AT+CIREG?", "+CIREG:");
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  logCaptureLn(String("准备发送短信..."));
  logCaptureLn(String("短信长度: ") + String(strlen(message)));

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    logCapture(String("PDU编码失败，错误码: "));
    logCaptureLn(String(pduLen));
    return false;
  }
  
  logCapture(String("PDU长度: ")); logCaptureLn(String(pduLen));
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    delay(1);
  }
  
  if (!gotPrompt) {
    logCaptureLn(String("未收到>提示符"));
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      int result = terminalResult(resp);
      if (result > 0) {
        logCaptureLn(String("\n短信发送成功"));
        return true;
      }
      if (result < 0) {
        logCaptureLn(String("\n短信发送失败"));
        return false;
      }
    }
    delay(1);
  }
  logCaptureLn(String("短信发送超时"));
  return false;
}
