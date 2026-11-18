# 安装、使用和恢复

## 安装与使用

1. 准备 [本地构建](BUILD.md) 的模块 ZIP，并退出当前 DP-in 会话。
2. 打开平板上的 KernelSU 管理器，在「模块」页面选择安装该 ZIP，确认模块处于启用状态。KernelSU 需要能将模块里的系统文件挂载到指定位置。采用元模块架构的版本请参照当前 KernelSU 配置。[KernelSU 模块指南](https://kernelsu.org/zh_CN/guide/module.html)
3. 完成管理器提示的重启。若系统重启后临时 root 失效，先恢复 root，再确认模块启用。
4. 连接同时支持视频和数据的 USB-C 线路，在平板上正常进入 DP-in。等 Windows 完成设备识别，然后试用触控板、触屏和笔。

Windows 的「设置 → 蓝牙和设备 → 触摸板」可用于调整精确式触摸板手势。笔身滑动可在支持绑定 `F15`／`F16` 的绘画软件里设置。

## 排查与恢复

- **有画面，Windows 仍看不到输入设备。** 先确认平板和电脑之间有 USB 数据连接。退出一次 DP-in，再正常进入，等待 Windows 枚举。
- **更新后仍像旧版。** KernelSU 热重启后可能仍使用旧的文件挂载。退出 DP-in 后按设备支持的方式完成模块激活。进行整机重启时，记得随后恢复临时 root。
- **想恢复出厂输入行为。** 先正常退出 DP-in。在 KernelSU 禁用或卸载本模块，再重启使修改生效。之后按平板原有方式进入 DP-in。
- **想微调触屏。** 默认门限为 21。掌侧仍误触时可保留 21；重按容易被忽略时可试 22。该值根据触点首帧大小判断，用于给 Windows 提供可信度提示。

平时无需这些命令。需要查看状态、临时停止或调整时，先在平板上取得 root shell，然后运行对应行。

```sh
/system/bin/sh /data/adb/modules/codex_usb_ptp_touchpad_native/ptpctl.sh status
/system/bin/sh /data/adb/modules/codex_usb_ptp_touchpad_native/ptpctl.sh stop
/system/bin/sh /data/adb/modules/codex_usb_ptp_touchpad_native/ptpctl.sh start
/system/bin/sh /data/adb/modules/codex_usb_ptp_touchpad_native/ptpctl.sh palm-test 22
/system/bin/sh /data/adb/modules/codex_usb_ptp_touchpad_native/ptpctl.sh palm-test-off
```

这里的 `stop` 停用 PTP 转发和新增笔身控制，并释放相应状态。触控显示器的 HID 布局随本次连接存在，完整回到原厂布局请用 KernelSU 禁用或卸载。 `palm-test-off` 关闭首帧大小门限。
