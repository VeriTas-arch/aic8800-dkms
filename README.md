# AIC8800 DKMS Driver

本仓库提供 AIC8800 驱动的 DKMS 集成版本，目标是在 Ubuntu 平台上实现内核升级后的自动重建与自动安装。

驱动来源：

- [水星官方驱动](https://service.mercurycom.com.cn/download-2596.html)
- [原修改版驱动](https://github.com/bk1d/aic8800fdrvpackage)

## 目录说明

- `code/scripts`: 只编译验证、DKMS 安装、刷新、清理与版本同步脚本
- `code/src/AIC8800`: 驱动源码、固件与 udev 规则
- `code/VERSION`: 仓库内统一版本号来源
- `archive`: 上游历史安装包，仅作来源存档，不参与当前构建或安装

## 适用范围

- 已验证环境：Ubuntu 24.04、Linux 6.8.x、amd64
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

启用内核额外编译器警告并把编译器警告视为失败（`W=1 WERROR=1`；历史
kernel-doc 警告仍会单独输出）：

```bash
./code/scripts/build-test.sh --warnings 6.8.0-xx-generic
```

可用 `--config NAME=VALUE` 重复覆盖可选编译路径；安装了 `sparse` 时也可执行
语义检查：

```bash
./code/scripts/build-test.sh --config CONFIG_USB_RX_AGGR=y 6.8.0-xx-generic
./code/scripts/build-test.sh --sparse 6.8.0-xx-generic
```

仓库检查会验证脚本语法、版本一致性和空白错误，对所有
`/lib/modules/*/build` 编译默认配置，并在最新 headers 上以严格警告模式构建
默认配置及 USB 聚合、tasklet、预分配接收和共享消息端点等可选路径。已安装
`shellcheck`/`sparse` 时会同时运行对应检查：

```bash
./code/scripts/check.sh
```

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
- 同步 `dkms.conf` 中的 `PACKAGE_VERSION`
- 安装前用 `SHA256SUMS` 校验仓库中的固件
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

## 版本维护

统一版本文件为 `code/VERSION`。

需要同步版本号时执行：

```bash
chmod +x code/scripts/sync-version.sh
./code/scripts/sync-version.sh 1.0.9
```

该脚本会同步以下文件：

- `code/VERSION`
- `code/src/AIC8800/drivers/aic8800/dkms.conf`
- `code/src/DEBIAN/control`
- `code/src/AIC8800/drivers/aic8800/aic_dkms_version.h`

只检查上述四处版本是否一致：

```bash
./code/scripts/sync-version.sh --check
```

## 运行期诊断

建议在异常出现后尽快采集。脚本只读系统状态，默认对常见 MAC、SSID 和
IPv4 地址进行脱敏；以 root 运行才能读取 debugfs 和完整内核日志。

```bash
sudo ./code/scripts/collect-runtime-logs.sh --minutes 30 --output /tmp/aic8800-runtime.log
```

`runtime_stats` 首行包含 schema、计数范围、设备代次和运行时长。`conn_guard_*`
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

## DKMS 自动重建回归用例

目标：在不切换当前运行内核的前提下，模拟系统升级后 DKMS 自动重建行为。

1. 选择目标内核（非当前内核，且已安装 headers）

   ```bash
   uname -r
   ls -1 /lib/modules | sort
   ls -1 /usr/src | grep -E '^linux-headers-' | sort
   ```

2. 触发自动重建

   ```bash
   TARGET=6.8.0-90-generic
   sudo dkms autoinstall -k "$TARGET"
   dkms status | grep aic8800fdrv
   ```

3. 若提示同版本已存在，强制安装 DKMS 产物

   ```bash
   TARGET=6.8.0-90-generic
   VER="$(cat code/VERSION)"
   sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
   sudo dkms install -m aic8800fdrv -v "$VER" -k "$TARGET" --force
   ```

4. 验证目标内核模块路径

   ```bash
   TARGET=6.8.0-90-generic
   modinfo -k "$TARGET" aic8800_fdrv | grep '^filename'
   modinfo -k "$TARGET" aic_load_fw | grep '^filename'
   ```

   期望输出路径包含 `/lib/modules/<target>/updates/dkms/`。

5. 通过判定

- `dkms status` 出现 `aic8800fdrv/<version>, <target-kernel>, x86_64: installed`
- `modinfo -k <target-kernel>` 显示两个模块均来自 `updates/dkms`

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
