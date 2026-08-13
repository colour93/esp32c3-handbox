# ESP32-C3 Handbox

基于 ESP32-C3 Super Mini、128x64 SPI OLED 和 EC11 编码器的 Drone CI 手持控制器。固件首页仅包含 CI、设置、状态和关于四个产品页面，版本为 `0.1.0`。

## 硬件

引脚完全沿用 `esp32c3-mini-12864-oled-kit`：

| 功能 | GPIO |
| --- | ---: |
| OLED SCL | 4 |
| OLED SDA | 6 |
| OLED CS | 7 |
| OLED DC | 10 |
| OLED RST | 5 |
| EC11 S1 | 0 |
| EC11 S2 | 1 |
| EC11 KEY | 3 |

目标环境使用 pioarduino `55.03.311`、`esp32-c3-devkitm-1`、`no_ota.csv` 和 USB CDC。`astra-ui-lite-arduino` 与 `esp-ble-config` 通过 `platformio.ini` 中固定 commit 的 Git URL 获取。

## 配置

首次启动会持续广播 `Handbox-XXXX`；打开 [esp32.9993.fun](https://esp32.9993.fun) 连接设备并填写配置。首次保存后广播自动关闭，本地“BLE 配置”开关可再次开放五分钟配置窗口。

所有持久配置均可通过 BLE 修改：

- Wi-Fi、设备名称、亮度、自动息屏时间和旋钮方向
- Drone 地址、Token、HTTPS 校验开关
- 1 至 10 个 `namespace / repo / branch` CI 目标

设备名称变更后固件会自动重启，使 OLED、GATT 名称和广播名保持一致。中文界面使用 U8g2 的 `u8g2_font_wqy12_t_gb2312` 字体。

## 操作

- 旋转：移动菜单或选择 CI 目标
- 短按：进入页面；CI 页面先预览最近 50 条构建中匹配分支的提交，再次短按才触发
- 长按：返回；CI 构建会留在后台每三秒轮询，最长监控 30 分钟
- 息屏后的第一次输入：仅唤醒，不执行导航；CI 监控期间暂停自动息屏

状态页显示 64 位开机时长、设备名称、BLE 状态、Wi-Fi/SSID/RSSI 和 IP 获取状态。关于页显示固件版本、构建时 Git short SHA 与仓库名。

## 构建

```bash
pio run
```

`scripts/build_meta.py` 在构建目录生成 Git short SHA；源码不在 Git 工作区时使用 `unknown`。HTTPS 默认使用 `certs/x509_crt_bundle` 中由 Mozilla/certifi CA 集合生成的 ESP x509 bundle；关闭校验时固件会显式使用 insecure TLS，明确的 HTTP 地址也受支持。
