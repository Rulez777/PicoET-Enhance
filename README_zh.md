# PicoET-Enhance

[English](README.md) | 简体中文

面向 PICO 4 Pro 的眼动增强 Magisk 模块：打开眼动服务的逐眼门控以输出真实瞳孔数据，并解除面部表情模型的跨眼耦合，让左右眼各自计算，配合 [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) 和 [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule) 使用。

> v2.3.0 · PICO 4 Pro / PICO OS 5.13.7 · [更新日志](CHANGELOG_zh.md)

## 它做什么

PICO 4 Pro 的固件会对左右眼分别追踪，但面部表情模型在输出前把两眼混合：一侧的 Blink、视线方向、Wide 和眉部会受另一侧输入影响。本模块在运行期做两处修正：

- 打开眼动服务内部的逐眼门控，使真实瞳孔直径数据写入共享内存；
- 解除表情模型的跨眼耦合：EyeLook、Blink、Wide 和眉部通道逐眼独立计算与滤波，每眼维护自己的质量窗口、参考图和历史。滤波后的右内眉与逐眼质量状态通过 `EPI1` 扩展导出，由 PicoFacialBridge 以 `X` 包转发。

完整眼部表情随新相机输入执行，原厂嘴部慢链维持约 24 Hz，并在这些帧上并行运行眼／嘴推理。眼部平滑按本眼源时间间隔换算，质量窗口在缺帧期间暂停；重复或倒退源时间不推进本眼状态。

融合视线和嘴部算法保持原厂行为。每眼保留 Blink 端点、滤波及眼内 inner→outer 眉部联动；暗图接纳、参考保护和模型量化仍启用。PC 端眼球方向与眼睑开合使用 EyeLook 和 `1−Blink`，原生眼数据仅提供实测瞳孔直径。

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

高频路径使用 `EPI1`／`X` v2，需要 PicoFacialBridge 0.5.0 及以上与 UnifiedPicoModule 0.4.0 及以上。此前的 PicoET-Enhance 2.2.0 搭配 PicoFacialBridge 0.4.0、UnifiedPicoModule 0.3.0，通过 v1 提供原厂表情节奏下的逐眼独立。较旧的 Bridge（0.3 及以上）在普通模式下仍可使用，无需本模块。

### 关于 90 Hz OSC

眼部路径送到 PC 的频率约为 90 Hz，但官方 VRCFaceTracking 宿主再把它交给 OSC 时会降频：宿主以固定的 10 ms 等待读取模块输出并写出 OSC。我们在 PICO 4 Pro 经 Virtual Desktop 串流的环境下，按原始 90 Hz 节奏回放 2,700 帧录制的眼部数据，统计 OSC 输出端每秒的新眼部方向数：

| VRCFT 宿主（5.4.5，Steam 版 `7add148`） | 每秒新眼部方向 |
|---|---:|
| 官方原版 | 约 61–78 Hz |
| 打补丁，4 ms 等待 | 约 87–88 Hz |

Windows 11 可能会忽略官方宿主申请的 1 ms 定时器精度，这时每次 10 ms 等待实际要 15 ms 左右，所以官方原版每次测量的结果都有起伏。

以上数据来自 Ryzen 7 9700X、RTX 4070 Ti SUPER、64 GB 内存、Windows 11 专业工作站版，电源计划为卓越性能。其他电脑的结果可能不同。

官方 VRCFaceTracking 搭配本模块可以正常使用。如果希望 OSC 接近 90 Hz，请使用 [osc90-host-patch](https://github.com/WolalaQAQ/osc90-host-patch)。它让宿主和模块进程不被 Windows 忽略定时器精度，把两处固定等待改为可设置项（默认 4 ms），并修正了宿主里几处没有同步的数据交换（等待缩短后它们更容易互相干扰）。这是源码补丁，需要自己构建，并放在一份独立的 VRCFaceTracking 副本里运行；不分发 VRCFaceTracking 的二进制文件。
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
   adb push magisk-picoet-enhance-v2.3.0.zip /data/local/tmp/
   adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.3.0.zip'
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

高频路径已在 PICO 4 Pro / PICO OS 5.13.7、PicoFacialBridge 0.5.0、UnifiedPicoModule 0.4.0 与官方 VRCFT 5.4.5 上完成验证。一次 84 秒的真人佩戴验证经 Virtual Desktop 串流，使用 Cymple 嘴部加 PICO 眼部，Bridge 每秒向 PC 发出约 89.7 个 `X` 包，每个包都带新的眼部源；仅 PICO 的眼部加面部（不开 Cymple）也运行正常。此前的录制输入测试覆盖了推理吞吐与传输。

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
- [ ] **眼追与面捕同步 90 Hz**：眼部表情已按相机速率运行，嘴部原厂链路仍约 24 Hz。目标是把面部模型也提到同一速率，使眼和嘴的数值一起以 90 Hz 到达 PC 并输出为 OSC；串流期间两个模型需共用头显的算力预算。

## 反馈

欢迎提交 Issue。请提供头显型号、PICO OS 版本、模块版本、当前模式，以及 `picoet.sh status` 的输出与相关日志片段。

## 许可证

本项目原创代码与脚本采用 [MIT](LICENSE)。内置的 ShadowHook 2.0.1 源码包含 MIT／BSD 许可的依赖，Magisk ZIP 内附相应声明与项目许可。本项目不包含也不分发任何 PICO 二进制，仅在运行期按数字偏移引用设备自身的库。详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
