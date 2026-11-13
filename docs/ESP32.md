# ESP32 蓝牙

ESP32 通过开发板原有 USB 转串口连接 Mac，运行独立蓝牙控制器固件。模拟器中的原固件负责蓝牙主机协议，手机可在空中发现和连接模拟设备。当前固件支持经典 ESP32、4 MB Flash，已验证 ESP32-D0WDQ6；不适用于 ESP32-S2、S3 或 C3。

当前仅支持具有独立持久 NOR 的 Q66 实例。在新实例的设备连接页面选择 ESP32 和对应 USB 串口。已有虚拟蓝牙实例需要另建实例，保留原数据。实例首次启动会用控制器真实蓝牙地址初始化独立 NOR，以后仍需同一控制器。ESP32 模式不创建虚拟对端绑定状态。

串口使用 115200 波特，理论传输上限约 11 KB/s；大批量 ACL 数据可能受到吞吐限制。

刷入前，应用通过官方 esptool RAM loader 读取完整 Flash 并与设备端摘要核对。备份及恢复所需收据保存在固件库的 `esp32-backups` 目录。恢复时选择该目录中的 `flash-backup.bin`，同时保留同目录的 `receipt.json`。安装只写入控制器镜像对应的 Flash 区段；恢复会写回完整 4 MB 备份，不写 eFuse。写入异常不会自动重试或恢复。

运行时串口由独立进程独占；模拟器只连接私有 H4 socket。关闭实例会先停止原固件，再发送真实 HCI Reset 和控制器 STOP，收到 CLOSED 后释放串口。控制器异常、超时或无法确认关闭时会保留日志并停止本次运行。

开发接口使用应用内置 Python 和 `Runtime/azki-simulator.py`：

```text
esp32-devices --engine-root ENGINE
esp32-bridge --engine-root ENGINE --port /dev/cu.PORT --state-root PRIVATE_DIRECTORY
esp32-flash --engine-root ENGINE --port /dev/cu.PORT --backup-root DIRECTORY --operation backup|install
esp32-flash --engine-root ENGINE --port /dev/cu.PORT --backup-root DIRECTORY --operation restore --backup DIRECTORY/flash-backup.bin
```

桥接进程先读取真实 HCI 地址并发布 `ready.json`，准备实例时传入 `--bluetooth-mode esp32 --esp32-ready-file PATH`。结束进程使用 SIGTERM；`closed.json` 记录控制器关闭、串口释放和首个错误。固件源码、构建说明及实际镜像在 `engine/project/backend/esp32-controller`。

## 验证范围

macOS 已通过应用实际隔离策略运行 Q66TC 4.100.155 原固件：独立 Intel 接收端确认真实广播，并完成首次连接、双向 ATT 设备名读取及同一连接 30.091 秒无 ATT 数据后的再次读取。测试使用空白存储和完整首次设置流程，正常关闭后 NAND、OOB 与 NOR 已共同提交。

应用业务写入、通知、SMP 配对及其他控制器型号尚未验证。存储提交为崩溃一致性，尚未验证固件文件系统干净卸载。
