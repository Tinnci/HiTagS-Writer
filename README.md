# HiTagS Writer

**Flipper Zero** 外部应用 (FAP)，用于读取、写入、转储和克隆 **HiTag S 8268 系列魔术卡**。

## 功能

| 功能 | 说明 |
|------|------|
| **写入 EM4100 ID** | 手动输入 5 字节 EM4100 ID，编码并写入配置页 + 数据页 |
| **从文件加载** | 浏览 `.rfid` 文件，加载 EM4100 协议数据并写入 |
| **读取标签数据** | 读取配置页与数据页，解码显示 EM4100 卡号 |
| **读取标签 UID** | 读取并显示 HiTag S 标签的 32 位 UID |
| **写入标签 UID** | 修改标签 UID（8268 魔术卡特有功能，写入 Page 0）|
| **全量转储 (Full Dump)** | 读取所有页面，显示摘要，支持保存为 `.hts` 文件或串口输出 |
| **加载并克隆** | 从 `.hts` 转储文件恢复完整标签数据（UID + 配置 + 数据页）|
| **擦除标签 (Wipe)** | 清空所有数据页，重置配置和密码为出厂默认值 |
| **关于** | 应用信息与支持的芯片型号 |

### 8268 高级特性

- **多密码认证**: 自动尝试默认密码 `0xBBDD3399` 和备选密码 `0x4D494B52`
- **写入校验**: 每页写入后自动回读验证（配置页 PWDH0 字节掩码处理）
- **页锁检测**: 根据 CON2 的 LCK 位检测锁定页面，跳过不可写页面
- **安全写入顺序**: 先写数据页，再写配置页，最后写 UID（避免配置变更锁定后续操作）

## 支持的芯片

- **ID-F8268** / F8278 / F8310 / K8678
- 默认密码: `0xBBDD3399`，备选: `0x4D494B52` ("MIKR")
- 兼容 HiTag S256 / S2048 协议
- MEMT 字段自动识别存储容量（8 / 64 页）

## .hts 转储文件格式

```
Filetype: HiTag S 8268 Dump
Version: 1
UID: XX XX XX XX
Max Page: N
# MEMT=x auth=x LKP=x ...
Page 0: XX XX XX XX
Page 1: XX XX XX XX
...
```

文件保存在 `/ext/lfrfid/HiTagS_XXXXXXXX.hts`（以 UID 命名）。

## 技术原理

### 写入流程

```
1. 上电等待 (2500µs @ 125kHz)
2. UID 请求 (UID_REQ_ADV1, 5 bits BPLM)
3. 接收 UID 响应 (Manchester MC4K 解码, 半周期跟踪算法)
4. SELECT (5bit cmd + 32bit UID + 8bit CRC = 45 bits)
5. 8268 认证: WRITE_PAGE(page 64) → 密码+CRC(40 bits)
6. 写入数据页 (Page 4, 5) → 写入配置页 (Page 1) → 写入 UID (Page 0)
7. 每页写入后回读校验
```

### 协议层

- **Reader → Tag**: Binary Pulse Length Modulation (BPLM)
  - 载波频率: 125 kHz
  - Gap (T_LOW): 8 载波周期 (64µs)
  - Bit 0: 20 载波周期 (160µs)
  - Bit 1: 28 载波周期 (224µs)
- **Tag → Reader**: Manchester 编码 (MC4K, 4 kbit/s)
  - 半周期: 16 载波周期 (128µs)
  - 使用半周期跟踪解码器（非 Flipper 内置 `manchester_advance`）
- **CRC**: CRC-8, 多项式 0x1D, 初始值 0xFF
- **编程等待 (T_PROG)**: 6000µs（写入后等待 EEPROM 编程完成）

### 配置页 (Page 1) 结构

```
Byte 0 (CON0): MEMT[1:0] RES0 RES3 ... (存储类型 + 82xx TTF 标志)
Byte 1 (CON1): auth TTFC TTFDR[1:0] TTFM[1:0] LCON LKP (认证 + 锁定)
Byte 2 (CON2): LCK7..LCK0 (各页组锁定位)
Byte 3 (PWDH0): 密码高字节 (明文模式读回 0xFF)
```

### EM4100 编码

40-bit 卡号编码为 64-bit Manchester 帧:
- 9 个 header bits (111111111)
- 10 行 × (4 data + 1 parity) bits
- 4 column parity bits + 1 stop bit

## 构建

### 环境要求

- [Pixi](https://pixi.sh/) 包管理器
- Flipper Zero 固件 SDK (由 ufbt 自动下载)

### 快速开始

```bash
# 安装依赖
pixi install

# 安装 ufbt
pixi run install-ufbt

# 编译
pixi run build

# 连接 Flipper Zero 并部署运行
pixi run launch
```

### 命令一览

| 命令 | 说明 |
|------|------|
| `pixi run build` | 编译 FAP |
| `pixi run launch` | 编译并部署到 Flipper Zero |
| `pixi run clean` | 清理构建产物 |
| `pixi run lint` | 代码风格检查 |

## 项目结构

```
HiTagS Writer/
├── application.fam            # FAP 清单文件
├── hitags_writer_main.c       # 主入口 + Worker 线程 + ViewDispatcher
├── hitags_writer_i.h          # 内部头文件 + App 结构体
├── hitag_s_proto.c/h          # HiTag S 协议层 (BPLM TX / MC4K RX / CRC-8 / 8268 auth)
├── em4100_encode.c/h          # EM4100 编码器 (40bit → 64bit Manchester)
├── scenes/
│   ├── hitags_writer_scene_config.h    # 场景声明 (X-Macro)
│   ├── hitags_writer_scene.c/h        # 场景处理器数组
│   ├── *_scene_start.c                # 主菜单 (9 个入口)
│   ├── *_scene_input_id.c            # EM4100 ID 字节输入
│   ├── *_scene_select_file.c         # .rfid 文件浏览器
│   ├── *_scene_write_confirm.c       # 写入确认对话框
│   ├── *_scene_write.c               # 执行写入 (Worker)
│   ├── *_scene_write_success.c       # 写入成功
│   ├── *_scene_write_fail.c          # 写入失败 + 重试
│   ├── *_scene_read_tag.c            # 读取标签数据
│   ├── *_scene_read_uid.c            # 读取 UID
│   ├── *_scene_write_uid.c           # 写入 UID (ByteInput)
│   ├── *_scene_full_dump.c           # 全量转储 + 保存 .hts
│   ├── *_scene_load_dump.c           # 加载 .hts + 克隆写入
│   ├── *_scene_wipe_tag.c           # 擦除标签 (Wipe)
│   └── *_scene_about.c              # 关于
├── images/                    # 图标资源
├── hitags_writer.png          # 应用图标 (10x10, 1-bit)
├── sim_manchester.py          # Manchester 解码器 Python 仿真测试
└── pixi.toml                 # Pixi 环境配置
```

## 参考资料

- [Proxmark3 RRG - hitagS.c](https://github.com/RfidResearchGroup/proxmark3) — 8268 底层 RF 交互 + 认证协议
- [T5577 Multiwriter](https://github.com/Leptopt1los/t5577_multiwriter) — FAP 架构参考
- [Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware) — SDK + Manchester 解码器
- [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) — 构建工具

## 许可证

MIT License

## 作者

Tinnci
