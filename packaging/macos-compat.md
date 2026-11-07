# macOS 10.15 / 11 兼容构建

交付两份独立安装包：Intel x86_64 最低 10.15，Apple Silicon arm64 最低 11.0。Qt 5.15.19 从官方固定 SHA256 的源码构建，Go helper 固定 Go 1.21.13。不要使用开发机 Homebrew Qt/SSL 动态库。运行以下命令前准备 Python 3.12、CMake、Ninja、Apple 编译工具和独立安装的 Go 1.21.13；`OSM_GO` 指向其可执行文件，不修改系统 Go。

## 原生构建

以下为 Apple Silicon 示例；Intel 原生机器将架构改为 `x86_64`、部署目标改为 `10.15`，并使用独立目录。CI 在两种架构的原生 runner 上执行此路径，固定 Xcode 16.4。

```sh
export OSM_GO=/path/to/go1.21.13/bin/go
python3 scripts/build-qt.py --prefix /tmp/osm-qt-arm64 \
  --work /tmp/osm-qt-build-arm64 --arch arm64 --deployment-target 11.0
cmake -S . -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/osm-qt-arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DGO_EXECUTABLE="$OSM_GO"
cmake --build build-arm64 --parallel 4
ctest --test-dir build-arm64 --output-on-failure
python3 scripts/package-macos.py --build build-arm64 \
  --macdeployqt /tmp/osm-qt-arm64/bin/macdeployqt \
  --output dist/arm64/OSMClient.app --deployment-target 11.0
python3 scripts/package-dmg.py --app dist/arm64/OSMClient.app \
  --output dist/OSMClient-4.1.2-macos-arm64.dmg --deployment-target 11.0
```

## Apple Silicon 交叉构建 Intel

无需 Rosetta。先完成上述 arm64 Qt 安装，再使用同版本原生 qmake/moc/rcc/uic 生成 Intel Qt 库和应用。Qt 构建工具使用 `--host-bindir`，应用使用 `CLIENT_QT_HOST_BINS`。Go 工具链可在 arm64 主机生成 amd64 helper。

```sh
python3 scripts/build-qt.py --prefix /tmp/osm-qt-x86_64 \
  --work /tmp/osm-qt-build-x86_64 --arch x86_64 --deployment-target 10.15 \
  --host-bindir /tmp/osm-qt-arm64/bin
cmake -S . -B build-x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/osm-qt-x86_64 \
  -DCLIENT_QT_HOST_BINS=/tmp/osm-qt-arm64/bin \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  -DGO_EXECUTABLE="$OSM_GO" -DBUILD_TESTING=OFF
cmake --build build-x86_64 --parallel 4
python3 scripts/package-macos.py --build build-x86_64 \
  --macdeployqt /tmp/osm-qt-arm64/bin/macdeployqt \
  --plugins-dir /tmp/osm-qt-x86_64/plugins --libpath /tmp/osm-qt-x86_64/lib \
  --output dist/x86_64/OSMClient.app --deployment-target 10.15 --skip-smoke
python3 scripts/package-dmg.py --app dist/x86_64/OSMClient.app \
  --output dist/OSMClient-4.1.2-macos-x86_64.dmg \
  --deployment-target 10.15 --skip-smoke
```

`--plugins-dir` 指定目标架构插件，并使用脚本自身的递归 Mach-O 依赖收集器；该路径不调用 macdeployqt，避免它从宿主 Qt 前缀混入 arm64 框架。`--macdeployqt` 参数为统一命令接口保留。没有 Rosetta 时，Intel 包无法在 ARM 主机启动，显式 `--skip-smoke` 将这一限制写入 manifest；原生 CI 不跳过启动检查。

## 包验证与签名

应用和挂载后的 DMG 都递归检查所有 Mach-O：架构一致、非系统依赖全部位于包内、无外部 rpath，且每个二进制的 minOS 不超过目标。应用 Info.plist 同步写入 `LSMinimumSystemVersion`。不满足条件立即失败，不能靠修改 plist 将 macOS 27 库伪装成旧系统库。签名按嵌套二进制、框架、主程序、应用顺序执行。

默认仅 ad-hoc 签名。正式签名/公证及缺证书跳过逻辑见 [CI 说明](ci.md)。构建输出路径必须不存在，重试时使用新目录。

2026-11-06 已用独立 Qt 源码、Go 1.21.13 实际生成两种 DMG。ARM 包通过 Cocoa smoke（包括 DMG 挂载内启动）；Intel 包在无 Rosetta 的 ARM 主机交叉构建，完成镜像、架构、minOS、依赖及签名完整性校验。此次本地编译使用 Apple clang 21 / Command Line Tools；固定 Xcode 16.4 的 CI 尚未执行。macOS 10.15/11 实机运行、真实用户登录与 SSH、Developer ID 签名和公证仍待目标环境验收。Mach-O 下限检查不能替代旧系统实机验证。
