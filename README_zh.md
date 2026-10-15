# PicoET-Enhance

[English](README.md) | 简体中文

面向 PICO 4 Pro 的眼动增强 Magisk 模块：打开眼动服务的逐眼门控以输出真实瞳孔数据，并解除面部表情模型的跨眼耦合，让左右眼各自计算，配合 [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) 和 [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule) 使用。

> v2.2.0 · PICO 4 Pro / PICO OS 5.13.7 · [更新日志](CHANGELOG_zh.md)

## 它做什么

PICO 4 Pro 的固件会对左右眼分别追踪，但面部表情模型在输出前把两眼混合：一侧的 Blink、视线方向、Wide 和眉部会受另一侧输入影响。本模块在运行期做两处修正：

- 打开眼动服务内部的逐眼门控，使真实瞳孔直径数据写入共享内存；
- 解除表情模型的跨眼耦合：EyeLook、Blink、Wide 和眉部通道逐眼独立计算与滤波，每眼维护自己的质量窗口、参考图和历史。滤波后的右内眉与逐眼质量状态通过 `EPI1` 扩展导出，由 PicoFacialBridge 以 `X` 包转发。

嘴部处理与融合视线保持原样。每眼保留原有的本眼规则：Blink 端点、平滑，以及眼内 inner→outer 眉部联动。

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

逐眼独立表情需要头显端 PicoET-Enhance 2.2.0 及以上、PicoFacialBridge 0.4.0 及以上，以及 PC 端 UnifiedPicoModule 0.3.0 及以上。较旧的 Bridge（0.3 及以上）在普通模式下仍可使用，无需本模块。

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
   adb push magisk-picoet-enhance-v2.2.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.2.0.zip'
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
| `dual` | 增强模式：同时设置 `dual/on`，打开瞳孔门控并启用逐眼独立表情 |
| `off` | 普通模式：同时设置 `off/off`，重启眼动服务且不注入，回到原版 |
| `toggle` | 增强已就绪时切到普通模式，否则尝试开启增强 |

切换模式会重启眼动服务并断开眼动、面捕客户端；有客户端连接时，命令行需要加 `--force`。Magisk App 的“操作”按钮直接切换增强与普通模式；增强加载失败后，再按会重试开启。

`on` 可作为 `dual` 的同义词。状态文件为 `/data/local/tmp/picoet-mode` 与 `/data/local/tmp/picoet-gate`，日志为 `/data/local/tmp/picoet-hook.log`。

## 关于企业版门控

固件通过 `PXR::EyeUtil::isBusinessDev()` 控制逐眼输出，包括真实瞳孔直径。本模块在眼动服务内将该函数的返回值设为真；整机企业模式属性 `ro.pxr.externalfunc` 保持原值。

## 兼容性与验证情况

逐眼表情路径已在 PICO 4 Pro / PICO OS 5.13.7 上完成验证：录制输入可复现独立输出，与原厂基线在全部非眼槽和嘴有效位上一致。端到端方面，事先录制的眼图在头显上经原生眼部推理回放，经 PicoFacialBridge 0.4.0 与 UnifiedPicoModule 0.3.0 到达官方 VRCFT 5.4.5，收到的 `E`、`X` 数值与源输出一致。录制约为 23.7 Hz 并以加速循环回放，因此验证的是推理吞吐与传输，不是真实 90 Hz 相机序列。

尚未覆盖：单眼质量失效、分时参考重建、嘴部输出的真机对照，以及长时间实机相机使用。嘴部共享处理与融合视线保持原样；当前 Avatar 使用的宿主侧眉部参数仍对两侧取平均，尽管扩展会同时携带两侧数值。

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

本项目原创代码与脚本采用 [MIT](LICENSE)。内置的 ShadowHook 2.0.1 源码包含 MIT／BSD 许可的依赖，Magisk ZIP 内附相应声明与项目许可。本项目不包含也不分发任何 PICO 二进制，仅在运行期按数字偏移引用设备自身的库。详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
