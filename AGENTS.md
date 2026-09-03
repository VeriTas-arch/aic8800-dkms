# Repository agent instructions

## Scope and supported target

- These instructions apply to the entire repository.
- The maintained driver source is under
  `code/src/AIC8800/drivers/aic8800`.
- The currently supported and validated target is USB on Ubuntu 22.04.5 LTS
  with the HWE Linux 6.8.x kernel, amd64.
- The retained SDIO source is upstream reference code. Do not modify it or
  claim that it is supported or buildable unless the user explicitly asks for
  SDIO work.
- Treat `archive/` as read-only provenance. Never build, install, or copy
  driver sources from it.
- Preserve automatic AP selection and roaming. Do not pin a BSSID as a driver
  workaround unless the user explicitly requests that policy change.

## Worktree and system safety

- Preserve unrelated tracked changes and untracked files. Stage only files
  that belong to the current task.
- Prefer compile-only verification. Do not install or remove DKMS versions,
  load or unload modules, trigger udev, reset a USB device, or reboot unless
  the user explicitly authorizes the operation.
- In particular, do not run `dkms-local-install.sh`,
  `dkms-refresh-all-kernels.sh`, or `dkms-clean-old-versions.sh` merely as a
  development check.
- Never use direct USB unbind/bind as a diagnostic shortcut; it can interrupt
  the active network connection.
- Do not bump the driver version or create a commit unless requested.
- Runtime logs can contain network identifiers and other host details. Review
  collected logs before sharing them outside the machine.

## Development checks

Run commands from the repository root. Start with the inexpensive checks:

```bash
bash -n code/scripts/*.sh
bash code/scripts/test-sync-version.sh
./code/scripts/sync-version.sh --check
git diff --check
```

Build the default USB configuration without installing it:

```bash
./code/scripts/build-test.sh
```

An installed kernel with headers can be selected explicitly. Strict warning,
configuration override, and optional sparse examples are:

```bash
./code/scripts/build-test.sh 6.8.0-xx-generic
./code/scripts/build-test.sh --warnings 6.8.0-xx-generic
./code/scripts/build-test.sh --extra-warnings 6.8.0-xx-generic
./code/scripts/build-test.sh --config CONFIG_USB_RX_AGGR=y 6.8.0-xx-generic
./code/scripts/build-test.sh --sparse 6.8.0-xx-generic
```

`--warnings` uses Kbuild `W=e`, so ordinary compiler warnings are fatal.
`--extra-warnings` uses advisory `W=1`; do not present it as a fatal warning
gate or hide its findings with suppression flags.

Use the repository-wide compile-only check before handing off driver changes:

```bash
./code/scripts/check.sh
```

It checks shell syntax, version consistency, version-script tests, patch
hygiene, the default build for every installed kernel with headers, the strict
latest-kernel build, and these optional USB compile paths:

- `CONFIG_USB_RX_AGGR=y`
- `CONFIG_RX_TASKLET=y`
- `CONFIG_TX_TASKLET=y`
- `CONFIG_RX_TASKLET=y CONFIG_TX_TASKLET=y`
- `CONFIG_USB_TX_AGGR=y`
- `CONFIG_PREALLOC_RX_SKB=y`
- `CONFIG_USB_MSG_IN_EP=n`

`shellcheck` and `sparse` are optional and are skipped when unavailable. A
compiler executable-name mismatch is acceptable when the compiler version is
identical to the kernel build compiler. Skipped BTF generation is also
acceptable when the target kernel has no usable `vmlinux`. Do not hide newly
introduced compiler warnings with additional warning-suppression flags.

Use `./code/scripts/verify-firmware.sh` to verify both checksums and exact
manifest coverage. Compile and DKMS scripts stage sources through
`code/scripts/lib/common.sh`; generated Kbuild files are excluded from the
staged copy, never deleted from the working tree.

## Version management

`code/VERSION` is the canonical version source and must contain a strict
`MAJOR.MINOR.PATCH` value with no leading zeroes. Do not edit these generated
mirrors by hand:

- `code/src/AIC8800/drivers/aic8800/dkms.conf`
- `code/src/DEBIAN/control`
- `code/src/AIC8800/drivers/aic8800/aic_dkms_version.h`

Use the synchronization script instead:

```bash
./code/scripts/sync-version.sh 1.1.10
./code/scripts/sync-version.sh --check
```

Setting the current version is a no-op. A downgrade or inconsistent mirror
state is rejected unless `--force` is supplied:

```bash
./code/scripts/sync-version.sh --force 1.1.8
```

Use `--force` only after identifying why the downgrade or drift is intended;
it does not bypass malformed version validation. A documentation-only or
script-only change does not require a version bump unless the user asks for
one.

## DKMS automatic-rebuild regression

This is a system-changing test and requires explicit authorization. Choose an
installed, non-running kernel that has headers; never invent or assume the
target kernel version:

```bash
uname -r
ls -1 /lib/modules | sort
ls -1 /usr/src | grep -E '^linux-headers-' | sort
```

After setting `TARGET` to the verified non-running kernel, test DKMS and inspect
the installed modules:

```bash
sudo dkms autoinstall -k "$TARGET"
dkms status | grep aic8800fdrv
modinfo -k "$TARGET" -n aic8800_fdrv
modinfo -k "$TARGET" -n aic_load_fw
modinfo -k "$TARGET" -F vermagic aic8800_fdrv
modinfo -k "$TARGET" -F dkms_version aic8800_fdrv
```

The module paths should resolve under `updates/dkms`, and `dkms_version` should
match `code/VERSION`. If the same version is already installed and a deliberate
reinstall is required, use the explicit uninstall/install flow only with the
same system-change authorization:

```bash
VER="$(<code/VERSION)"
sudo dkms uninstall -m aic8800fdrv -v "$VER" -k "$TARGET" || true
sudo dkms install -m aic8800fdrv -v "$VER" -k "$TARGET" --force
```
