# SatDump Rotator Enhancement Project Roadmap

## Mission Statement

**Goal**: Add Wi-Fi convenience to SatDump's existing rotator control by creating a simple ESP32 bridge that connects to any Yaesu G-5500 compatible rotator wirelessly.

**Scope**: 
- **ESP32 Firmware**: 40-line Wi-Fi-to-UART bridge (no intelligence, just byte forwarding)
- **Leverage Existing Infrastructure**: Use SatDump's built-in rotctld support and Hamlib
- **Hardware**: LilyGO T-Display S3 + MAX3232 level shifter for RS-232 connection
- **No SatDump Modifications**: Works with existing rotator features

## System Architecture

```
┌─────────────────────┐    ┌─────────────────────┐    ┌────────────────────┐
│   SatDump GUI       │───▶│  rotctld (Hamlib)   │───▶│  ESP32 Bridge      │
│  (existing rotator  │    │  • GS-232 protocol │    │  • 40-line firmware│
│   support)          │    │  • TCP client       │    │  • Wi-Fi bridge   │
│                     │    │  • localhost:4533   │    │  • TCP↔UART       │
└─────────────────────┘    └─────────────────────┘    └──────────┬─────────┘
                                                                   │ RS-232
                                                                   │ MAX3232
                                                                   ▼
                                                        ┌──────────────────┐
                                                        │ Yaesu G-5500 Box │
                                                        └──────────────────┘
```

**Data Flow**: SatDump → rotctld (Hamlib) → TCP → ESP32 → UART → Rotator Controller

## Dependencies

### Hamlib Installation (Required)
SatDump requires external Hamlib installation for rotator control:

- **macOS**: `brew install hamlib`
- **Ubuntu/Debian**: `sudo apt install hamlib-utils`  
- **Windows**: SatDump nightlies include `rotctld.exe` in `extras/` folder - no PATH changes needed!
- **Usage**: `rotctld -m 602 -r tcp:ESP_IP:4533 -t 4533`

## Technical Requirements

| Area | Mandatory | Nice-to-Have |
|------|-----------|--------------|
| **Firmware** | PlatformIO, LilyGO T-Display S3, AsyncTCP library, 40-line bridge code | LCD status display, OTA updates, AP fallback |
| **Hardware** | MAX3232 level shifter, DB-9 cable, 3.3V power supply | Custom PCB, enclosure, status LEDs |
| **Dependencies** | Hamlib/rotctld installation, SatDump (existing) | None - uses existing infrastructure |
| **Development** | VS Code + PlatformIO, basic Arduino knowledge | CI/CD, automated testing |

## Hardware Requirements

### Core Components
| Component | Specs | Cost (USD) | Source |
|-----------|-------|------------|--------|
| **LilyGO T-Display S3** | ESP32-S3, 1.9" LCD, USB-C | ~$20 | AliExpress, Amazon |
| **MAX3232 Level Shifter** | 3.3V→RS-232 converter | ~$5 | Electronics suppliers |
| **DB-9 Male-Male Null Modem Cable** | Serial cable for Yaesu GS-232 | ~$10 | Ham radio dealers |
| **Jumper Wires** | GPIO 17/18 to MAX3232 | ~$5 | Any electronics store |

### Wiring Diagram
```
LilyGO T-Display S3        MAX3232          Yaesu G-5500 (DB-9)
┌─────────────────┐       ┌────────┐       ┌─────────────┐
│ GPIO 17 (TX) ●──│──────→│T1 IN   │──────→│ Pin 2 (RX) │
│ GPIO 18 (RX) ●──│←──────│R1 OUT  │←──────│ Pin 3 (TX) │
│ GND          ●──│───────│GND     │───────│ Pin 5 (GND)│
│ 3.3V         ●──│───────│VCC     │       │             │
└─────────────────┘       └────────┘       └─────────────┘

Data Flow:
ESP32_TX → MAX3232_T1IN → DB-9 Pin 2 (Yaesu RX)
ESP32_RX ← MAX3232_R1OUT ← DB-9 Pin 3 (Yaesu TX)
```

### Power Requirements
- **ESP32**: 3.3V via USB-C (development) or external 3.3V supply (permanent installation)
- **MAX3232**: Powered from ESP32 3.3V pin
- **Current Draw**: ~80mA during Wi-Fi transmission, ~20mA idle

## Milestone Tracking

### ✅ Milestone 0: House-keeping (Complete)
- [x] Create upstream remote & pull latest SatDump master
- [x] Repository structure planning
- [x] Initial documentation (CLAUDE.md, roadmap)

### 🚧 Milestone 1: **Software Chain Validation** (REVISED - In Progress)
**Target**: 2-3 days
**Goal**: Prove the full software chain works before adding hardware

| Task | Status | Priority | Notes |
|------|--------|----------|-------|
| Install Hamlib and test rotctld | ⏳ | HIGH | Verify `rotctld -m 602` works on development machine |
| Create 40-line ESP32 bridge firmware | ⏳ | HIGH | PlatformIO project with AsyncTCP Wi-Fi bridge |
| Test ESP32 TCP loopback | ⏳ | HIGH | `telnet ESP_IP 4533` echoes characters |
| Connect rotctld to ESP32 | ⏳ | HIGH | `rotctld` connects to ESP32, SatDump to rotctld |
| Document successful software chain | ⏳ | MEDIUM | Proof that Hamlib→ESP32→(virtual rotator) works |

### ⏸️ Milestone 2: **Hardware Integration** (Queued)
**Target**: 1 week
**Goal**: Add physical rotator connection and basic production features

| Task | Status | Priority | Notes |
|------|--------|----------|-------|
| Wire MAX3232 to ESP32 GPIO 17/18 | ⏸️ | HIGH | Follow wiring diagram above |
| Connect to actual Yaesu rotator | ⏸️ | HIGH | Test real rotator movement commands |
| Hardware smoke test validation | ⏸️ | HIGH | Verify ESP32→rotator communication works |
| Implement Wi-Fi fallback AP mode | ⏸️ | MEDIUM | `RotatorBridge` AP if STA fails |
| Create setup documentation | ⏸️ | MEDIUM | Hardware assembly and first-time setup guide |

### ⏸️ Milestone 3: **Production Polish** (Queued)
**Target**: 1 week
**Goal**: Make it reliable and user-friendly for daily use

| Task | Status | Priority | Notes |
|------|--------|----------|-------|
| Add Wi-Fi configuration web interface | ⏸️ | HIGH | Captive portal for SSID/password setup |
| Change default AP password from "rotator123" | ⏸️ | HIGH | Security improvement for fallback mode |
| Add LCD status display | ⏸️ | MEDIUM | Show current az/el and connection status |
| Implement OTA firmware updates | ⏸️ | MEDIUM | HTTP endpoint for firmware flashing |
| Add connection health monitoring | ⏸️ | MEDIUM | Reconnection logic, status LEDs |
| Create user manual and FAQ | ⏸️ | MEDIUM | Troubleshooting common issues |
| Repository structure and CI | ⏸️ | LOW | Move to firmware/ folder, add GitHub Actions |

### 💭 Milestone 4: **Future Enhancements** (Optional)
**Target**: TBD based on user feedback
**Goal**: Advanced features if there's demand

| Task | Priority | Notes |
|------|----------|-------|
| Enhanced SatDump UI integration | LOW | Only if existing rotator UI is insufficient |
| MQTT/Home Assistant integration | MEDIUM | For home automation enthusiasts |
| Multiple rotator support | LOW | For advanced tracking stations |
| Mobile app companion | LOW | Remote monitoring and control |
| Custom PCB design | LOW | For commercial or kit production |

## Quick Start Tutorial

### Prerequisites
1. **Install Hamlib**:
   - macOS: `brew install hamlib`
   - Ubuntu: `sudo apt install hamlib-utils`
   - Windows: Use `rotctld.exe` from SatDump's `extras/` folder (no installation needed!)

2. **Install PlatformIO**: VS Code + PlatformIO extension

3. **Hardware**: LilyGO T-Display S3 board (MAX3232 comes later)

### Step-by-Step First Success

| Step | Action | Expected Result |
|------|--------|----------------|
| A | Create PlatformIO project with `lilygo_t_display_s3` board | Project builds without errors |
| B | Flash 40-line WiFi bridge firmware to ESP32 | Board connects to WiFi, shows IP on serial |
| C | Test TCP loopback: `telnet ESP_IP 4533` | Characters echo back |
| D | Start rotctld: `rotctld -m 602 -r tcp:ESP_IP:4533 -t 4533` | rotctld connects, no errors |
| E | Test rotctld: `echo 'P' \| nc 127.0.0.1 4533` | Returns `0 0` (current position) |
| F | Connect SatDump: Settings→Tracker→rotctld→127.0.0.1:4533 | SatDump shows "Connected" |
| G | Test SatDump rotator controls | Commands appear in rotctld logs |
| H | Add MAX3232 hardware and connect to Yaesu | Physical rotator moves |

### 40-Line ESP32 Bridge Code
```cpp
#include <WiFi.h>
#include <AsyncTCP.h>

const char* STA_SSID = "YourHomeWiFi";   // <-- change this
const char* STA_PASS = "YourWiFiPass";   // <-- change this

AsyncServer server(4533);

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 18, 17); // UART1: GPIO 18 (RX), 17 (TX)

  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.softAP("RotatorBridge", "rotator123");  // fallback AP
  }

  server.onClient([](void*, AsyncClient* c) {
    c->onData([](void*, AsyncClient* c, void* data, size_t len) {
      Serial1.write((uint8_t*)data, len);  // laptop → UART
    }, nullptr);
  }, nullptr);

  server.begin();
  Serial.printf("WiFi: %s, IP: %s\n", 
    WiFi.status() == WL_CONNECTED ? STA_SSID : "RotatorBridge",
    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
}

void loop() {
  // UART → all TCP clients
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    server.writeAll(&b, 1);
  }
}
```

**Success Criteria**: Complete steps A-G for software validation, step H when hardware arrives.

### PlatformIO Configuration Tips
Add to `platformio.ini` if needed:
```ini
[env:lilygo_t_display_s3]
platform = espressif32
board = lilygo_t_display_s3
framework = arduino
lib_deps = me-no-dev/AsyncTCP@^1.1.1
; upload_port = COM3  ; uncomment and set if Windows can't auto-detect
```

### CI/CD Setup (Optional)
Basic GitHub Action for firmware builds:
```yaml
name: Build ESP32 Firmware
on: [push, pull_request]
jobs:
  esp32:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/cache@v3
        with:
          path: ~/.platformio
          key: platformio-${{ runner.os }}-${{ hashFiles('**/platformio.ini') }}
      - uses: platformio/platformio-action@v2
        with:
          working-directory: firmware
          run: platformio run
      - uses: actions/upload-artifact@v3
        with:
          name: firmware
          path: firmware/.pio/build/*/firmware.bin
```

## Current Sprint Focus

### Sprint 1 (2-3 days): Software Chain Validation
**Goal**: Validate the complete software chain without hardware

**Definition of Done**:
- [ ] Hamlib installed and `rotctld -m 602` working
- [ ] 40-line ESP32 firmware builds and runs
- [ ] TCP loopback test: `telnet ESP_IP 4533` echoes bytes
- [ ] rotctld connects to ESP32: `rotctld -m 602 -r tcp:ESP_IP:4533 -t 4533`
- [ ] SatDump connects to rotctld: Settings→Tracker→rotctld→127.0.0.1:4533
- [ ] Command flow verified: SatDump→rotctld→ESP32→(logs show commands)

**Next Steps**: Order MAX3232 hardware while software is being validated

**Blockers**: None identified - uses existing SatDump rotator infrastructure

### Optional Upstream Sync Workflow
To ensure your fork stays current with SatDump development:
```yaml
name: Upstream Sync
on:
  schedule:
    - cron: '0 6 * * *'  # Daily at 6 AM UTC
  workflow_dispatch:

jobs:
  sync:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          token: ${{ secrets.GITHUB_TOKEN }}
          fetch-depth: 0
      - name: Sync with upstream
        run: |
          git remote add upstream https://github.com/SatDump/SatDump.git
          git fetch upstream
          git checkout main
          git rebase upstream/master
          git push origin main
      - name: Test firmware still builds
        run: |
          cd firmware
          pip install platformio
          platformio run
```

### Pre-built Firmware Releases
Attach firmware binaries to GitHub releases:
```bash
# Flash pre-built firmware with esptool
pip install esptool
esptool.py write_flash 0x0 firmware.bin
```

## Key Decisions & Architecture

### ADR-001: ESP32 as Simple TCP-UART Bridge
**Decision**: ESP32 is a 40-line firmware doing only TCP↔UART passthrough
**Rationale**: Leverages existing SatDump+Hamlib infrastructure, minimal complexity
**Impact**: Faster development, uses proven protocols, easy debugging

### ADR-002: Use Existing SatDump Rotator Infrastructure  
**Decision**: Build on SatDump's existing rotctld support, not custom protocols
**Rationale**: SatDump already has mature rotator features, Hamlib handles GS-232
**Impact**: No SatDump modifications needed, standard rotator compatibility

### ADR-003: Hardware-First Validation Strategy
**Decision**: Validate complete software chain before ordering hardware
**Rationale**: Cheaper to debug software issues than hardware problems
**Impact**: Faster iteration, reduces risk of hardware incompatibility

### ADR-004: Minimal UI Changes Initially
**Decision**: Use existing SatDump rotator UI, enhance only if needed
**Rationale**: Existing UI works well, focus on WiFi convenience first
**Impact**: Faster time to market, user feedback drives UI improvements

## Success Metrics

### Technical
- [ ] ESP32 firmware builds on PlatformIO without errors
- [ ] <1° rotator positioning accuracy (same as wired)
- [ ] <500ms command latency ESP32→rotator  
- [ ] 99%+ Wi-Fi connection uptime in typical home environment
- [ ] Works with existing SatDump rotator features without modification

### User Experience
- [ ] <15 minutes first-time setup (software chain validation)
- [ ] Hardware assembly with basic electronics skills
- [ ] Uses familiar SatDump rotator interface
- [ ] Clear troubleshooting documentation for common issues

### Project Health
- [ ] Firmware builds successfully on GitHub Actions
- [ ] No SatDump core modifications needed (upstream compatibility maintained)
- [ ] Clear documentation for hardware assembly and troubleshooting
- [ ] Community validation with real hardware setups

## Risk Management

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Wi-Fi reliability in RF environment | MEDIUM | MEDIUM | Test in real shack environment, document interference sources |
| MAX3232/RS-232 wiring issues | MEDIUM | MEDIUM | Clear wiring diagrams, test with multimeter |
| ESP32 hardware availability | LOW | MEDIUM | LilyGO is widely available, document alternative ESP32-S3 boards |
| Hamlib compatibility issues | LOW | HIGH | Use standard rotctld interface, test with multiple Hamlib versions |

## Communication Plan

- **Weekly Progress**: Update milestone status in this document
- **Blockers**: Create GitHub issues with "blocked" label
- **Design Questions**: Use GitHub Discussions
- **Code Reviews**: All changes via pull requests
- **Documentation**: Update CLAUDE.md for any workflow changes

---

**Last Updated**: 2025-06-30
**Next Review**: Weekly (Mondays)
**Project Status**: 🚧 Active Development