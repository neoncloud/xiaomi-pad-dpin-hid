#!/usr/bin/env python3
"""Build a KernelSU ZIP locally from the supported vendor service and ShadowHook."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from build_ptp_native_assets import write_header


ROOT = Path(__file__).resolve().parents[1]
AAR_URL = ("https://repo.maven.apache.org/maven2/com/bytedance/android/shadowhook/"
           "2.0.1/shadowhook-2.0.1.aar")
AAR_SHA256 = "2cad01ff4d59542958775e19f281981a7e84b39670b5fae75905b8511e10c7cd"
MAX_AAR_BYTES = 4 * 1024 * 1024


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ndk_compiler(ndk: Path) -> Path:
    hosts = {
        "Windows": "windows-x86_64",
        "Linux": "linux-x86_64",
        "Darwin": "darwin-x86_64",
    }
    host = platform.system()
    if host not in hosts:
        raise ValueError(f"Unsupported Android NDK host: {host}")
    compiler = ndk / "toolchains" / "llvm" / "prebuilt" / hosts[host] / "bin" / (
        "clang.exe" if host == "Windows" else "clang"
    )
    if not compiler.is_file():
        raise ValueError(f"NDK clang not found: {compiler}")
    return compiler


def load_shadowhook(path: Path | None, work: Path) -> dict[str, bytes]:
    if path is None:
        path = work / "shadowhook.aar"
        with urllib.request.urlopen(AAR_URL, timeout=30) as response:
            data = response.read(MAX_AAR_BYTES + 1)
        if len(data) > MAX_AAR_BYTES:
            raise ValueError("ShadowHook download exceeded the expected size")
        path.write_bytes(data)
    if sha256(path.read_bytes()) != AAR_SHA256:
        raise ValueError("ShadowHook 2.0.1 AAR hash mismatch")

    libraries = {}
    with zipfile.ZipFile(path) as archive:
        header = archive.read("prefab/modules/shadowhook/include/shadowhook.h")
        (work / "shadowhook.h").write_bytes(header)
        for name in ("libshadowhook.so", "libshadowhook_nothing.so"):
            libraries[name] = archive.read(f"jni/arm64-v8a/{name}")
            (work / name).write_bytes(libraries[name])
    return libraries


def zig_command() -> list[str]:
    if importlib.util.find_spec("ziglang") is not None:
        return [sys.executable, "-m", "ziglang"]
    zig = shutil.which("zig")
    if zig:
        return [zig]
    raise ValueError("Zig is missing. Install the build requirements first.")


def write_package(output: Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, contents in sorted(files.items()):
            entry = zipfile.ZipInfo(name)
            entry.create_system = 3
            entry.compress_type = zipfile.ZIP_DEFLATED
            executable = name.endswith((".sh", ".real", "vendor.xiaomi.usb.dp-service"))
            entry.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            archive.writestr(entry, contents)


def build(stock: Path, ndk: Path, aar: Path | None, output_dir: Path) -> Path:
    compiler = ndk_compiler(ndk)
    stock = stock.resolve(strict=True)
    zig = zig_command()
    module = ROOT / "module"
    source = ROOT / "src"
    props = dict(line.split("=", 1) for line in (module / "module.prop").read_text().splitlines())
    with tempfile.TemporaryDirectory(prefix="dpin-hid-") as temporary:
        work = Path(temporary)
        write_header(stock, work / "ptp_native_generated.h")
        shadowhook = load_shadowhook(aar, work)
        shim = work / "libptp_auto.so"
        launcher = work / "launcher"

        subprocess.run([
            str(compiler), "--target=aarch64-linux-android29", "-shared", "-fPIC", "-O2",
            "-fno-sanitize=all", "-Wall", "-Wextra", "-Wno-unused-parameter",
            f"-I{source}", f"-I{work}", f"-L{work}",
            "-Wl,-soname,libptp_auto.so", "-Wl,-z,max-page-size=16384",
            "-Wl,--allow-shlib-undefined", "-o", str(shim), str(source / "ptp_native.c"),
            "-lshadowhook", "-llog", "-ldl", "-lm",
        ], check=True)
        subprocess.run([
            *zig, "cc", "-target", "aarch64-linux-musl", "-static", "-O2", "-s",
            "-o", str(launcher), str(source / "native_ptp_launcher.c"),
        ], check=True)

        files = {path.name: path.read_bytes() for path in module.iterdir() if path.is_file()}
        files.update({
            "system/odm/bin/vendor.xiaomi.usb.dp-service": launcher.read_bytes(),
            "system/odm/bin/vendor.xiaomi.usb.dp-service.real": stock.read_bytes(),
            "system/odm/lib64/libptp_auto.so": shim.read_bytes(),
            "LICENSES/ShadowHook.txt": (ROOT / "third_party" / "ShadowHook-LICENSE").read_bytes(),
        })
        files.update({f"system/odm/lib64/{name}": data for name, data in shadowhook.items()})

    output_dir.mkdir(parents=True, exist_ok=True)
    package = output_dir / f"{props['id']}-{props['version']}.zip"
    write_package(package, files)
    digest = sha256(package.read_bytes())
    package.with_suffix(".zip.sha256").write_text(f"{digest}  {package.name}\n", encoding="ascii")
    manifest = {
        "module_id": props["id"],
        "version": props["version"],
        "version_code": int(props["versionCode"]),
        "zip": package.name,
        "zip_sha256": digest,
        "files": {name: {"length": len(data), "sha256": sha256(data)}
                  for name, data in sorted(files.items())},
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return package


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True, help="original vendor service ELF")
    parser.add_argument("--ndk", type=Path, default=os.environ.get("ANDROID_NDK_HOME"),
                        help="Android NDK directory (or ANDROID_NDK_HOME)")
    parser.add_argument("--shadowhook-aar", type=Path, help="optional local ShadowHook 2.0.1 AAR")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    if args.ndk is None:
        parser.error("provide --ndk or set ANDROID_NDK_HOME")
    try:
        package = build(args.stock, args.ndk.resolve(), args.shadowhook_aar, args.output_dir)
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Build failed: {error}\n")
    print(f"Built {package}")


if __name__ == "__main__":
    main()
