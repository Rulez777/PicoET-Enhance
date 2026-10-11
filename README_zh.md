# PicoET-Enhance

[English](README.md) | 简体中文

面向 PICO 4 Pro 的眼动增强 Magisk 模块：让头显的眼动服务输出真实的逐眼视线与瞳孔数据，配合 [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) 和 [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule) 使用。

> v2.1.0 · PICO 4 Pro / PICO OS 5.13.7 · [更新日志](CHANGELOG_zh.md)

## 它做什么

PICO 4 Pro 的固件本身就能算出左右眼各自的视线，但默认只对外提供融合后的结果。本模块在运行期对该行为做两处修正：

- 打开眼动服务内部的逐眼门控，使逐眼视线与真实瞳孔数据被写入共享内存；
- 让眼动算法同时运行左右眼的单眼模型，得到真正的分眼视线向量（`dual` 模式）。

dual 模式下，两眼分别输出单眼推理结果。一眼追踪失效时，另一眼继续更新，PC 模块保持失效眼最近的有效值。OpenXR 等应用读取的融合视线沿用原厂推理和滤波。

增强通过运行期 hook 作用于眼动服务，随服务重启而卸载。

## 与其他组件的关系

```text
PicoET-Enhance（头显，本模块）
        ↓ 运行期生效，通告当前模式
PicoFacialBridge（头显）
        ↓ BridgeSplit，局域网 UDP 9030
UnifiedPicoModule（PC）
        ↓
VRCFaceTracking → VRChat OSC
```

PicoFacialBridge 读取本模块公布的临时模式属性，自动告知 PC 当前处于普通还是增强模式。

建议搭配 UnifiedPicoModule 0.2.0 或更新版本。PC 端默认按模式启用自适应平滑，参数见 [UnifiedPicoModule 配置说明](https://github.com/WolalaQAQ/UnifiedPicoModule/blob/main/docs/configuration_zh.md#视线平滑)。

## 环境要求

| 项目 | 要求 |
|---|---|
| 头显 | PICO 4 Pro |
| 固件 | PICO OS 5.13.7（Android 10 / API 29，arm64-v8a） |
| Root | 已安装 Magisk |
| 配套组件 | PicoFacialBridge 与 UnifiedPicoModule，用于把数据接入 VRCFaceTracking |

安装脚本会校验设备眼动库的 SHA-256，固件版本不匹配时停止安装。

## 安装

1. 从 [Releases](https://github.com/WolalaQAQ/PicoET-Enhance/releases) 下载 `magisk-picoet-enhance-v<版本>.zip`。
2. 在 Magisk App 中通过“从本地安装”刷入，或使用命令行：

   ```sh
   adb push magisk-picoet-enhance-v2.1.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.1.0.zip'
   ```

3. 重启头显。

首次安装默认启用增强模式 `dual/on`，重启后自动生效。选择普通模式后，后续重启会保持关闭。

## 使用

在头显上以 Root 运行：

```sh
sh /data/adb/modules/picoet-enhance/picoet.sh status
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh off
sh /data/adb/modules/picoet-enhance/picoet.sh toggle
```

| 命令 | 作用 |
|---|---|
| `status` | 显示保存的模式、注入状态、服务 PID 与 `ro.pxr.externalfunc` 的当前值 |
| `dual` | 增强模式：同时设置 `dual/on`，启用真实逐眼视线与瞳孔数据 |
| `off` | 普通模式：同时设置 `off/off`，重启眼动服务且不注入，回到原版 |
| `toggle` | 增强已就绪时切到普通模式，否则尝试开启增强 |

切换模式会重启眼动服务并断开眼动、面捕客户端；有客户端连接时，命令行需要加 `--force`。Magisk App 的“操作”按钮直接切换增强与普通模式；增强加载失败后，再按会重试开启。

状态文件为 `/data/local/tmp/picoet-mode` 与 `/data/local/tmp/picoet-gate`，日志为 `/data/local/tmp/picoet-hook.log`。

## 关于企业版门控

固件通过 `PXR::EyeUtil::isBusinessDev()` 控制逐眼数据写入。本模块在眼动服务内将该函数的返回值设为真，启用逐眼输出；整机企业模式属性 `ro.pxr.externalfunc` 保持原值。

## 兼容性与验证情况

已在 PICO 4 Pro / PICO OS 5.13.7 / SELinux Permissive 下观察逐眼独立输出，单眼失效时另一眼持续更新；VD 1.34.22.0 的高清区跟随和菜单眼动指针正常。新版控制脚本的两档切换、相对路径启动及 Enforcing 下的规则尚待实机验证。

## 关闭与卸载

运行 `picoet.sh off` 可关闭增强并恢复原厂眼动服务。要移除模块，在 Magisk 中卸载后重启头显。

## 从源码构建

构建需要 Android NDK 与 CMake/Ninja。脚本会从 `-Ndk` / `-CmakeBin` 参数、`PICOET_NDK` / `PICOET_CMAKE_BIN` 环境变量，或 `ANDROID_HOME` 下的标准目录中查找工具链。

```powershell
pwsh -File hook\build-hook.ps1                 # 构建 injector 与 payload
pwsh -File build-picoet-enhance.ps1            # 打包成 Magisk zip
```

产物为 `artifacts/magisk-picoet-enhance-v<版本>.zip`，包含模块脚本、注入器和 hook 载荷。打包时会检查 ELF 文件清单和原厂库哈希。

推送 `v<版本>` 标签后，GitHub Actions 会在 Windows 运行器上构建并发布带校验和的 zip。

## 待做

- [ ] **舌头方向增强**：PICO 目前只对外提供伸舌量 `TongueOut` 一个标量，而 VRCFaceTracking 已定义了 `TongueLeft/Right/Up/Down` 等方向通道。计划先取下脸相机的原始帧，用视觉分割基模自动打标，再训练一个小模型，并允许用户采集自己的数据在本地做在线微调。
- [ ] **面捕不滤波输出**：把端侧 FaceTrackor 的时间平滑做成可选关闭，让模块直接转发逐帧原始面部数据。时间滤波改由 PC 端按需处理，端侧只负责发原始值。

## 反馈

欢迎提交 Issue。请提供头显型号、PICO OS 版本、模块版本、当前模式，以及 `picoet.sh status` 的输出与相关日志片段。

## 许可证

本项目原创代码与脚本采用 [MIT](LICENSE)。`hook/third_party/shadowhook-2.0.1/` 为 ByteDance ShadowHook 2.0.1 的源码副本，采用 MIT 许可。本项目不包含也不分发任何 PICO 二进制，仅在运行期按数字偏移引用设备自身的库。详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
