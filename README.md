# AIC8800 DKMS Driver

本仓库提供 AIC8800 驱动的 DKMS 集成版本，目标是在 Ubuntu 平台上实现内核升级后的自动重建与自动安装。

驱动来源：

- [水星 UX3H(免驱版) V1.0 Linux 系统驱动程序 20250118](https://service.mercurycom.com.cn/download-2917.html)
- [原修改版驱动](https://github.com/bk1d/aic8800fdrvpackage)

固件基线：

- 2026-09-03 核对的上述水星官方 ZIP，SHA-256 为
  `13551eb7d0fec9c6b96c23e7e4a04a7adc7d8401fb18c450bb40f6fb1316120a`
- 仓库中的 12 个 AIC8800DC 固件二进制与该官方包逐文件一致；
  `aic_userconfig_8800dc.txt` 仅省略了末尾空行
- 同一官方包中的 `aic_userconfig_8800dw.txt` 和
  `aic_userconfig_8800dw_2357.txt` 原样纳入清单；SHA-256 分别为
  `1acebc464acc5c3d512e81fee9870e044386af6a57d558f3d7bfe635bfd4de95` 和
  `414edbb2b724be2b74eb9732b5197cba2590be5670f8e585d436554f45d63992`
- TP-Link/Mercury `2357:0147` 使用产品专用 DW 配置；其他 DW 设备使用通用
  DW 配置，DC 设备继续使用 DC 配置。缺失或格式错误时按兼容顺序回退，
  最后才使用驱动内置默认值

## 目录说明

- `code/scripts`: 只编译验证、DKMS 安装、刷新、清理与版本同步脚本
- `code/src/AIC8800`: 驱动源码、固件与 udev 规则
- `code/VERSION`: 仓库内统一版本号来源
- `archive`: 上游历史安装包，仅作来源存档，不参与当前构建或安装

## 适用范围

- 已验证环境：Ubuntu 22.04.5 LTS（HWE）、Linux 6.8.x、amd64
- 主要场景：USB 设备上电后先枚举为存储态（Aic MSC），再通过规则触发切换到无线驱动态

## 前置依赖

```bash
sudo apt update
sudo apt install dkms build-essential linux-headers-$(uname -r)
```

## 快速开始

以下命令默认在仓库根目录执行。

### 只编译验证（推荐先执行）

该脚本在临时目录中编译 `aic_load_fw.ko` 和 `aic8800_fdrv.ko`，结束后自动清理。
它不会调用 `sudo`、DKMS、安装模块、加载模块或修改网络状态。

```bash
chmod +x code/scripts/build-test.sh
./code/scripts/build-test.sh
```

也可以指定已安装 headers 的目标内核：

```bash
./code/scripts/build-test.sh 6.8.0-xx-generic
```

严格警告、可选 USB 配置、`sparse` 和完整编译矩阵等维护者命令见
[AGENTS.md](AGENTS.md)。

当前 Ubuntu/DKMS 主线只验证仓库默认的 USB 总线路径。源码中保留的 SDIO
分支仍含上游遗留的 USB 专用结构依赖，不能作为受支持配置构建；本仓库的 USB
稳定性改动不会顺带改变该未验证分支的运行语义。

### 安装或刷新 DKMS

1. 本机当前内核安装

   ```bash
   chmod +x code/scripts/dkms-local-install.sh
   ./code/scripts/dkms-local-install.sh
   ```

2. 指定目标内核安装（可选）

   ```bash
   ./code/scripts/dkms-local-install.sh 6.8.0-xx-generic
   ```

3. 刷新所有已安装 headers 的内核（可选）

   ```bash
   chmod +x code/scripts/dkms-refresh-all-kernels.sh
   ./code/scripts/dkms-refresh-all-kernels.sh
   ```

4. 清理旧版本 DKMS 记录（可选）

   ```bash
   chmod +x code/scripts/dkms-clean-old-versions.sh
   ./code/scripts/dkms-clean-old-versions.sh
   ```

脚本行为说明：

- 在任何 DKMS 或系统文件变更前执行只编译预检
- `dkms-refresh-all-kernels.sh` 会先完成所有内核的构建，再开始任何内核的安装
- 刷新失败或被中断时，自动恢复同版本原有的 `/usr/src` 源码和 DKMS 构建/安装状态
- 单内核安装失败或被中断时，也会恢复该版本原有的 `/usr/src` 源码和目标内核状态
- 若旧 DKMS 状态涉及缺少 headers 的内核，全内核刷新会在任何变更前停止，避免删除无法重建的模块
- 复制源码目录 `code/src/AIC8800/drivers/aic8800` 到 `/usr/src/aic8800fdrv-<version>/`
- 复制时排除并复核 `.cmd`、`.o`、`Module.symvers` 等 Kbuild 临时产物，
  不删除开发工作树中的忽略文件
- 同步 `dkms.conf` 中的 `PACKAGE_VERSION`
- 安装前用 `SHA256SUMS` 校验仓库中的固件，并确认所有文件都恰好被清单覆盖
- 安装固件目录 `code/src/AIC8800/fw/aic8800DC` 到 `/lib/firmware/aic8800DC`
- 安装 udev 规则 `code/src/AIC8800/aic.rules` 到 `/etc/udev/rules.d/aic.rules`
- 只重新加载 udev 规则，不再对全系统设备执行无范围的 `udevadm trigger`；需要重新插拔 AIC 设备使规则生效
- 安装后逐内核验证 DKMS 状态、模块路径、`vermagic` 和嵌入模块的 `dkms_version`，并复核已安装固件校验和

`dkms-local-install.sh` 只用于该版本尚未登记到其他内核的情况；如果同一版本已
用于其他内核，脚本会拒绝覆盖共享的 `/usr/src` 源码，并提示改用全内核刷新脚本。
`dkms-clean-old-versions.sh --dry-run` 可先预览旧版本清理范围。

## 安装后验证

```bash
dkms status | grep aic8800fdrv
sudo modprobe aic_load_fw
sudo modprobe aic8800_fdrv
lsmod | grep -E "aic_load_fw|aic8800_fdrv"
```

如果设备仍停留在 Aic MSC，可重新加载规则并重新插拔设备：

```bash
sudo udevadm control --reload
# 重新插拔 AIC USB 设备后继续检查
ls -l /dev/aicudisk
sudo eject /dev/aicudisk
sudo dmesg -w | grep -Ei "aic|usb|firmware|rwnx"
```

不建议使用 `unbind/bind` 强制切换 USB 接口，这可能导致 USB 栈异常。

## 开发与维护

严格编译矩阵、版本同步与降级规则、脚本测试以及 DKMS 自动重建回归流程已整理到
[AGENTS.md](AGENTS.md)。README 仅保留安装、运行、诊断和恢复所需的操作说明。

## 运行期诊断

建议在异常出现后尽快采集。脚本只读系统状态，默认对常见 MAC、SSID 和
IPv4 地址进行脱敏；以 root 运行才能读取 debugfs 和完整内核日志。

```bash
sudo ./code/scripts/collect-runtime-logs.sh --minutes 30 --output /tmp/aic8800-runtime.log
```

`runtime_stats` 的 schema 3 包含计数范围、设备代次、运行时长、USB VID/PID、
实际采用的用户配置，以及配置加载、解析和回退计数。`conn_guard_*`
从模块初始化开始累计；其余计数按当前设备代次累计，USB 重新枚举后会从零开始。
重点比较同一 `device_generation` 下异常前后两次快照：命令池耗尽/高水位、总线
关闭拒绝、发送失败、超时或过大 CFM，固件连接拒绝，USB 数据端点与消息端点各自
的提交/完成/终止、队列溢出和非法长度，以及固件消息、A-MSDU/monitor 元数据和
固件日志丢弃。计数保持为零是健康基线；非零不一定代表持续故障，应结合增量和
同一时间段的内核日志判断。

自动漫游时，`conn_guard_noncanonical_roamed` 可能因固件以 `0x20` 等非零值表示
布尔真而增长，该计数本身不代表漫游失败。`conn_guard_synth_roam` 仅统计固件未
提供漫游提示、但驱动根据活动事务或 BSSID 变化恢复语义的情况。RSSI/CQM 的
高低阈值事件、非规范状态值和无效 VIF 分别记录在 `cqm_rssi_*` 计数中，可与
NetworkManager 和 wpa_supplicant 的时间线对照分析漫游原因。

采集结果还包含启动 ID、源码提交与工作树状态、已加载模块和磁盘模块的
`srcversion`/`vermagic`、USB 拓扑、接口统计、驱动计数以及 NetworkManager 日志，
可用于区分驱动异常、USB 总线异常和上游网络问题。

## 清理回滚

仅清理某个目标内核：

```bash
TARGET=6.8.0-90-generic
VER="$(cat code/VERSION)"
sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
sudo dkms remove -m aic8800fdrv -v "$VER" -k "$TARGET" || true
```

清理全部内核：

```shell
sudo dkms remove -m aic8800fdrv -v "$(cat code/VERSION)" --all
```
