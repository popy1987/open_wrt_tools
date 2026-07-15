# cwc 构建与部署指南

**目标平台：Banana Pi BPI-R4 + BE14 · OpenWrt 25.12.5 · Ubuntu 20.04 交叉编译**

C + **libuci** 二进制 **`cwc`**（Change Wi-Fi Channel），整合了 `change_wifi_channel.sh`（改信道）与 `detect_wifi_hw.sh`（硬件探测）。

| 文件 | 说明 |
|------|------|
| `cwc.c` | 主程序、CLI |
| `reg.c` / `reg.h` | 国家码 / 信道 / DFS 规则 |
| `wifi_uci.c` / `wifi_uci.h` | libuci 读写、`iw reg set`、`wifi reload` |
| `probe.c` / `probe.h` | 硬件 / 监管域只读探测 |
| `Makefile` | OpenWrt SDK 交叉编译 |
| `build.sh` | 一键编译（SDK 与脚本同目录） |

**交互菜单** 仍建议用 `change_wifi_channel.sh`；二进制侧重 **自动化 / 少暴露源码**。

---

## 前提与版本锁定

| 项目 | 值 |
|------|-----|
| OpenWrt 稳定版 | **[25.12.5](https://openwrt.org/releases/25.12/start)**（2026-07 当前最新维护版） |
| Target | `mediatek/filogic` |
| 架构 | `aarch64_cortex-a53` |
| 编译机 | Ubuntu 20.04 x86_64 |
| 路由器 | BPI-R4 + BE14（**不必在路由器上编译**） |

固件下载（刷机用，与 SDK 版本一致）：

- 固件选择器：https://firmware-selector.openwrt.org/?version=25.12.5&target=mediatek%2Ffilogic&id=bananapi_bpi-r4
- 镜像目录：https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/
- eMMC sysupgrade 示例：`openwrt-25.12.5-mediatek-filogic-bananapi_bpi-r4-squashfs-sysupgrade.itb`

**SDK 小版本必须与路由器 `DISTRIB_RELEASE` 完全一致**（25.12.4 的 SDK 不要拿去配 25.12.5 固件）。

---

## 流程总览

```
┌──────────────────┐     ┌─────────────────────────┐     ┌──────────────────────┐
│ BPI-R4 路由器     │     │ Ubuntu 20.04 编译机      │     │ BPI-R4 路由器         │
│ Step 0           │     │ Step 1–3                │     │ Step 4–5             │
├──────────────────┤     ├─────────────────────────┤     ├──────────────────────┤
│ cat openwrt_     │ ──► │ apt 装依赖               │     │ scp 上传二进制        │
│   release        │     │ 下载 25.12.5 SDK (.zst)  │ ──► │ cwc  │
│ 确认 25.12.5     │     │ make 交叉编译 aarch64    │     │   -p -s 验证         │
└──────────────────┘     └─────────────────────────┘     └──────────────────────┘
```

---

## Step 0：路由器确认固件（25.12.5）

SSH 登录 BPI-R4：

```sh
cat /etc/openwrt_release
```

期望输出（关键字段）：

```text
DISTRIB_RELEASE='25.12.5'
DISTRIB_TARGET='mediatek/filogic'
DISTRIB_ARCH='aarch64_cortex-a53'
DISTRIB_BOARD='bananapi_bpi-r4'
```

若不是 `25.12.5`，请先 **sysupgrade 到 25.12.5** 再编译二进制，或改用与固件一致的小版本 SDK。

BE14 硬件检查：

```sh
# SW4 拨到 ON（12V 给 Wi-Fi 模块供电）
command -v uci iw wifi && ls /etc/config/wireless
opkg list-installed | grep -E 'iwinfo|kmod-mt7996'   # 可选
```

若 Wi-Fi 发射功率异常，确认 U-Boot 已启用 BE14 overlay（25.12.x 部分批次需要）：

```sh
fw_printenv bootconf_extra
# 应含 mt7988a-bananapi-bpi-r4-wifi-be14；若无：
overlay="mt7988a-bananapi-bpi-r4-wifi-be14"
current="$(fw_printenv -n bootconf_extra 2>/dev/null)"
if [ -n "${current}" ]; then
  fw_setenv bootconf_extra "${current}#${overlay}"
else
  fw_setenv bootconf_extra "${overlay}"
fi
reboot
```

---

## Step 1：准备 Ubuntu 20.04 编译机

### 1.1 安装依赖

25.12.5 SDK 包为 **`.tar.zst`**，需 `zstd`。

**在 SDK 内编译 libuci**（首次必做）还需要 OpenWrt 官方主机依赖，至少包括 **ncurses** 和 **GNU awk**：

```bash
sudo apt update
sudo apt install -y \
  build-essential make wget curl tar xz-utils zstd file ssh \
  gawk libncurses-dev rsync unzip patch python3 python3-distutils \
  git cmake
```

验证（应与 OpenWrt prereq 检查一致）：

```bash
gawk --version | head -1
echo '#include <ncurses.h>' | gcc -E - >/dev/null && echo "ncurses OK"
```

### 1.2 获取源码

将 `openwrt/bpi-r4/` 放到编译机，例如 `~/work/SimpleCli/openwrt/bpi-r4/`。

**从 Windows 开发机拷贝：**

```bash
mkdir -p ~/work
scp -r user@windows-host:/path/to/SimpleCli/openwrt/bpi-r4 ~/work/
```

**确认文件齐全：**

```bash
cd ~/work/SimpleCli/openwrt/bpi-r4
ls cwc.c probe.c reg.c wifi_uci.c Makefile
```

### 1.3 环境变量（建议写入 `~/.bashrc`）

```bash
export WORK=~/work/SimpleCli/openwrt/bpi-r4
export SDK=~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
export ROUTER=192.168.1.1          # 改成 BPI-R4 的 IP
export STAGING=$SDK/staging_dir/target-aarch64_cortex-a53
```

---

## Step 2：下载 OpenWrt 25.12.5 SDK

SDK 目录：

https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/

文件名（2026-06 官方发布）：

```text
openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst
```

下载并解压：

```bash
mkdir -p ~/openwrt-sdk
cd ~/openwrt-sdk

wget https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst

tar --zstd -xf openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst
```

**推荐布局**（SDK 与 `build.sh` 同级，便于 `./build.sh`）：

```text
openwrt/bpi-r4/
  build.sh
  Makefile  cwc.c  ...
  openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/
```

也可把 SDK 放在其它路径，用 `make STAGING_DIR=...` 手动编译（见 Step 3）。

验证 SDK（手动编译时）：

```bash
export SDK=~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
export STAGING=$SDK/staging_dir/target-aarch64_cortex-a53

ls "$SDK/staging_dir/toolchain-"*/bin/*openwrt*gcc | head -1
# 期望: .../aarch64-openwrt-linux-musl-gcc
```

**注意：** 刚解压的 SDK **默认没有** `uci.h`。25.12.5 SDK 里 **没有** `package/system/uci/`，需先更新 feeds，或用 cmake 脚本直接编 libuci。

**方式 A — OpenWrt SDK 包系统（需联网拉 feeds）：**

```bash
cd "$SDK"
make defconfig
./scripts/feeds update -a
make package/feeds/base/uci/compile V=s

ls "$STAGING/usr/include/uci.h"
ls "$STAGING/usr/lib/libuci.so"*
```

若报 `No rule to make target 'package/system/uci/compile'`，说明路径不对，用上面 `package/feeds/base/uci/compile`。

**方式 B — cmake 直接编 json-c + libubox + libuci（推荐，不依赖 feeds）：**

依赖链：`libubox` → 需要 `json-c`；`libuci` → 需要 `libubox`。

```bash
sudo apt install -y git cmake gawk libncurses-dev pkg-config

cd ~/work/SimpleCli/openwrt/bpi-r4
chmod +x build_libuci_staging.sh
./build_libuci_staging.sh "$SDK"
```

脚本会依次安装到 `staging_dir/.../usr/include` 和 `usr/lib`，并使用 SDK 自带的 `staging_dir/host/bin/pkg-config` 做交叉查找。

**方式 C — 一键 `./build.sh`：** 先尝试方式 A，失败则自动走方式 B。

---

## Step 3：交叉编译

**快捷方式**（SDK 已放在 `openwrt/bpi-r4/` 下与 `build.sh` 同级）：

```bash
cd openwrt/bpi-r4
chmod +x build.sh
./build.sh
# 部署: ROUTER=192.168.1.1 ./build.sh install
```

**手动方式**：

```bash
cd "$WORK"

make STAGING_DIR=$STAGING clean all
```

成功时 Makefile 会打印：

```text
Cross CC=.../aarch64-openwrt-linux-musl-gcc
```

产物：`./cwc`（已 strip，约几十 KB）。

### 编译后自检（仍在 Ubuntu 上）

```bash
file ./cwc
# 期望: ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV), dynamically linked, ...

readelf -h ./cwc | grep Machine
# 期望: Machine: AArch64
```

若显示 `x86-64`，说明 **未走交叉工具链**（`STAGING_DIR` 未设置或路径错误），**不要上传**。

---

## Step 4：上传到 BPI-R4

```bash
cd "$WORK"

ssh root@$ROUTER "mkdir -p /usr/local/bin"
scp ./cwc root@$ROUTER:/usr/local/bin/
ssh root@$ROUTER "chmod 755 /usr/local/bin/cwc"
```

Ubuntu 的 `scp` **不需要** Windows Dropbear 下的 `-O` 参数。

---

## Step 5：路由器上验证

```sh
cwc -h
cwc -p -s          # 硬件探测摘要（原 detect_wifi_hw.sh -s）
cwc --status       # KEY=VALUE（脚本/自动化）
cwc -l             # 改信道前状态
cwc -r             # 国家码 / 信道参考表
```

改信道示例（**先 `-p -s` 确认 radio 与 band**）：

```sh
cwc -c CN -i radio1 -n 149 -y --no-reboot
cwc -p -s
```

完整探测（含 iw phy 详情）：

```sh
cwc -p -v
```

可选：安装 `iwinfo` 以显示更多运行时信息：

```sh
opkg update && opkg install iwinfo
```

---

## 路由器运行依赖

OpenWrt 25.12.5 默认镜像已包含，**无需在路由器上安装 gcc/make**：

| 依赖 | 用途 |
|------|------|
| `libuci` / `libubox` | 读写 `/etc/config/wireless` |
| `iw` | 监管域、phy 信息 |
| `wifi` | `wifi reload` |
| `kmod-mt7996*` | BE14 Wi-Fi 驱动（镜像自带） |
| `/etc/config/wireless` | UCI 无线配置 |

可选：`iwinfo`（探测报告更完整）。

---

## 常见问题

### SDK 下载 404 或文件名变化

- 打开 https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/ 查看当前 `openwrt-sdk-*.tar.zst` 全名。
- GCC 版本号可能随维护版更新（如 `gcc-14.3.0`），以目录实际文件为准。

### `tar: Unrecognized archive format`

- 25.12.5 SDK 是 **zst** 不是 xz：`tar --zstd -xf ...`，或 `zstd -d ... && tar xf ...`。

### `Cannot find toolchain under .../toolchain-*`

- `SDK` 必须指向解压后的 **SDK 根目录**（含 `staging_dir/`），不是 `staging_dir` 本身。

### `No rule to make target 'package/system/uci/compile'`

- 25.12.5 SDK **不包含** `package/system/uci/` 源码目录；uci 在 **feeds** 里。
- 那些 `firmware does not exist` 警告也是 feeds 未更新时的正常现象。
- 正确流程：

```bash
cd ~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
make defconfig
./scripts/feeds update -a
make package/feeds/base/uci/compile V=s
```

- 若 feeds 仍有问题，用 cmake 备选（不依赖 OpenWrt 包系统）：

```bash
cd ~/work/SimpleCli/openwrt/bpi-r4
./build_libuci_staging.sh ~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
```

### `missing .../usr/include/uci.h`

- **正常现象**：OpenWrt SDK 压缩包只带交叉工具链，**不带** libuci 开发头文件。
- `uci.h` 在 SDK 内执行 `make package/system/uci/compile` 后才会安装到 `staging_dir/.../usr/include/`。
- 用 `./build.sh` 会在首次编译时自动构建 libuci；或手动：

```bash
cd ~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
make package/system/uci/compile V=s
```

### `ncurses.h` / `GNU awk` / `Prerequisite check failed`

SDK 内任何 `make` 都会做主机依赖检查。Ubuntu 20.04 缺包时常见：

```text
Checking 'ncurses.h'... failed.
Checking 'awk'... failed.
Build dependency: Please install ncurses
Build dependency: Please install GNU 'awk'
```

安装后重试：

```bash
sudo apt install -y gawk libncurses-dev
```

### `pkg-config.real: not found` / libubox 找不到 json-c

- OpenWrt SDK 的 `staging_dir/host/bin/pkg-config` 是包装脚本，常指向不存在的 `/bin/pkg-config.real`。
- 新版 `build_libuci_staging.sh` 会改用 `pkg-config.real` 或宿主机 `pkg-config` + `PKG_CONFIG_SYSROOT_DIR`，并对 libubox 打补丁跳过 `PKG_SEARCH_MODULE`。
- 确保已安装：`sudo apt install -y pkg-config`

### `No feed for package 'libuci' found`

- 通常是因为 **未先** `./scripts/feeds update`，或误用了 `feeds install libuci`。
- **libuci 不需要走 feeds**：SDK 已自带 `package/system/uci/`，直接 `make package/system/uci/compile V=s` 即可。

### `cannot find -luci`

- `STAGING_DIR` 必须是 `$SDK/staging_dir/target-aarch64_cortex-a53`（含 `target-` 前缀）。

### 路由器 `Exec format error`

- 架构不匹配：BPI-R4 必须编 **aarch64**，不要用 arm 或其他 target 的二进制。

### 固件 25.12.4，能否用 25.12.5 SDK 编？

- **不建议**。请固件与 SDK 小版本对齐；同系列内用 sysupgrade 升到 25.12.5 后再编。

### 与 Shell 版并存

| 能力 | `.sh` | C 二进制 |
|------|-------|----------|
| `-c/-i/-n/-y` 改信道 | ✓ | ✓ |
| `-l` / `-r` | ✓ | ✓ |
| 硬件探测 `-p/-s/-v` | `detect_wifi_hw.sh` | ✓ |
| 交互菜单 | ✓ | 用 `.sh` |

推荐路径：

```text
/usr/local/bin/cwc                      # C 二进制（短命令名）
/usr/local/bin/change_wifi_channel.sh   # Shell 交互（可选）
/usr/local/bin/detect_wifi_hw.sh        # Shell 探测（可选，已被 -p 取代）
```

---

## 一键命令备忘（复制即用）

将 `ROUTER` 和 `WORK` 路径按实际情况修改后，在 **Ubuntu 20.04** 上整段执行：

```bash
# --- 环境 ---
export WORK=~/work/SimpleCli/openwrt/bpi-r4
export ROUTER=192.168.1.1
export SDK=~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
export STAGING=$SDK/staging_dir/target-aarch64_cortex-a53

# --- 首次：装依赖 + 下 SDK（已下载可跳过）---
sudo apt install -y build-essential make wget zstd file ssh
mkdir -p ~/openwrt-sdk && cd ~/openwrt-sdk
wget -nc https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst
tar --zstd -xf openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64.tar.zst

# --- 首次：staging 安装 libuci（任选其一）---
# A) feeds 方式
cd "$SDK" && make defconfig && ./scripts/feeds update -a
make package/feeds/base/uci/compile V=s
# B) 或 cmake 备选: ./build_libuci_staging.sh "$SDK"
cd -

# --- 编译 ---
cd "$WORK"
make STAGING_DIR=$STAGING clean all
file ./cwc

# --- 部署 + 验证 ---
scp ./cwc root@$ROUTER:/usr/local/bin/
ssh root@$ROUTER "chmod 755 /usr/local/bin/cwc && cwc -p -s"
```

---

## 附录：其他 OpenWrt 设备

本仓库 Makefile 默认链接路径针对 **mediatek/filogic**。若需为其他路由器编译（如 ipq806x arm），须：

1. 下载对应版本 / target 的 SDK；
2. 使用匹配的 `STAGING_DIR`（如 `target-arm_cortex-a15_neon-vfpv4`）；
3. 若链接报 `root-mediatek/lib` 错误，修改 `Makefile` 中 `-L$(STAGING_DIR)/root-*/lib` 为对应平台目录。

BPI-R4 + BE14 长期使用请 stick to **OpenWrt 25.12.5 + mediatek/filogic SDK**。
