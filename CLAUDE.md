# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

SatDump is a generic satellite data processing software with a simple Wi-Fi enhancement:
- **Core**: Existing satellite data processing, demodulation, and decoding capabilities (unchanged)
- **ESP32 Wi-Fi Bridge**: 40-line firmware providing wireless connection to Yaesu rotators
- **Existing Rotator UI**: Uses SatDump's built-in rotator interface via rotctld (Hamlib)

## Repository Structure

```
SatDump/
├── src-core/          # Core SatDump library code
├── src-interface/     # Main GUI application
├── src-cli/          # CLI application
├── plugins/          # Satellite-specific processing modules
├── pipelines/        # Processing pipeline definitions (.json)
├── resources/        # Calibration data, fonts, maps, themes
├── firmware/         # NEW: ESP32 Wi-Fi bridge firmware (PlatformIO)
└── docs/            # Documentation
```

## Build System

### Main Application
- **Build System**: CMake 3.12+
- **Language**: C++17
- **GUI Framework**: ImGui (existing), Qt/QML (new rotor UI)

### Common Build Commands
```bash
# Configure build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build (Linux/macOS)
make -j$(nproc)

# Build (Windows with Visual Studio)
cmake --build . --config Release

# Run without installing
ln -s ../pipelines .
ln -s ../resources .
ln -s ../satdump_cfg.json .
./satdump-ui
```

### Build Options
- `-DBUILD_GUI=OFF` - Disable GUI build
- `-DBUILD_TESTING=ON` - Enable testing
- `-DBUILD_TOOLS=ON` - Build additional tools
- `-DPLUGIN_[SDR]_SUPPORT=OFF` - Disable specific SDR support

### ESP32 Firmware
- **Build System**: PlatformIO
- **Target**: LilyGO T-Display S3 (ESP32-S3)
- **Framework**: Arduino
- **Dependencies**: AsyncTCP library (auto-installed)

```bash
# From firmware/ directory
pio run                    # Build firmware
pio run --target upload    # Flash to device
pio device monitor         # Monitor serial output
```

## Dependencies

### Hamlib Installation (Required)
SatDump requires external Hamlib for rotator control:

```bash
# macOS
brew install hamlib

# Ubuntu/Debian
sudo apt install hamlib-utils

# Windows
# Use rotctld.exe from SatDump's extras/ folder - no installation needed!
```

**Usage**: `rotctld -m 602 -r tcp:ESP_IP:4533 -t 4533`

## Testing

### Main Application Tests
- **Framework**: Custom test framework in src-testing/
- **Run Tests**: `make test` (after building with BUILD_TESTING=ON)

### ESP32 Firmware Validation
- **Loopback Test**: `telnet ESP_IP 4533` should echo characters
- **rotctld Test**: `echo 'P' | nc 127.0.0.1 4533` should return `0 0`
- **Integration Test**: SatDump → rotctld → ESP32 → (command logs)

## Architecture Guidelines

### Core Principles
1. **Don't modify core SatDump logic** unless fixing compilation breaks from upstream
2. **Maintain upstream compatibility** - rebase frequently from SatDump/SatDump
3. **Modular design** - ESP32 firmware and UI enhancements should be self-contained

### Key Components

#### DSP Pipeline
- Located in `src-core/common/dsp/`
- Block-based processing architecture
- Modules communicate via buffers

#### Plugin System
- Satellite-specific code in `plugins/`
- Each plugin has its own CMakeLists.txt
- Modules inherit from base classes in `src-core/core/module.h`

#### Configuration
- Pipeline definitions: `pipelines/*.json`
- Settings: `satdump_cfg.json`
- Calibration data: `resources/calibration/`

### ESP32 Bridge Architecture
```
SatDump → rotctld (Hamlib) → TCP → ESP32 → UART → Yaesu G-5500
```

#### Responsibilities
- **SatDump**: All orbital calculations, pass scheduling, tracking logic (unchanged)
- **rotctld (Hamlib)**: GS-232 protocol translation, command validation
- **ESP32**: Only TCP↔UART passthrough (40 lines of code)
- **MAX3232**: 3.3V↔RS-232 level conversion

#### Key Integration Points
- **Existing rotator code**: `src-core/common/tracking/rotator/`
- **rotctld handler**: `RotctlHandler` class already implemented
- **CLI autotrack**: `src-cli/autotrack/` has complete implementation

## Hardware Setup

### Required Components
| Component | Specs | Cost | Purpose |
|-----------|-------|------|----------|
| LilyGO T-Display S3 | ESP32-S3, 1.9" LCD, USB-C | ~$20 | Wi-Fi bridge + status display |
| MAX3232 Level Shifter | 3.3V↔RS-232 converter | ~$5 | Voltage level conversion |
| DB-9 Male-Male Null Modem Cable | For Yaesu GS-232 port | ~$10 | Connect to Yaesu rotator |
| Jumper Wires | Dupont connectors | ~$5 | GPIO connections |

### Wiring Connections
```
LilyGO GPIO    →    MAX3232    →    Yaesu G-5500 (DB-9)
GPIO 17 (TX)   →    T1 IN      →    Pin 2 (RX)
GPIO 18 (RX)   →    R1 OUT     →    Pin 3 (TX)
GND            →    GND        →    Pin 5 (GND)
3.3V           →    VCC

Data Flow:
ESP32_TX → MAX3232_T1IN → DB-9 Pin 2 (Yaesu RX)
ESP32_RX ← MAX3232_R1OUT ← DB-9 Pin 3 (Yaesu TX)
```

### First-Time Setup
1. Flash ESP32 with bridge firmware
2. Connect to Wi-Fi (hardcoded initially)
3. Test with `telnet ESP_IP 4533`
4. Install Hamlib and run `rotctld -m 602 -r tcp:ESP_IP:4533 -t 4533`
5. Connect SatDump: Settings → Tracker → rotctld → 127.0.0.1:4533

## Development Workflow

### Branch Strategy
- `main` - Mirror of upstream SatDump/SatDump (never edit directly)
- `esp32-fw` - ESP32 firmware development in firmware/ folder
- **Simple approach**: firmware/ folder is ignored by upstream, no merge conflicts

### Daily Workflow
1. Pull upstream changes: `git pull upstream master`
2. Rebase feature branches on latest upstream
3. Run full build + tests before commits
4. Use pre-commit hooks for code formatting

### Code Style
- **C++**: Follow existing SatDump style, use clang-format
- **Arduino**: Standard Arduino/PlatformIO style with AsyncTCP
- **Keep it simple**: 40-line firmware, minimal dependencies

## Key Files and Locations

### Configuration
- `CMakeLists.txt` - Main build configuration
- `satdump_cfg.json` - Runtime configuration
- `pipelines/*.json` - Processing pipeline definitions

### Core Modules
- `src-core/core/module.h` - Base module class
- `src-core/common/dsp/` - DSP building blocks
- `src-interface/main_ui.cpp` - Main GUI application

### Rotator Integration Points
- `src-core/common/tracking/rotator/rotator_handler.h` - Base rotator interface
- `src-core/common/tracking/rotator/rotcl_handler.h` - rotctld TCP client
- `src-cli/autotrack/autotrack.h` - Complete autotrack implementation
- `src-interface/recorder/tracking/` - GUI tracking widgets
- **No new integration needed** - ESP32 appears as standard rotctld device

## External Dependencies

### Required Libraries (SatDump)
- FFTW3 - Fast Fourier Transform
- libpng, libtiff - Image processing
- libcurl - Network operations
- VOLK - Vector optimized library
- nng - Networking library
- **Hamlib** - rotctld daemon for rotator control

### Required Libraries (ESP32 Firmware)
- **AsyncTCP** - Non-blocking TCP server (auto-installed by PlatformIO)
- **WiFi** - Built into ESP32 Arduino framework
- **No other dependencies** - keeps firmware minimal

### Optional Libraries
- OpenCL - GPU acceleration
- PortAudio - Audio output
- Various SDR libraries (RTL-SDR, HackRF, etc.)

## Common Development Tasks

### Adding New Satellite Support
1. Create plugin directory in `plugins/`
2. Implement demodulator and decoder modules
3. Add pipeline definition in `pipelines/`
4. Update CMakeLists.txt

### Debugging Build Issues
1. Check CMake configuration output
2. Verify all dependencies installed
3. Check platform-specific build docs in README.md
4. Use `VERBOSE=1 make` for detailed build output

### ESP32 Development
1. **Setup**: Install VS Code + PlatformIO extension
2. **Create Project**: Board = `lilygo_t_display_s3`, Framework = Arduino
3. **Flash Code**: 40-line WiFi bridge firmware
4. **Test**: `telnet ESP_IP 4533` for TCP loopback
5. **Debug**: Serial monitor shows WiFi connection status and IP
6. **Hardware**: Wire MAX3232 to GPIO 17/18 for RS-232 connection

## Important Notes

- **Never commit secrets** or hardcoded Wi-Fi credentials in firmware
- **No SatDump modifications** - ESP32 works with existing rotator features
- **Hamlib dependency** - rotctld must be installed separately on development machine
- **Hardware validation** - Test software chain before ordering MAX3232 hardware
- **GPIO pin safety** - Double-check 3.3V levels before connecting to MAX3232
- **Wi-Fi interference** - Test in actual shack environment with RF equipment
- **Simple is better** - 40-line firmware is easier to debug than complex solutions
- **Use existing infrastructure** - SatDump already has excellent rotator support