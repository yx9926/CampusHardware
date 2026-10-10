# ESP32-S3 区域签到与环境监测

基于 ESP-IDF 5.4.4；主控、RTC、按键和麦克风的要求来自 Agent.md。

## 接线

| 模块信号 | 默认 ESP32-S3 引脚 |
| --- | --- |
| DS3231 SDA / SCL | GPIO8 / GPIO9 |
| KY-004 签到 / 签退信号 | GPIO4 / GPIO5 |
| MAX9814 OUT | GPIO1（ADC1 channel 0） |

所有模块共地，信号电平为 3.3V。DS3231 SDA/SCL 使用约 4.7kΩ 外部上拉到 3.3V，配好备用电池。按键使用低电平有效接法。MAX9814 OUT 不能超过芯片 ADC 输入限制。GPIO 必须结合实际开发板确认，不可复用 USB、Flash、PSRAM 或板载外设占用的引脚。

## 配置、编译和烧录

本机使用已安装的 ESP-IDF 5.4.4，推荐在本目录执行版本启动脚本：

```powershell
$env:PYTHONUTF8 = '1'
.\idf544.ps1 menuconfig
.\idf544.ps1 build
.\idf544.ps1 -p COM3 flash monitor
```

脚本加载本机 5.4.4 环境，在 `.idf-env/esp-idf` 提供无空格的 SDK 目录联接，构建目录为 `build-v5.4.4`，避免复用其他 SDK 的 CMake 缓存。迁移沿用现有 sdkconfig，已保存的 Wi-Fi 和 MQTT 参数不会被 set-target 重置。另一台电脑需按自己的 5.4.4 安装位置调整脚本的激活路径及 VS Code 本地设置；容器镜像固定为 espressif/idf:v5.4.4。

在“Area check-in hardware configuration”中填写 Wi-Fi SSID/密码、实际 MQTT broker URI 和凭据；默认 SSID 为空，设备仍可离线采集，默认 broker 地址需按实际修改。首次使用的 RTC 必须先联网获得 NTP 校准，看到 `DS3231 synchronized (UTC)` 后才开始签到；已有有效时间和电池的 RTC 可直接离线启动。全部 Unix 时间为 UTC，前端显示时转换为本地时区。

生产环境配置 `mqtts://`，固件启用系统证书包校验；私有 CA 需接入对应证书，而非关闭证书验证。配置密码会存入本机 sdkconfig 与固件，请妥善保管。

## 模块与行为

- main.c：初始化、三个 FreeRTOS 任务、硬件中断 WDT 与任务 WDT。
- buttons.c：ISR 任务通知、10ms 复查、按下/释放各 50ms 稳定消抖；长按一次，只在稳定释放后重新计数。
- rtc.c：I²C 100ms 超时、总线重置和单次重试；互斥锁；BCD、日期、12/24小时制及 OSF 校验。
- sensor.c：连续 50ms 窗口采样、峰峰值估算、ADC 校准与削顶/异常判定。
- state.c：线程安全人数与声音状态、NVS 原子快照离线队列。
- network.c：Wi-Fi 指数退避重连、MQTT 自动重连、QoS1 按序补传、每小时 NTP 校准 RTC、每 5 秒在线遥测。

首次启动人数为 0，此后重启恢复持久化人数，签退下限为 0。签到事件在 flash 提交成功后才改变内存人数。所有业务时间戳读自 DS3231，RTC 无效时不会改用系统时间伪造时间戳。遥测只发送最新状态，不积压历史。

## MQTT 协议

设备 ID 默认从完整 MAC 派生；可配置为最多 64 个英文字母、数字、下划线或连字符。主题：

- `checkin/<device_id>/events`，QoS1
- `checkin/<device_id>/telemetry`，QoS0

事件示例：

```json
{"device_id":"AREA_A_001","event_id":"1","timestamp":1791532800,"event_type":"CHECK_IN","headcount_after_event":1}
```

遥测示例：

```json
{"device_id":"AREA_A_001","timestamp":1791532805,"headcount":1,"decibel":65.4,"sound_valid":true}
```

ADC 异常时 decibel 为 null，sound_valid 为 false。后端必须按 `(device_id, event_id)` 去重；QoS1 允许重复。事件只在 broker PUBACK 后从 NVS 删除，PUBACK 代表 broker 收到，不代表业务数据库已经写入。需要端到端落库保证时，应扩展后端应用确认协议。擦除 NVS 后序号归零，须同时更换设备标识或清理后端去重命名空间。

## 运行边界与验收

离线队列默认 128 条，已提交记录在重启后可恢复。满队列、RTC 失效或 flash 写失败时，串口明确输出 `Event REJECTED` 并保持人数，绝不静默覆盖旧记录。有限存储无法保证无限期断网不丢任何物理按键动作；现场必须监控拒绝日志，在超过容量前恢复网络。断电发生在提交之前的动作无法保证持久化。高流量部署应采用更大容量、低写放大的持久日志或外部 FRAM；当前每事件和确认各写一个快照，需评估 flash 寿命。

MAX9814 带自动增益，峰峰值只能给出近似声强；用参考声源测得 94dB 下的峰峰电压，配置 `CHECKIN_DB_REFERENCE_MV`。默认值仅供调试，不能据此声明绝对声压级或 A 计权值。

硬件验收清单：

1. 空人数签退仍为 0；长按和模拟机械抖动均只计一次；稳定释放再按增加一次。
2. 断开 Wi-Fi 和 broker，产生若干事件，重启并恢复网络，检查原时间戳、顺序、人数与去重。
3. 模拟 PUBACK 丢失，确认重复事件在后端只生效一次；缓存满时确认拒绝日志及人数不变。
4. 断开 RTC / 拉低 I²C 总线、移除电池导致 OSF，确认任务继续运行且不生成伪时间；恢复联网后校时。
5. ADC 断线/削顶与参考声源校准；持续运行观察 WDT、空闲堆内存和连接恢复。

ESP-IDF 5.4.4 构建产物位于 `build-v5.4.4/sample_project.bin`。本次升级的构建结果见下方验证记录；尚未烧录或进行板卡测试。

2026年10月9日验证：使用本机 ESP-IDF v5.4.4 / GCC 14.2.0，完成 ESP32-S3 全量编译、bootloader、分区表、应用链接与镜像生成。应用大小 0xf4bd0（1,002,448）字节，应用分区剩余约 47%。Wi-Fi 与 MQTT 本地配置已保留；设备运行及网络联调仍需烧录后确认。

编译成功仅能验证软件构建，实际板卡接线、声学校准、断电恢复和网络故障仍须按上述步骤测试。

接口依据：[Espressif I²C 文档](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/peripherals/i2c.html)、[Espressif MQTT 文档](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/protocols/mqtt.html)。实际 API 兼容性以本机 5.4.4 头文件及编译结果验证。
