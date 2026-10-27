# PicoET-Enhance

<p align="center"><a href="docs/media/tongue-demo.mp4"><img src="docs/media/tongue-demo.gif" width="480" alt="Tongue tracking demo"></a><br><sub>PICO 4 Enterprise 舌头左右追踪，在头显上计算（v2.4.2-tongue.1）。点击图片查看原画质视频。</sub></p>

[English](README.md) | 简体中文

面向 PICO 4 Pro 的眼动增强 Magisk 模块：打开眼动服务的逐眼门控以输出真实瞳孔数据，解除面部表情模型的跨眼耦合让左右眼各自计算，并把眼部与嘴部表情目标频率从原厂约 24 Hz 提高到默认 90/90 Hz。配合 [PicoFacialBridge](https://github.com/WolalaQAQ/PicoFacialBridge) 和 [UnifiedPicoModule](https://github.com/WolalaQAQ/UnifiedPicoModule) 使用。

> v2.4.2 · PICO 4 Pro / PICO OS 5.13.7 · [更新日志](CHANGELOG_zh.md)

## 本分支：舌头左右（v2.4.2-tongue.1）

PICO 面部追踪只输出 TongueOut。本分支在**头显上**用小型分割模型从嘴部摄像头计算 **TongueLeft / TongueRight**，
通过 Wi-Fi 发送给 VRCFaceTracking（无需 ADB、PC 不处理图像、无需防火墙规则），眼部和面部追踪保持 90/90 Hz 目标。
下载：[v2.4.2-tongue.1](https://github.com/Rulez777/PicoET-Enhance/releases/tag/v2.4.2-tongue.1)；说明：[TONGUE.md](TONGUE.md)（英文）。
**个人模型**：仅用一个人的面部训练，其他人可能识别不准；未实现上下方向。

## 它做什么

PICO 4 Pro 的固件会对左右眼分别追踪，但面部表情模型在输出前把两眼混合，一侧的 Blink、视线方向、Wide 和眉部会受另一侧输入影响。眼部相机以 90 Hz 运行，这些表情却每秒只更新约 24 次。本模块在运行期做三处改动：

- **真实瞳孔数据**：打开眼动服务内部的逐眼门控，使真实瞳孔直径数据写入共享内存。
- **左右眼独立**：解除表情模型的跨眼耦合。EyeLook、Blink、Wide 和眉部通道逐眼独立计算与滤波，每眼维护自己的质量窗口、参考图和历史。滤波后的右内眉与逐眼质量状态通过 `EPI1` 扩展导出，由 PicoFacialBridge 以 `X` 包转发。
- **默认眼嘴 90/90 Hz**：眼部与嘴部表情各自在每个符合接纳条件的新相机输入上执行，同一帧的两路推理并行。降低频率是可选项，见[眼部与嘴部频率](#眼部与嘴部频率)。要让高频输出一直传到 VRChat，见[关于 90 Hz OSC](#关于-90-hz-osc)。

平滑、嘴部基线适应和质量窗口都按各自来源的时间戳推进：长时间缺帧会让质量窗口暂停，重复或倒退的来源不推进任何状态。

推理模型和融合视线保持原厂行为。每眼保留 Blink 端点、滤波及眼内 inner→outer 眉部联动，嘴部后处理保留原厂规则。暗图接纳、参考保护和模型量化仍启用。PC 端眼球方向与眼睑开合使用 EyeLook 和 `1−Blink`，原生眼数据仅提供实测瞳孔直径。

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

官方 VRCFaceTracking 搭配本模块可以正常使用。如果希望 OSC 接近 90 Hz，请使用 [VRCFT-Enhance](https://github.com/WolalaQAQ/VRCFT-Enhance)。它让宿主和模块进程不被 Windows 忽略定时器精度，把两处固定等待改为可设置项（默认 4 ms），并修正了宿主里几处没有同步的数据交换（等待缩短后它们更容易互相干扰）。它运行在一份独立的 VRCFaceTracking 副本里；Release 提供打好补丁的两个 DLL，仓库里同时保留源码补丁，想自己构建也可以。

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
    adb push magisk-picoet-enhance-v2.4.2.zip /data/local/tmp/
    adb shell su -c 'magisk --install-module /data/local/tmp/magisk-picoet-enhance-v2.4.2.zip'
   ```

3. 重启头显。

首次安装默认启用增强模式 `dual/on`，重启后以 90/90 Hz 运行。如果升级前已切到普通模式，升级后仍保持关闭。

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
| `status` | 显示保存与实际生效的模式、注入状态、服务 PID、眼／嘴有效目标频率及 `ro.pxr.externalfunc` 的当前值 |
| `dual` | 增强模式：同时设置 `dual/on`，打开瞳孔门控并启用逐眼独立表情 |
| `off` | 普通模式：同时设置 `off/off`，重启眼动服务且不注入，回到原版 |
| `toggle` | 增强已就绪时切到普通模式，否则尝试开启增强 |

切换模式会重启眼动服务并断开眼动、面捕客户端；有客户端连接时，命令行需要加 `--force`。Magisk App 的“操作”按钮直接切换增强与普通模式；增强加载失败后，再按会重试开启。

`on` 可作为 `dual` 的同义词。状态文件为 `/data/local/tmp/picoet-mode` 与 `/data/local/tmp/picoet-gate`，日志为 `/data/local/tmp/picoet-hook.log`。

### 自动加载与恢复

自动注入会等待 `sys.boot_completed=1`，并确认同一个眼动服务实例的 Binder 已响应。使用增强模式时，建议先确认 `picoet.sh status` 显示 `phase=ready applied_mode=dual applied_gate=on`，再启动 Bridge 等追踪客户端。

`ready` 表示 hook 已安装且目标频率已配置。如果引擎在 hook 前就已初始化，会保留该引擎，不追补 profile 0 或启动期 ET 配置；增强侧状态会重新热身，不强制重建引擎。

暂时找不到安全执行线程，或准备期间遇到临时信号停止时，注入器返回 `3`。看护脚本会释放控制锁，在锁外等待 30 秒后重试，不消耗两类预算。同步与异步不安全失败共用一次自动恢复机会，每次开机或显式模式命令重新计数。另有独立上限：最多自动注入三个连续不稳定的服务实例，只有同一实例连续保持 `ready` 满 60 秒才补充次数。预算耗尽后，自动注入暂停，直到下次开机或显式执行模式命令。

### 眼部与嘴部频率

默认眼部 90 Hz、嘴部 90 Hz，无需任何配置。要降低其中一项，把正整数写入对应文件：

| 文件 | 默认值 | 作用 |
|---|---:|---|
| `/data/local/tmp/picoet-eye-hz` | 90 | 眼部表情频率；90 表示处理每个符合接纳条件的新相机输入 |
| `/data/local/tmp/picoet-mouth-hz` | 90 | 嘴部表情频率，不能超过眼部频率，因为每个嘴帧同时也会计算眼部 |

大于 90 的值截为 90，文件缺失或内容无效时使用默认值。频率之外的帧会直接跳过而不计算。文件只在服务启动时读取，所以修改后需要先退出 Bridge 等追踪客户端，再运行 `dual` 重启服务来生效。例如眼部保持 90 Hz、嘴部降到原厂的 24 Hz：

```sh
echo 90 > /data/local/tmp/picoet-eye-hz
echo 24 > /data/local/tmp/picoet-mouth-hz
sh /data/adb/modules/picoet-enhance/picoet.sh dual
sh /data/adb/modules/picoet-enhance/picoet.sh status
```

`status` 中的 `effective_eye_hz` 和 `effective_mouth_hz` 是当前生效的配置目标，不是实测输出频率。要恢复 90/90，删除两个文件后再次运行 `dual`。

在 2.4.0 及之后版本设置的频率会在重装或升级时保留。只有首次从 2.4.0 之前的版本迁入时，才清除旧覆盖并以 90/90 起步。普通模式 `off` 完全是原厂行为，不受这些文件影响。

## 关于企业版门控

固件通过 `PXR::EyeUtil::isBusinessDev()` 控制逐眼输出，包括真实瞳孔直径。本模块在眼动服务内将该函数的返回值设为真；整机企业模式属性 `ro.pxr.externalfunc` 保持原值。

## 兼容性与验证情况

启动安全改动使用 NDK 26.1.10909125 的本地构建，在 PICO 4 Pro / PICO OS 5.13.7 / SELinux permissive 上完成验收，正常开机、升级、模式切换、重复控制命令及定点失败分支均通过。一次 45 秒真人佩戴验证确认了持续眼嘴更新、眨眼与张闭嘴响应、profile 0 初始化无重试，以及正常停止追踪，期间服务未重启。原厂库未修改。SELinux enforcing 尚未实测。2.4.2 的 CI 构建显式选用 NDK 26.1.10909125 与 CMake 3.22.1。

以下 90/90 数据来自此前的 2.4.0 验证，并非 2.4.2 重新测得的吞吐。设备为 PICO 4 Pro（PICO OS 5.13.7），保持唤醒、追踪就绪，未佩戴。测试用一套测试驱动程序复用模块的表情处理算法，回放 2,700 组录制的相机输入（约 30 秒），搭配 PicoFacialBridge 0.5.0、UnifiedPicoModule 0.4.0、VRCFT-Enhance 和桌面 VRChat：

| 环节 | 频率 |
|---|---:|
| 头显上的眼部与嘴部计算 | 89.80 Hz（2,700 组输入中完成 2,694 组；另 6 组遇到原厂的队列已满丢弃规则，推理无失败） |
| Bridge → PC（`X` 包） | 89.84 Hz |
| VRCFT → OSC，同一来源的眼部与嘴部 | 87.80 Hz（2,633 个来源，占输入的 97.5%） |

到达 OSC 的每个来源，VRChat 都回送了至少一项眼部值和一项嘴部值，与写入值相同。各环节的数值都与来源一致，覆盖范围是测试 Avatar 实际发送的受支持字段。loopback 抓包存在丢弃，因此 PC 侧的计数是可验证下界。从 `X` 到达 PC 到 OSC 输出，中位数为 9.1 ms，第 95 百分位为 12.5 ms。回放不是经已安装的 Magisk 包运行的，也不包含实时相机复制和 Virtual Desktop 的解码与渲染。

在 2.3.0 版本（嘴部约 24 Hz）上，另用官方 VRCFT 5.4.5 完成过一次 84 秒的真人佩戴 Virtual Desktop 运行，使用 Cymple 嘴部加 PICO 眼部：每秒约 89.7 个 `X` 包到达 PC，每包带新的眼部来源。

尚未覆盖：单眼质量失效、分时参考重建、嘴部输出的真机对照，以及长时间实机相机使用。融合视线保持原样；当前 Avatar 使用的宿主侧眉部参数仍对两侧取平均，尽管扩展会同时携带两侧数值。

## 关闭与卸载

运行 `picoet.sh off` 可关闭增强并恢复原厂眼动服务。要移除模块，在 Magisk 中卸载后重启头显。清理只删除已知状态文件，不递归进入意外目录或子挂载，因此这些目录可能保留。

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
