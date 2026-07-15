# open_wrt_tools — `cwc`

OpenWrt 路由器上的 Wi-Fi 工具：**硬件/监管域探测** 与 **设置国家码 + 锁定单信道**。

交付物是一个 C 二进制 **`cwc`**（Change Wi-Fi Channel），基于 **libuci**，在设备上只读探测或改写 `/etc/config/wireless`。

| 能力 | 命令 |
|------|------|
| 硬件 / 监管域探测（只读） | `cwc -p` / `-p -s` / `-p -v` / `--status` |
| 改国家码 + 锁单信道 | `cwc -c … -i … -n …` |
| 查看现状 / 参考表 | `cwc -l` / `cwc -r` |

**主要目标平台**：Banana Pi **BPI-R4 + BE14**（OpenWrt **25.12.x**，`mediatek/filogic`，mt76）。  
也兼容部分其它 OpenWrt 机型，但 `radioN` 与频段对应关系因设备而异——**改信道前务必先跑** `cwc -p -s`。

交叉编译与 SDK 细节见 **[BUILD.md](BUILD.md)**。

---

## 仓库结构

```
open_wrt_tools/
├── cwc.c                   # 主程序 / CLI
├── probe.c / probe.h       # 硬件 / 监管域只读探测
├── reg.c / reg.h           # 国家码 / 信道 / DFS 规则
├── wifi_uci.c / wifi_uci.h # libuci 读写、iw reg set、wifi reload
├── Makefile                # OpenWrt SDK 交叉编译
├── build.sh                # 一键编译 / 安装
├── build_libuci_staging.sh # 向 SDK staging 安装 libuci
├── BUILD.md                # 构建与部署详解
└── README.md               # 本文件（使用说明）
```

---

## 前置条件

- 路由器已刷 **OpenWrt**，可 **SSH 以 root 登录**
- 运行依赖（25.12 默认镜像通常已有）：`libuci` / `libubox`、`iw`、`wifi`、`/etc/config/wireless`
- 建议安装 `iwinfo`（探测报告更完整）
- **BPI-R4 + BE14**：板载 **SW4 = ON**（Wi-Fi 模块供电）
- 改信道需 root（写 UCI、`wifi reload`，可选 reboot）

### 安装 `iw` / `iwinfo`（大陆可用清华源）

先确认版本：`cat /etc/openwrt_release`。

```sh
cp /etc/opkg/distfeeds.conf /etc/opkg/distfeeds.conf.bak
sed -i 's|https://downloads.openwrt.org|https://mirrors.tuna.tsinghua.edu.cn/openwrt|g' /etc/opkg/distfeeds.conf
opkg update && opkg install iw iwinfo
```

`opkg update` 仍 404 时，检查 `distfeeds.conf` 里的 **版本 / target / 架构** 是否与固件一致。

---

## 快速开始

### 1. 编译（Ubuntu 编译机）

```bash
# SDK 与固件小版本必须一致（示例：25.12.5）
export SDK=~/openwrt-sdk/openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64
./build.sh                 # 或见 BUILD.md 手动 make
file ./cwc                 # 必须是 ARM aarch64，不是 x86-64
```

### 2. 部署到路由器

```bash
# Linux / macOS
scp ./cwc root@192.168.1.1:/usr/local/bin/
ssh root@192.168.1.1 "chmod 755 /usr/local/bin/cwc"

# Windows（OpenWrt Dropbear 通常需 -O）
scp -O .\cwc root@192.168.1.1:/usr/local/bin/
```

也可：`ROUTER=192.168.1.1 ./build.sh install`。

### 3. 推荐流程

```
上传 cwc ──► cwc -p -s（确认 radio / band）──► cwc -c/-i/-n ──► 再跑 -p -s 验证
```

```sh
cwc -p -s
cwc -l
cwc -c CN -i radio1 -n 149 -y --no-reboot
cwc -p -s
```

---

## 硬件探测（`cwc -p`）

```sh
cwc -p              # 完整报告
cwc -p -s           # 一页摘要（推荐）
cwc -p -v           # 附加完整 iw phy 输出
cwc --status        # KEY=VALUE（自动化 / 脚本）
```

报告大致包含：系统与 board、`iw reg get`、各 UCI radio 的 band/country/channel、各 phy 在当前 regdomain 下允许的信道、`iwinfo` 运行时信息，以及 BPI-R4/BE14 相关提示。

**注意**：`radio0` / `radio1` / `radio2` 只是 UCI 段名。老设备常见 `radio0=5g`、`radio1=2g`（与 BPI-R4 相反）——**以探测输出的 band 列为准**。

---

## 改信道（`cwc -c/-i/-n`）

### 行为摘要

- 设置 **ISO 国家码**（可选；省略则按 CN → JP → US → DE 推断）
- 将指定 radio **锁定为单一信道**（`channel=N`，`channels` 列表仅含 N）
- 不支持 `auto` / ACS
- **DFS/CAC**（如 5G 52–64、100–144）会警告并要求确认
- 流程：staged → 确认 → `uci commit` → `wifi reload` → 可选 **reboot**

### 命令行

```sh
cwc -l                                          # 详细状态（只读）
cwc -r                                          # 国家码 × 频段参考表
cwc -h                                          # 帮助

cwc -i radio1 -n 36                             # 国家码自动推断
cwc -c CN -i radio0 -n 6 -y --no-reboot         # 显式 CN，跳过确认，不 reboot
cwc -c JP -i radio1 -n 100 -y                   # DFS 信道（有 CAC 警告）
cwc -c JP -i radio2 -n 37 -y                    # 6G（CN regdb 通常无 6G）
```

### 选项

| 选项 | 说明 |
|------|------|
| `-p, --probe` | 硬件 / 监管域探测（只读） |
| `-s, --summary` | 配合 `-p`：一屏摘要 |
| `-v, --verbose` | 配合 `-p`：附带原始 `iw phy` |
| `--status` | KEY=VALUE 状态（脚本用） |
| `-c, --country CODE` | 国家码（CN、JP、US、DE…）；省略则按信道推断 |
| `-i, --radio NAME` | UCI `wifi-device` 名，如 `radio0` |
| `-n, --channel NUM` | 数字信道（不支持 `auto`） |
| `-y, --yes` | 跳过确认（含 DFS / 6G 警告后的确认） |
| `--no-reboot` | commit + reload 后不 reboot |
| `-l, --list` | 打印详细状态并退出 |
| `-r, --reference` | 国家码参考表 |
| `-h, --help` | 帮助 |

无参数运行时打印帮助；探测用 `-p`，改信道必须同时给出 `-i` 与 `-n`。

### 写入 UCI

```text
wireless.<radio>.country='XX'
wireless.<radio>.channel='N'
wireless.<radio>.channels='N'    # 列表仅一项
```

### BPI-R4 + BE14 典型布局

| Radio | 频段 | 说明 |
|-------|------|------|
| radio0 | 2.4 GHz | |
| radio1 | 5 GHz | |
| radio2 | 6 GHz | CN 监管下通常不可用；测试需 JP/US/DE 等 |

### 中国大陆常用信道（CN）

| 频段 | 可用信道（示例） | 备注 |
|------|------------------|------|
| 2.4G | 1–13 | |
| 5G 低段 | 36–48 | 室内 |
| 5G DFS | 52–64 | DFS + CAC 静默 |
| 5G 高段 | 149–165 | 室内外均可，无 DFS |
| 5G 中段 | 100–144 | **CN 不允许** |
| 5G UNII-4 | 169–177 | **CN 不允许**（主要 US） |
| 6G | — | **CN regdb 一般无 6G Wi-Fi** |

### 国家码推断（省略 `-c`）

按 **CN → JP → US → DE**，选第一个能覆盖所选 band+信道 的国家。

| 示例 | 推断 |
|------|------|
| 5G ch 149 | CN |
| 5G ch 100 | JP |
| 5G ch 169 | US |
| 6G ch 37 | JP 或 DE |

推断失败时请显式加 `-c`。

### DFS / CAC

选择 DFS 信道时：

- 会打印 **WARNING**，需确认后继续（`-y` 也会跳过该确认）
- reload 后 SSID 可能 **静默 1–10 分钟**（CAC），属正常现象

---

## 使用示例

### BPI-R4：5G 锁定 149（国内常用）

```sh
cwc -p -s
cwc -c CN -i radio1 -n 149 -y --no-reboot
cwc -p -s
```

### 老设备（radio 命名可能相反）

```sh
cwc -p -s
# 若 band 显示 radio1=2g，则 2.4G 信道 3：
cwc -c CN -i radio1 -n 3 -y --no-reboot
```

### 6G 测试（非 CN，仅合法测试环境）

```sh
cwc -c JP -i radio2 -n 37 -y
```

---

## 故障排除

| 现象 | 处理 |
|------|------|
| `scp` 报 `sftp-server: not found` | Windows 使用 **`scp -O`** |
| 上传的 `cwc` 无法运行 / 架构不对 | 用 SDK 交叉编译；`file cwc` 须为 **aarch64** |
| `Permission denied` / 非 root | `chmod 755`；以 **root** 运行 |
| `cannot infer country` | 加 **`-c CN`**（或 JP/US/DE） |
| `country CN disallows …` | 选错 radio 或信道；以 `-p -s` 的 **band** 为准 |
| `missing command: iw` | `opkg install iw` |
| `opkg update` 404 | 源与固件版本不一致；改用归档版或清华镜像 |
| 改完 SSID 长时间不见 | DFS **CAC** 进行中 |
| 运行时 `36(42)` | 主信道 36，80 MHz 中心 42，非配置错误 |

```sh
logread | grep -iE 'wifi|mt76|regdom'
uci show wireless | grep -E 'country|channel|band|hwmode'
iw reg get
cwc -p -s
```

---

## 限制与合规

- 监管域在 cfg80211 中 **全局共享**：所有 radio 同一国家码
- **仅在合法地区使用对应国家码**；勿在国内随意设 US/JP 发射
- **拒绝 `auto` 信道**，仅支持显式数字信道
- 确认前取消：UCI **不会** commit 到 flash
- 6G、177 等频段依赖硬件、regdb 与终端支持

---

## 许可证

本项目采用 **[Unlicense](LICENSE)**（公有领域奉献）：可自由复制、修改、发布、商用或闭源再分发，**无需署名、无需开源衍生作品**。软件按「现状」提供，作者不承担担保责任。

详情见 [unlicense.org](https://unlicense.org)。

---

## 相关文档

- **[BUILD.md](BUILD.md)** — OpenWrt SDK 交叉编译、libuci staging、上传与验证
- 固件选择器（BPI-R4）：https://firmware-selector.openwrt.org/?version=25.12.5&target=mediatek%2Ffilogic&id=bananapi_bpi-r4
