# AK3Y Instrument Controller — Software Architecture

**Document:** `docs/SoftwareArchitecture.md`  
**Project:** AK3Y Instrument Controller  
**Maintainer:** Bob Fontana, AK3Y  
**Status:** Current-implementation reference (documentation cleanup, October 2026)

## 1. Purpose and scope

This document describes the implemented organization of the AK3Y Instrument Controller firmware. It is intended for maintainers and technical reviewers, and complements the root `README.md` and the module-level documentation in `lib/`.

The controller is built with PlatformIO and the Arduino framework, using C++17 on an **ESP32-DevKitC-32E (ESP32-WROOM-32E)**. PlatformIO selects the board with `board = esp32dev`. The project provides IEEE-488 (GPIB), two RS-232 ports, TCP instrument communication, a Wi-Fi browser interface, an OLED status display, and microSD-based configuration and file services.

The descriptions below are based on the project source archive reviewed for this update. Source code remains authoritative for detailed behavior, supported commands, and limits.

## 2. System structure

```text
                         Browser / HTTP clients
                                  |
                          WebServerManager
                           /      |       \
            configuration    commands      macros
                 |              |             |
       ConfigurationFile  CommandExecutor  MacroExecutor
                 |              |             |
       InstrumentConfiguration   |      MacroConfiguration
                                |
                   +------------+------------+
                   |            |            |
                 IEEE488       RS232     TCPInstrument
                   |
                  GPIB
                   |
             MCP23S17 / GPIO
                   |
             IEEE-488 transceivers

          Application (owns and initializes the services)
              |       |       |       |        |
           Console  SDCard  WiFiManager OLED ActivityMonitor
              |       |
             Timer  SPIManager / SPIDevice
```

This is a **responsibility diagram**, not a strict call graph: components also exchange status and diagnostic information, and not every connection is shown. In particular, SD-card access is shared by configuration, macro, and web-file features.

## 3. Application lifecycle and ownership

`src/main.cpp` supplies the Arduino entry point. `Application` is the principal composition root: its member objects include `Console`, `Timer`, `SPIManager`, `MCP23S17`, `SDCard`, `OLED`, `ConfigurationFile`, `MacroConfiguration`, `WiFiManager`, `GPIB`, `RS232`, `TCPInstrument`, `IEEE488`, `ActivityMonitor`, `CommandExecutor`, `MacroExecutor`, `WebServerManager`, `ScriptExecutor`, and `Controller`.

`Application::initialize()` coordinates startup and diagnostics; `Application::update()` supports ongoing service processing. Its `initializeSubsystem()` helper works with the common `Subsystem` initialization interface. Collaborating modules generally receive references to services they use, rather than creating independent copies of the hardware interfaces.

The `Controller` module retains console-oriented command and hardware-test responsibilities. It is **not** a not-yet-implemented layer between the application and every service, as depicted in the original architecture document.

## 4. Instrument communication

### 4.1 IEEE-488 / GPIB

The GPIB path deliberately separates two concerns:

- **`GPIB`** operates the electrical interface: eight data lines, IEEE-488 control signals, transceiver/buffer direction, and underlying MCP23S17/GPIO interactions.
- **`IEEE488`** implements bus-level protocol operations: talker/listener handshakes, addressing, byte transfers, timeouts, and instrument-independent write/query transactions.

`Instrument` supplies a simple GPIB-addressed instrument abstraction. Instrument-specific command strings are handled at higher levels, rather than embedded in the electrical driver. This separation is important when reviewing handshake timing and error recovery.

### 4.2 RS-232 and TCP

`RS232` provides the two serial instrument ports, including port settings and write/read/query operations. `TCPInstrument` provides network instrument transactions and response handling. Both are separate transports rather than special cases of the GPIB driver.

### 4.3 Command dispatch and configuration

`InstrumentConfiguration` represents configured instruments and their interface settings. `ConfigurationFile` reads INI-style configuration values from the SD card. `CommandExecutor` validates and dispatches commands to the appropriate instrument transport and reports transport errors. `EngineeringValue` supports conversion and formatting of numerical values used by instrument controls.

The specific configuration keys, command syntax, supported transports, and error codes are defined by the corresponding source files; this architecture document does not duplicate them.

## 5. Web interface

`WebServerManager` implements the HTTP-facing user interface using the Arduino `WebServer` library. Its responsibilities include:

- Instrument discovery from configuration and profile definitions.
- Generation of browser control panels and capability-based controls.
- Dispatch of instrument commands and presentation of responses.
- Macro initiation and progress/status interaction.
- SD-card file browsing and supported file-management operations.
- Workspace/control-panel configuration and persistence.
- Presentation of diagnostic and instrument-activity information.

The implementation generates HTML and JavaScript from C++ source. Consequently, seemingly cosmetic changes to generated strings can alter browser behavior; review and test those changes independently of documentation edits.

## 6. Macro and script execution

`MacroConfiguration` loads and validates macro definitions and their parameters. `MacroExecutor` performs macro preflight and execution, including command dispatch, delays, captures, sweeps, logging, progress reporting, and cancellation as supported by the implementation. `ScriptExecutor` provides a separate SD-card command-script execution path.

The execution modules use shared instrument-command services rather than implementing independent electrical-interface drivers. Detailed directive syntax belongs in the corresponding module and user-facing reference documentation.

## 7. Supporting services

| Module | Architectural responsibility |
|---|---|
| `SDCard` | Initialize and access microSD storage; provide storage diagnostics |
| `SPIManager` / `SPIDevice` | Share the SPI bus and support SPI peripherals |
| `MCP23S17` | SPI GPIO-expander register and port operations |
| `WiFiManager` | Wi-Fi configuration, connection, and network status |
| `OLED` | Local SSD1306 status and error display |
| `ActivityMonitor` | Collect instrument-interface activity for display |
| `Console` | Serial diagnostic and operator-console output |
| `Timer` | Timing and timeout support |
| `Version` / `Configuration` | Project/build identifiers and compile-time settings |

Configuration and diagnostic information may be surfaced in more than one place (serial console, OLED, and web UI). These are presentation paths, not separate sources of hardware state.

## 8. Repository organization

```text
AK3Y_Instrument_Controller/
├── src/                 Arduino firmware entry point
├── include/             Shared project headers
├── lib/                 Project modules and PlatformIO library manifests
├── docs/                Current technical documentation
│   └── archive/         Historical engineering documents
├── test/                Test material, where present
├── platformio.ini       Build environment and dependencies
└── README.md            Project overview and build instructions
```

Module responsibilities are documented primarily in `.h` and `.cpp` file headers. Per-library README files are **not required**. Existing `library.json` files provide PlatformIO library metadata and should be retained unless their build role has been independently verified to be unnecessary.

Generated `.pio/` contents and Git metadata are not part of the source architecture.

## 9. Design and maintenance principles

- Keep physical bus control, transport protocols, command dispatch, and browser presentation conceptually separate.
- Preserve explicit ownership and initialization order in `Application`.
- Use bounded operations and meaningful diagnostics for instrument I/O failures.
- Keep instrument definitions and macros data-driven where supported by the implementation.
- Preserve explanatory comments on IEEE-488 timing, buffer direction, and error handling.
- Track development history in Git rather than repeating milestone logs in source headers.
- Test generated HTML/JavaScript behavior after any web-interface change, even when the C++ build succeeds.

## 10. Boundaries and future work

This document describes the present implementation, not a commitment to future interfaces. General amateur-radio CAT control, receiver characterization, and automated SINAD measurements are possible future applications and are **not represented here as implemented capabilities**.

The exact firmware version is deliberately not duplicated in this document. Review the central version definition in `Configuration.h` and reconcile it with release practice before using a version identifier in publication or distribution.

## 11. Related documents

- `README.md` — overview, hardware identification, and build procedure.
- `docs/InterfaceSpecifications.md` — interface reference; requires a separate update against the current source.
- `docs/CodingStandards.md` — coding conventions; review for consistency with the documentation cleanup.
- `docs/archive/` — preserved project history, design notes, and bring-up records.
