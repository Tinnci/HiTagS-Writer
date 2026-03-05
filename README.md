# HiTagS Writer

**Flipper Zero** external application (FAP) for reading, writing, dumping, and cloning **HiTag S 8268 series magic chips**.

## Features

| Feature | Description |
|---------|-------------|
| **Write EM4100 ID** | Manually input a 5-byte EM4100 ID, encode and write to config + data pages |
| **Load from File** | Browse `.rfid` files, load EM4100 protocol data and write to tag |
| **Read Tag Data** | Read config and data pages, decode and display EM4100 card number |
| **Read Tag UID** | Read and display the 32-bit UID of a HiTag S tag |
| **Write Tag UID** | Modify the tag UID (8268 magic chip exclusive — writes Page 0) |
| **Full Dump** | Read all pages, display summary, save as `.hts` file or log to serial |
| **Load & Clone** | Restore full tag data from `.hts` dump file (UID + config + data pages) |
| **Wipe Tag** | Clear all data pages, reset config and password to factory defaults |
| **About** | App information and supported chip models |

### 8268 Advanced Features

- **Multi-password authentication**: Auto-tries 5 default passwords in sequence:
  `0xBBDD3399` (standard), `0x4D494B52` ("MIKR"), `0xAAAAAAAA`, `0x00000000`, `0xFFFFFFFF`
- **Write verification**: Auto read-back after each page write (config page PWDH0 byte masked)
- **Page lock detection**: Checks CON2 LCK bits for locked pages, skips non-writable pages
- **Safe write order**: Data pages → config page → UID (prevents config changes from locking writes)

## Supported Chips

- **ID-F8268** / F8278 / F8310 / K8678
- Default password: `0xBBDD3399`, alternates: `0x4D494B52` ("MIKR"), `0xAAAAAAAA`, `0x00000000`, `0xFFFFFFFF`
- Compatible with HiTag S256 / S2048 protocol
- MEMT field auto-detection for memory capacity (8 / 64 pages)

## .hts Dump File Format

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

Files are saved to `/ext/lfrfid/HiTagS_XXXXXXXX.hts` (named by UID).

## Technical Details

### Write Sequence

```
1. Power-up wait (2500µs @ 125kHz carrier)
2. UID request (UID_REQ_ADV1, 5 bits BPLM)
3. Receive UID response (Manchester MC4K decode, half-period tracking)
4. SELECT (5-bit cmd + 32-bit UID + 8-bit CRC = 45 bits)
5. 8268 auth: WRITE_PAGE(page 64) → Password+CRC (40 bits)
6. Write data pages (Page 4, 5) → config page (Page 1) → UID (Page 0)
7. Read-back verification after each page write
```

### Protocol Layer

- **Reader → Tag**: Binary Pulse Length Modulation (BPLM)
  - Carrier frequency: 125 kHz
  - Gap (T_LOW): 8 carrier cycles (64µs)
  - Bit 0: 20 carrier cycles (160µs)
  - Bit 1: 28 carrier cycles (224µs)
- **Tag → Reader**: Manchester encoding (MC4K, 4 kbit/s)
  - Half-period: 16 carrier cycles (128µs)
  - Custom half-period tracking decoder (not Flipper's built-in `manchester_advance`)
- **CRC**: CRC-8, polynomial 0x1D, initial value 0xFF
- **Programming delay (T_PROG)**: 6000µs (EEPROM programming time after write)

### Config Page (Page 1) Layout

```
Byte 0 (CON0): MEMT[1:0] RES0 RES3 ... (memory type + 82xx TTF flags)
Byte 1 (CON1): auth TTFC TTFDR[1:0] TTFM[1:0] LCON LKP (auth + lock)
Byte 2 (CON2): LCK7..LCK0 (per-group lock bits)
Byte 3 (PWDH0): Password high byte (reads back as 0xFF in plain mode)
```

### EM4100 Encoding

40-bit card number encoded as 64-bit Manchester frame:
- 9 header bits (111111111)
- 10 rows × (4 data + 1 parity) bits
- 4 column parity bits + 1 stop bit

## Building

### Prerequisites

- [Pixi](https://pixi.sh/) package manager
- Flipper Zero firmware SDK (auto-downloaded by ufbt)

### Quick Start

```bash
# Install dependencies
pixi install

# Install ufbt
pixi run install-ufbt

# Build
pixi run build

# Connect Flipper Zero and deploy
pixi run launch
```

### Commands

| Command | Description |
|---------|-------------|
| `pixi run build` | Build FAP |
| `pixi run launch` | Build and deploy to Flipper Zero |
| `pixi run clean` | Clean build artifacts |
| `pixi run lint` | Code style check |

## Project Structure

```
HiTagS Writer/
├── application.fam            # FAP manifest
├── hitags_writer_main.c       # Main entry + Worker thread + ViewDispatcher
├── hitags_writer_i.h          # Internal header + App struct
├── hitag_s_proto.c/h          # HiTag S protocol (BPLM TX / MC4K RX / CRC-8 / 8268 auth)
├── em4100_encode.c/h          # EM4100 encoder (40-bit → 64-bit Manchester)
├── scenes/
│   ├── hitags_writer_scene_config.h    # Scene declarations (X-Macro)
│   ├── hitags_writer_scene.c/h        # Scene handler arrays
│   ├── *_scene_start.c                # Main menu (9 entries)
│   ├── *_scene_input_id.c            # EM4100 ID byte input
│   ├── *_scene_select_file.c         # .rfid file browser
│   ├── *_scene_write_confirm.c       # Write confirmation dialog
│   ├── *_scene_write.c               # Execute write (Worker)
│   ├── *_scene_write_success.c       # Write success
│   ├── *_scene_write_fail.c          # Write failure + retry
│   ├── *_scene_read_tag.c            # Read tag data
│   ├── *_scene_read_uid.c            # Read UID
│   ├── *_scene_write_uid.c           # Write UID (ByteInput)
│   ├── *_scene_full_dump.c           # Full dump + save .hts
│   ├── *_scene_load_dump.c           # Load .hts + clone
│   ├── *_scene_wipe_tag.c           # Wipe tag
│   └── *_scene_about.c              # About
├── images/                    # Icon assets
├── hitags_writer.png          # App icon (10x10, 1-bit)
├── sim_manchester.py          # Manchester decoder Python simulation tests
└── pixi.toml                 # Pixi environment config
```

## References

- [Proxmark3 RRG — hitagS.c](https://github.com/RfidResearchGroup/proxmark3) — 8268 low-level RF + auth protocol
- [T5577 Multiwriter](https://github.com/Leptopt1los/t5577_multiwriter) — FAP architecture reference
- [Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware) — SDK + Manchester decoder
- [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) — Build tool

## License

MIT License

## Author

Tinnci
