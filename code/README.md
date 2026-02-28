# AIC8800FDRVPACKAGE

Mercury UX3H(免驱版)(一点也不免驱) AX300 Ubuntu24.04 Linux kernel 6.8.0 amd64 驱动
基于 [水星官方驱动](https://service.mercurycom.com.cn/download-2596.html) 修改

## deb 版：<https://github.com/bk1d/aic8800fdrvpackage/releases>

## 自己编译

### Step 1

```shell
sudo apt update
sudo apt install build-essential
```

### Step 2

```shell
dpkg-deb -b src aic8800fdrvpackage_linux_6.8_amd64.deb
```

### Step 3

```shell
sudo dpkg -i aic8800fdrvpackage_linux_6.8_amd64.deb
```

## 本机 DKMS 接入（推荐）

该方式用于“内核升级后自动重编译驱动”，不依赖重新手动安装驱动包。

### Step 1: 安装依赖

```shell
sudo apt update
sudo apt install dkms build-essential linux-headers-$(uname -r)
```

### Step 2: 运行本机接入脚本

```shell
chmod +x scripts/dkms-local-install.sh
./scripts/dkms-local-install.sh
```

可选：指定目标内核版本

```shell
./scripts/dkms-local-install.sh 6.8.0-xx-generic
```

可选：一键刷新所有已安装 headers 的内核

```shell
chmod +x scripts/dkms-refresh-all-kernels.sh
./scripts/dkms-refresh-all-kernels.sh
```

可选：清理旧版本驱动

```shell
chmod +x scripts/dkms-clean-old-versions.sh
./scripts/dkms-clean-old-versions.sh
```

### Step 3: 验证

```shell
dkms status | grep aic8800fdrv
sudo modprobe aic_load_fw
sudo modprobe aic8800_fdrv
lsmod | grep -E "aic_load_fw|aic8800_fdrv"
```

## 版本维护

- 驱动版本统一维护在仓库根目录 `VERSION`。
- 本机 DKMS 脚本会读取 `VERSION`，并在复制到 `/usr/src/aic8800fdrv-<version>/` 后自动同步 `dkms.conf` 中的 `PACKAGE_VERSION`。

## 自动重建模拟验证（标准回归用例）

用于在“不切换当前运行内核”的前提下，模拟“升级到新内核后 DKMS 自动重建”。

### Step 1: 选择一个非当前内核且有 headers 的目标内核

```shell
uname -r
ls -1 /lib/modules | sort
ls -1 /usr/src | grep -E '^linux-headers-' | sort
```

示例目标内核：`6.8.0-90-generic`

### Step 2: 触发自动重建模拟

```shell
TARGET=6.8.0-90-generic
sudo dkms autoinstall -k "$TARGET"
dkms status | grep aic8800fdrv
```

### Step 3: 若提示同版本已存在，强制安装 DKMS 产物

```shell
TARGET=6.8.0-90-generic
sudo dkms uninstall -m aic8800fdrv -v "$(cat VERSION)" -k "$TARGET" || true
sudo dkms install -m aic8800fdrv -v "$(cat VERSION)" -k "$TARGET" --force
```

### Step 4: 验证目标内核使用的是 updates/dkms 模块

```shell
TARGET=6.8.0-90-generic
modinfo -k "$TARGET" aic8800_fdrv | grep '^filename'
modinfo -k "$TARGET" aic_load_fw | grep '^filename'
```

期望输出路径包含：`/lib/modules/<target>/updates/dkms/`

### Step 5: 回归通过判定

- `dkms status` 中出现 `aic8800fdrv/<version>, <target-kernel>, x86_64: installed`
- `modinfo -k <target-kernel>` 显示两个模块都来自 `updates/dkms`

### Step 6: 清理回滚（回归后恢复环境）

仅删除某个目标内核上的 DKMS 安装：

```shell
TARGET=6.8.0-90-generic
VER="$(cat VERSION)"
sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
sudo dkms remove -m aic8800fdrv -v "$VER" -k "$TARGET" || true
```

验证清理结果：

```shell
TARGET=6.8.0-90-generic
dkms status | grep aic8800fdrv || true
modinfo -k "$TARGET" aic8800_fdrv 2>/dev/null || echo "aic8800_fdrv not found for $TARGET"
modinfo -k "$TARGET" aic_load_fw 2>/dev/null || echo "aic_load_fw not found for $TARGET"
```

如果要连同当前内核一起全部清理：

```shell
sudo dkms remove -m aic8800fdrv -v "$(cat VERSION)" --all
```
