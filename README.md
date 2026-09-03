# 低成本短信转发器

> 本分支 `feature/reliable-sms-forwarding` 基于上游 `master`，补足“稳定接收、可诊断、低流量”的运行能力。2022 年的老方案请前往[上游 luatos 分支](https://github.com/chenxuuu/sms_forwarding/tree/old-luatos)。

本项目仅用于接收短信和保号相关功能；不以多卡控制、通话、拨号或开放自动化接口为目标。

[后台页面演示](https://sms.j2.cx/)

本项目旨在使用低成本的硬件设备，实现短信的自动转发功能，支持多种推送方式同时启用。

> 视频教程：[B站视频](https://www.bilibili.com/video/BV1cSmABYEiX)

<img src="assets/photo.png" width="200" />

## 本分支的改动

| 范围 | 内容 |
| --- | --- |
| 选网 | 新增“选网配置”：自动选择或按 PLMN 手动指定运营商及接入制式；适用于需要固定驻留网络的部署，页面展示已保存配置的应用状态。 |
| 收件 | 使用 SIM 存储通知模式（`CNMI=2,1`），将 `+CMTI` 短信读取、推送和删除串行化。 |
| 可靠性 | 仅在至少一个通知通道成功后删除 SIM 短信；失败消息留在 SIM 中并由轮询重试。 |
| 重复保护 | 对成功投递的“发件人 + 正文”保留 30 分钟指纹，重启后仍有效；不写入短信正文。 |
| 流量 | 普通 PDP 数据上下文默认关闭，选网和注册完成后会再次关闭；仅“网络测试”会临时启用。 |
| 构建与隐私 | 固定已验证的 Arduino/库版本；Wi-Fi 凭据改为本机忽略文件，CI 使用示例配置。 |

## 功能

- 支持使用通用AT指令与模块进行通信
- 开启后支持通过WEB界面配置短信转发参数、查询当前状态
- **最多同时启用 5 个推送通道**，每个通道可独立配置
- 支持将收到的短信转发到指定的邮箱
- 支持通过WEB界面主动发送短信，以便消耗余额
- 支持通过WEB界面进行Ping测试，以极低的成本消耗余额
- 支持最多 10 段的长短信自动合并，延迟分段会保留在 SIM 中等待重试
- 支持管理员短信远程发送短信和重启设备
- 支持在 WEB 界面自动选网或按 PLMN 手动指定运营商

## 手动运营商选择

自动选网适合大多数场景；当部署环境要求固定驻留网络，或自动选择的运营商、制式不符合预期时，可在 WEB 管理页的“选网配置”选择“手动指定”，填写 5 或 6 位数字 PLMN（MCC + MNC），并选择 LTE、3G 或 2G。保存后设备重启并应用；页面会显示当前保存配置的应用状态。

手动选网会校验模组实际驻留状态；LTE 模式会等待 IMS 短信注册。失败时保留手动配置，不静默切回自动选择，便于诊断和再次尝试。

## 可靠性与流量策略

- 存储通知的处理链路为：`+CMTI → CNMA（记录确认结果）→ CMGR → 推送 → CMGD`。每个 AT 事务的结束响应会被明确消费，避免串口残留响应串到下一条命令。
- 短信只会在至少一个已启用推送通道成功后从 SIM 删除。网络或推送失败时保留短信，后续轮询重试。极端断电时优先避免丢短信，因而仍可能出现一次重复投递。
- 成功投递后的“发件人 + 正文”指纹在运行内存和 ESP NVS 中保留 30 分钟（6 条轮换记录）。模组或网络重投的同内容消息会被删除但不再次推送；NVS 不保存号码或正文。
- 日志同时记录短信事件序号、原始 PDU 指纹、模组时间、内容指纹及 RAM/NVS 去重命中来源，以区分本机重读、网络重投或内容相同的独立短信。
- Wi-Fi 不可用不会阻止模组进入短信接收模式；网络恢复后会继续转发留在 SIM 中的消息。
- 蜂窝数据默认关闭，选网和注册后会再次确认关闭。只有管理员主动点击“网络测试”时才临时激活 PDP；漫游环境下该操作可能产生流量费用。

## 推送通道支持

支持以下方式；最多同时启用 5 个通道：

| 推送方式 | 说明 | 需要配置 |
|---------|------|---------|
| **POST JSON** | 通用HTTP POST | URL |
| **Bark** | iOS推送服务 | Bark服务器URL |
| **GET请求** | URL参数方式 | URL |
| **钉钉机器人** | 企业群通知 | Webhook URL，可选Secret加签 |
| **PushPlus** | 微信公众号推送 | Token |
| **Server酱** | 微信推送服务 | SendKey |
| **自定义模板** | 灵活的JSON模板 | URL + 请求体模板 |
| **飞书机器人** | 自定义通知 | Webhook URL |
| **Gotify** | 自建推送服务 | 服务器地址 + 应用 Token |
| **Telegram Bot** | Telegram 通知 | Chat ID + Bot Token |

### 推送格式说明

- **POST JSON**: `{"sender":"发送者号码","message":"短信内容","timestamp":"时间戳"}`
- **Bark**: `{"title":"发送者号码","body":"短信内容"}`
- **GET请求**: `URL?sender=xxx&message=xxx&timestamp=xxx`（自动URL编码）
- **钉钉机器人**: 文本消息格式，支持加签验证
- **PushPlus**: 使用Token推送，支持HTML格式
- **Server酱**: 使用SendKey推送，支持Markdown格式
- **自定义模板**: 使用`{sender}`、`{message}`、`{timestamp}`占位符
- **飞书机器人**: 文本消息格式，支持加签验证

|状态信息|主动ping|
|-|-|
|![](assets/status.png)|![](assets/ping.png)|

## 硬件搭配

若没有焊接能力，希望直接使用成品，可选直接购以下套件（我看过了，和自己做的成本一样）  
支持**移动/联通/电信卡**：

- [小蓝鲸WIFI短信宝](https://item.taobao.com/item.htm?id=1003711355912)（找客服问）
- [4G FPC天线](https://item.taobao.com/item.htm?id=1003711355912&skuId=6162872574943)，与开发板同购

如果希望自行焊接硬件，参考下面的硬件搭配，总成本约¥27.8（会有浮动，可按实际自行组合搭配）  
仅支持**移动/联通卡**：

- ESP32C3开发板，实测选用[ESP32C3 Super Mini](https://item.taobao.com/item.htm?id=852057780489&skuId=5813710390565)，¥9.5包邮
- ML307R-DC开发板，实测选用[小蓝鲸ML307R-DC核心板](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥16.3包邮
- [4G FPC天线](https://item.taobao.com/item.htm?id=797466121802&skuId=5722077108045)，¥2，与核心板同购


## 硬件连接

ESP32C3 与 ML307R-DC 通过串口（UART）连接，接线如下：

```
┌───────────────────────────────────────────────┐
|                                               |
|   ESP32C3 Super Mini      ML307R-DC核心板     |
| ┌───────────────────┐    ┌─────────────────┐ |
└─┼─ GPIO5 (MODEM_EN) │    │                 │ |
  │       GPIO3 (TX) ─┼───►│ RX              │ |
  │                   │    │             EN ─┼─┘
  │       GPIO4 (RX) ◄┼────┤ TX              │ 
  │                   │    │                 │ 
  │              GND ─┼────┤ GND             │ 
  │                   │    │                 │ 
  │               5V ─┼────┤ VCC (5V)        |
  │                   │    │                 │
  └───────────────────┘    └─────────────────┘
                           │                 │
                           │  SIM卡槽        │
                           │  (插入Nano SIM) │
                           │                 │
                           │  天线接口       │
                           │  (连接4G天线)   │
                           └─────────────────┘
```

可通过USB连接ESP32C3进行编程和供电，正常工作时，可通过网页与模组进行AT通信，方便调试。

## 软件组成

- ESP32C3运行自己的`Arduino`固件，负责连接WiFi和接收ML307R-DC发送过来的短信数据，然后转发到指定HTTP接口或邮箱
- ML307R-DC运行默认的AT固件，不用动

需要在`Arduino IDE`中单独安装这些库：

- **ReadyMail** by Mobizt
- **pdulib** by David Henry

需要在`Arduino IDE`中安装ESP32开发板支持，参考[官方文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)，版型选`MakerGO ESP32 C3 SuperMini`。

固件包含多个推送通道和诊断页面，建议将分区方案设为 **Huge APP (3MB No OTA/1MB SPIFFS)**。项目不使用 OTA 或 SPIFFS，该设置可避免默认 1.2MB 应用分区余量过小。

## 首次编译

Wi-Fi 凭据不纳入版本控制。先将示例复制为本机配置文件并填写，再编译或烧录：

```sh
cp code/wifi_config.example.h code/wifi_config_local.h
```

`wifi_config_local.h` 已被忽略；请勿把它或任何推送密钥提交到仓库。GitHub Actions 会使用示例配置进行编译检查。

命令行构建（与 CI 一致）：

```sh
arduino-cli core install esp32:esp32@3.3.10
arduino-cli lib install "pdulib@0.5.11" "ReadyMail@0.4.2"
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini:PartitionScheme=huge_app ./code
```

Arduino IDE 请安装 ESP32 开发板平台，并选择 `MakerGO ESP32 C3 SuperMini` 与 **Huge APP (3MB No OTA/1MB SPIFFS)** 分区方案。
