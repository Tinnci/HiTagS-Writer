# HiTagS Writer

**Flipper Zero** 外部应用 (FAP)，用于向 **HiTag S 8268 系列魔术卡** 写入 EM4100 卡数据。

## 功能

| 功能 | 说明 |
|------|------|
| **写入 EM4100 ID** | 手动输入 5 字节 EM4100 ID，写入到 8268 芯片 |
| **从文件加载** | 浏览 Flipper 上的 `.rfid` 文件，加载 EM4100 协议数据并写入 |
| **读取标签 UID** | 读取并显示 HiTag S 标签的 32 位 UID |
| **关于** | 应用信息与支持的芯片型号 |

## 支持的芯片

- **ID-F8268** / F8278 / F8310 / K8678
- 默认密码: `0xBBDD3399`
- 兼容 HiTag S2048 协议

## 技术原理

### 写入流程

```
1. 上电等待 (2500µs @ 125kHz)
2. UID 请求 (UID_REQ_ADV1, 5 bits BPLM)
3. SELECT (5bit cmd + 32bit UID + 8bit CRC = 45 bits)
4. 认证: WRITE_PAGE → page 64 → 密码 0xBBDD3399
5. 写入配置页 (Page 1) + 数据页 (Page 4, 5)
```

### 协议层

- **Reader → Tag**: Binary Pulse Length Modulation (BPLM)
  - 载波频率: 125 kHz
  - Gap (T_LOW): 48µs, Bit 0: 144µs, Bit 1: 224µs
- **Tag → Reader**: Manchester 编码 (MC4K, 4 kbit/s)
- **CRC**: CRC-8, 多项式 0x1D, 初始值 0xFF

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
├── hitags_writer_main.c       # 主入口 + ViewDispatcher
├── hitags_writer_i.h          # 内部头文件 + App 结构体
├── hitag_s_proto.c/h          # HiTag S 协议层 (BPLM TX / Manchester RX / CRC-8)
├── em4100_encode.c/h          # EM4100 编码器 (40bit → 64bit Manchester)
├── scenes/
│   ├── hitags_writer_scene_config.h    # 场景声明 (X-Macro)
│   ├── hitags_writer_scene.c/h        # 场景处理器数组
│   ├── *_scene_start.c                # 主菜单
│   ├── *_scene_input_id.c            # EM4100 ID 字节输入
│   ├── *_scene_select_file.c         # .rfid 文件浏览器
│   ├── *_scene_write.c               # 执行写入
│   ├── *_scene_write_success.c       # 写入成功
│   ├── *_scene_write_fail.c          # 写入失败 + 重试
│   ├── *_scene_read_uid.c            # 读取 UID
│   └── *_scene_about.c              # 关于
├── images/                    # 图标资源
├── hitags_writer.png          # 应用图标 (10x10, 1-bit)
└── pixi.toml                 # Pixi 环境配置
```

## 参考资料

- [Proxmark3 - hitagS.c](https://github.com/RfidResearchGroup/proxmark3) — 8268 底层 RF 交互
- [T5577 Multiwriter](https://github.com/Leptopt1los/t5577_multiwriter) — FAP 架构参考
- [Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware) — SDK 文档
- [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) — 构建工具

## 许可证

MIT License

## 作者

Tinnci
