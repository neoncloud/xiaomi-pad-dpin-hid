# 生成安装包

构建会从用户自己提供的原厂 USB 服务中读取本机的 HID 格式和调用点，并把该原厂服务放进本地生成的 ZIP。仓库提供模块的源文件和打包程序。构建完成后，用生成的 ZIP 安装。

## 准备

- Python 3.10 或更高版本。
- 对应构建电脑的 Android NDK。已用 Linux x86-64 的 **NDK r27c** 验证，Windows 可以使用 Linux x86-64 的 WSL。也可传入本机的 Windows NDK。
- 从受支持平板取得的原厂 `vendor.xiaomi.usb.dp-service`。
- 安装 Python 构建依赖和获取 ShadowHook 时可访问软件包源。已有官方 AAR 时可以通过 `--shadowhook-aar` 传入。

[Android NDK 下载](https://developer.android.com/ndk/downloads) · [ShadowHook 2.0.1](https://central.sonatype.com/artifact/com.bytedance.android/shadowhook/2.0.1)

## 取出原厂文件

在能识别平板的电脑上运行。将 `YOUR_SERIAL` 换成 `adb devices` 中自己平板的序列号。

```text
adb devices
mkdir stock
adb -s YOUR_SERIAL pull /odm/bin/vendor.xiaomi.usb.dp-service stock/vendor.xiaomi.usb.dp-service
```

如果平板已安装本模块并正在使用模块挂载，原厂副本位于 `/odm/bin/vendor.xiaomi.usb.dp-service.real`。拉取该副本时仍将本地文件命名为 `vendor.xiaomi.usb.dp-service`。

构建会校验文件 SHA-256，期望值为 `17b70644c4cd1ed8421616704eb72641aad5b7c00d94ecf029d324ce02d5a922`。校验失败时先检查固件版本以及取出的是否为原厂文件。

## 构建

在仓库根目录执行，替换 NDK 路径。以下以 Linux 或 WSL 为例。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-build.txt
python tools/build_module.py \
    --stock stock/vendor.xiaomi.usb.dp-service \
    --ndk /path/to/android-ndk-r27c
```

在 Windows PowerShell 中，虚拟环境中的 Python 路径为 `.\.venv\Scripts\python.exe`。使用 Windows 的 NDK 时，把 `--ndk` 参数替换成对应目录。支持用环境变量 `ANDROID_NDK_HOME` 指定同一个目录。

安装包是 `dist/codex_usb_ptp_touchpad_native-3.1.6-native-pen-controls.zip`。同目录的 `.zip.sha256` 和 `manifest.json` 分别给出整个包和内部文件的哈希。已有构建环境时运行同一条 Python 命令即可重新打包。
