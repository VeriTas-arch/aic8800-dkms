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

启用内核额外警告检查：

```bash
./code/scripts/build-test.sh --warnings 6.8.0-xx-generic
```

检查脚本语法、版本一致性、空白错误，并对所有 `/lib/modules/*/build`
执行只编译矩阵：

```bash
./code/scripts/check.sh
```

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
- 复制源码目录 `code/src/AIC8800/drivers/aic8800` 到 `/usr/src/aic8800fdrv-<version>/`
- 同步 `dkms.conf` 中的 `PACKAGE_VERSION`
- 安装前用 `SHA256SUMS` 校验仓库中的固件
- 安装固件目录 `code/src/AIC8800/fw/aic8800DC` 到 `/lib/firmware/aic8800DC`
- 安装 udev 规则 `code/src/AIC8800/aic.rules` 到 `/etc/udev/rules.d/aic.rules`
- 只重新加载 udev 规则，不再对全系统设备执行无范围的 `udevadm trigger`；需要重新插拔 AIC 设备使规则生效

`dkms-local-install.sh` 只重建指定内核，不再删除同版本在其他内核上的
DKMS 状态。`dkms-clean-old-versions.sh --dry-run` 可先预览旧版本清理范围。

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

只检查三处版本是否一致：

```bash
./code/scripts/sync-version.sh --check
```

## 运行期诊断

建议在异常出现后尽快采集。脚本只读系统状态，默认对常见 MAC、SSID 和
IPv4 地址进行脱敏；以 root 运行才能读取 debugfs 和完整内核日志。

```bash
sudo ./code/scripts/collect-runtime-logs.sh --minutes 30 --output /tmp/aic8800-runtime.log
```

`runtime_stats` 是从模块初始化开始累计的计数，不会按采集窗口自动清零。
重点比较异常前后两次快照：USB 提交/完成错误、终止状态、短帧或非法长度、
固件消息非法、A-MSDU/monitor 元数据非法以及固件日志丢弃计数。计数保持为零
是健康基线；非零不一定代表持续故障，应结合增量和同一时间段的内核日志判断。

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
