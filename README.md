# AIC8800 DKMS Driver

本仓库提供 AIC8800 驱动的 DKMS 集成版本，目标是在 Ubuntu 平台上实现内核升级后的自动重建与自动安装。

驱动来源：

- [水星官方驱动](https://service.mercurycom.com.cn/download-2596.html)
- [原修改版驱动](https://github.com/bk1d/aic8800fdrvpackage)

## 目录说明

- `code/scripts`: DKMS 安装、刷新、清理与版本同步脚本
- `code/src/AIC8800`: 驱动源码、固件与 udev 规则
- `code/VERSION`: 仓库内统一版本号来源

## 适用范围

- 已验证环境：Ubuntu 24.04、Linux 6.8.x、amd64
- 主要场景：USB 设备上电后先枚举为存储态（Aic MSC），再通过规则触发切换到无线驱动态

## 前置依赖

```shell
sudo apt update
sudo apt install dkms build-essential linux-headers-$(uname -r)
```

## 快速开始

以下命令默认在仓库根目录执行。

1. 本机当前内核安装

   ```shell
   chmod +x code/scripts/dkms-local-install.sh
   ./code/scripts/dkms-local-install.sh
   ```

2. 指定目标内核安装（可选）

   ```shell
   ./code/scripts/dkms-local-install.sh 6.8.0-xx-generic
   ```

3. 刷新所有已安装 headers 的内核（可选）

   ```shell
   chmod +x code/scripts/dkms-refresh-all-kernels.sh
   ./code/scripts/dkms-refresh-all-kernels.sh
   ```

4. 清理旧版本 DKMS 记录（可选）

   ```shell
   chmod +x code/scripts/dkms-clean-old-versions.sh
   ./code/scripts/dkms-clean-old-versions.sh
   ```

脚本行为说明：

- 复制源码目录 `code/src/AIC8800/drivers/aic8800` 到 `/usr/src/aic8800fdrv-<version>/`
- 同步 `dkms.conf` 中的 `PACKAGE_VERSION`
- 安装固件目录 `code/src/AIC8800/fw/aic8800DC` 到 `/lib/firmware/aic8800DC`
- 安装 udev 规则 `code/src/AIC8800/aic.rules` 到 `/etc/udev/rules.d/aic.rules`

## 安装后验证

```shell
dkms status | grep aic8800fdrv
sudo modprobe aic_load_fw
sudo modprobe aic8800_fdrv
lsmod | grep -E "aic_load_fw|aic8800_fdrv"
```

如果设备仍停留在 Aic MSC，可做一次安全热触发：

```shell
sudo udevadm control --reload
sudo udevadm trigger
ls -l /dev/aicudisk
sudo eject /dev/aicudisk
sudo dmesg -w | grep -Ei "aic|usb|firmware|rwnx"
```

不建议使用 `unbind/bind` 强制切换 USB 接口，这可能导致 USB 栈异常。

## 版本维护

统一版本文件为 `code/VERSION`。

需要同步版本号时执行：

```shell
chmod +x code/scripts/sync-version.sh
./code/scripts/sync-version.sh 1.0.9
```

该脚本会同步以下文件：

- `code/VERSION`
- `code/src/AIC8800/drivers/aic8800/dkms.conf`
- `code/src/DEBIAN/control`

## DKMS 自动重建回归用例

目标：在不切换当前运行内核的前提下，模拟系统升级后 DKMS 自动重建行为。

1. 选择目标内核（非当前内核，且已安装 headers）

   ```shell
   uname -r
   ls -1 /lib/modules | sort
   ls -1 /usr/src | grep -E '^linux-headers-' | sort
   ```

2. 触发自动重建

   ```shell
   TARGET=6.8.0-90-generic
   sudo dkms autoinstall -k "$TARGET"
   dkms status | grep aic8800fdrv
   ```

3. 若提示同版本已存在，强制安装 DKMS 产物

   ```shell
   TARGET=6.8.0-90-generic
   VER="$(cat code/VERSION)"
   sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
   sudo dkms install -m aic8800fdrv -v "$VER" -k "$TARGET" --force
   ```

4. 验证目标内核模块路径

   ```shell
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

```shell
TARGET=6.8.0-90-generic
VER="$(cat code/VERSION)"
sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
sudo dkms remove -m aic8800fdrv -v "$VER" -k "$TARGET" || true
```

清理全部内核：

```shell
sudo dkms remove -m aic8800fdrv -v "$(cat code/VERSION)" --all
```
