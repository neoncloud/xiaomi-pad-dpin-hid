# 模块怎样转发输入

```mermaid
flowchart LR
    A[平板的触控板、触屏和笔] --> B[原厂输入中继]
    B --> C[ShadowHook 在指定位置补充或替换报告]
    C --> D[原厂 USB HID 接口]
    D --> E[Windows 触摸板、触屏、笔和滑动快捷键]
```

- **平板侧。** 模块启动器通过 KernelSU 文件挂载运行指定的原厂中继，并加载 ShadowHook 和本项目的原生库。构建时检查原厂 ELF 哈希，运行时核对调用点字节。固件升级后需要重新检查实现。
- **触控板。** Windows 选择 PTP 模式、现有 USB 会话就绪后，模块在全抬手边界接管。原厂解析路径继续跟踪触点状态，来自同一触控板调用域的旧 HID 输出受到限制。Windows 获得最多五个报告槽位；可用手指数由设备输入决定。
- **触屏与笔。** 触屏保留 62 字节布局，补充可信度。笔报告用 12 字节承载近场、压感、X/Y 倾角、笔杆侧旋与两个侧键。滑动使用独立的 HID 键盘报告发送 F15／F16。平板端脚本按 DP-in 会话取得并归还笔杆姿态模式。
- **生命周期。** 模块通过原厂当前代际的 HID 文件描述符发送，按原厂创建和关闭边界重置。主机仍在线时尽力发送释放。后续自然建链生成本代描述符。

## 源码入口

| 路径 | 用途 |
| --- | --- |
| [`src/ptp_native.c`](../src/ptp_native.c) | 原生转发、会话处理、平板输入编码 |
| [`src/digitizer_reports.h`](../src/digitizer_reports.h) | 触屏和笔的数据布局 |
| [`src/native_ptp_launcher.c`](../src/native_ptp_launcher.c) | 启动原厂中继和模块库 |
| [`module/`](../module/) | KernelSU 启动、安装、停止和侧旋控制 |
| [`tools/`](../tools/) | 从用户提供的原厂 ELF 生成配置、编译与打包 |

3.1.6 在 Windows 11 上验证了 PTP mode 3、同屏触笔和触屏枚举、笔侧旋的正数范围及两种侧键。原始输入还验证了两个滑动方向的按下与释放。按键与笔刷行为取决于绘画应用的绑定和支持。

[Windows 精确式触摸板](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-collection) · [Windows 笔 HID 字段](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/required-hid-top-level-collections) · [ShadowHook](https://github.com/bytedance/android-inline-hook)
