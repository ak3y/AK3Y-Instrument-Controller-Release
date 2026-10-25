# AK3Y Instrument Controller — Software Interface Specifications

**Document ID:** ENG-004  
**Status:** Implementation reference  
**Scope:** Public interfaces and responsibilities of the firmware modules

## 1. Purpose and scope

This document describes the interfaces between the software components of the AK3Y Instrument Controller. It is based on the implemented C++ headers in the project archive used for the v0.83(a) documentation review, rather than the early framework development plan. It complements `SoftwareArchitecture.md`: that document describes overall organization; this document emphasizes the contracts between components.

The C++ headers remain authoritative for method signatures, argument types, limits, and return codes. This document is a guide to ownership, call paths, and error handling; it is not a substitute for the source code or a complete instrument-profile format specification.

## 2. Interface organization

The controller is built around cooperating services rather than a rigid four-layer dependency hierarchy. The principal paths are:

```text
main.cpp
  -> Application
       -> subsystem and hardware-service initialization / update
       -> Controller (console-driven diagnostics and test operations)
       -> WiFiManager, WebServerManager, OLED, ActivityMonitor

WebServerManager
  -> ConfigurationFile / InstrumentConfiguration (instrument and UI definitions)
  -> CommandExecutor -> IEEE488 -> GPIB -> SPI / MCP23S17 / buffers
                     -> RS232
                     -> TCPInstrument
  -> MacroConfiguration -> MacroExecutor -> CommandExecutor
  -> SDCard (via collaborating services and filesystem operations)
```

The diagram shows conceptual call paths, not an exhaustive include graph or initialization order. `Application` and the module constructors are the reference for actual ownership and startup sequencing.

## 3. Application and framework interfaces

### Application

`Application` coordinates startup, initialization checks, and periodic service updates. Its principal public entry points are `initialize()` and `update()`. It connects subsystem instances and services rather than exposing transport operations to callers.

### Subsystem

`Subsystem` supplies a common virtual `initialize()` contract. Classes such as `Controller`, `WiFiManager`, `OLED`, and `WebServerManager` implement this contract. Not every service derives from `Subsystem`.

### Configuration and Version

`Configuration.h` holds compile-time hardware assignments and project constants. `Version.h` supplies firmware identification declarations. Neither is an SD-card configuration parser. Firmware-version identifiers should be reconciled centrally, not duplicated in documentation.

### Console and Timer

`Console` wraps serial-console output, formatted reporting, and input. `Timer` provides elapsed-time, delay, and timeout helpers. These services support initialization and diagnostics without defining the instrument command language.

## 4. Configuration interfaces

### SDCard and ConfigurationFile

`SDCard` owns initialization and access to the microSD storage interface. `ConfigurationFile` is a **read-only INI-style configuration reader** constructed with an `SDCard&`.

Its `getValue(file, section, key, destination, size)` retrieves a single setting. `getValues(file, section, requests, count)` retrieves multiple settings in one file scan; each `ValueRequest` includes the key, destination, capacity, `required` flag, and returned `found` flag. The latter operation reports invalid requests, read errors, oversized values, and missing required keys as failures.

Browser-based file editing is implemented separately by `WebServerManager`; it does not turn `ConfigurationFile` into a writable parser.

### InstrumentConfiguration

`InstrumentConfiguration` enumerates configured instrument sections and loads each instrument into a typed `Definition`. The definition identifies the selected interface and interface-specific connection settings. The class also provides text conversion helpers for interface and parity values. See `InstrumentConfiguration.h` for exact fields and validation limits.

### MacroConfiguration

`MacroConfiguration` parses SD-card macro definitions into a `Macro` containing metadata, parameters, and ordered steps. Its current header defines `MaxParameters = 8` and `MaxSteps = 32`. It also defines value overrides and parameter-validation support. These limits describe the parsed macro structure; they should not be confused with the separate web-interface override storage limit.

## 5. Instrument command and transport interfaces

### CommandExecutor — command dispatch

`CommandExecutor` accepts textual instrument commands through:

```cpp
Result execute(const char* command, char* response,
               size_t responseSize, size_t& responseLength,
               uint32_t timeoutMilliseconds = 1000);
Result validate(const char* command);
```

It resolves instrument configuration and capability/profile information, selects the appropriate transport, applies configured command/response framing, and returns a typed `Result`. The public result set distinguishes syntax and configuration errors, unsupported interfaces, missing templates, transport failures, and response-parsing errors.

The response buffer is caller-owned; `responseSize` specifies its capacity and `responseLength` reports the returned length. Callers must check the result before using the response.

The executor retains the most recent transport failure in `TransportDiagnostic`, including transport type (`GPIB`, `RS232`, or `TCP`), transport result code, and identifying connection information. `lastTransportDiagnostic()` and `lastTransportErrorText()` expose this information. An optional `DiagnosticCallback` reports failures and subsequent recovery without requiring a direct dependency on `Console`.

### IEEE488 — protocol layer

`IEEE488` implements IEEE-488 addressing and three-wire byte-transfer sequencing above the electrical GPIB driver. Public operations include listener and talker addressing, `write(...)`, `query(...)`, byte reception, and a receive-turnaround helper (`beginReceiveFromTalker(...)`).

The header documents that listener/talker addresses are limited to 0–30, that `write()` asserts EOI with the last message byte, and that `query()` handles turnaround and receives through EOI. Query response buffers include space for the terminating NUL; `responseLength` excludes that terminator. Operations return a `TransferResult` and accept timeouts.

### GPIB — electrical bus interface

`GPIB` controls the physical IEEE-488 signals and data lines, including DAV, NRFD, NDAC, ATN, IFC, REN, SRQ, and EOI. It provides listener/talker direction setup, data reads/writes, signal assertion/release, and low-level diagnostics. The electrical interface uses the SPI-connected MCP23S17 and associated bus-control hardware.

The separation is intentional: callers needing an instrument message should use `IEEE488` (normally via `CommandExecutor`), rather than manually sequencing the GPIB handshake lines.

### RS232 — serial transport

`RS232` manages the controller's two serial instrument ports. Its public interface includes `configurePort(...)`, transport operations, activity counts, and diagnostic/loopback routines. Port configuration and message framing are coordinated with the instrument definition and command executor. Exact settings and error results are specified in `RS232.h`.

### TCPInstrument — network transport

`TCPInstrument` provides TCP-based instrument communication and activity reporting. `CommandExecutor` selects it for configured TCP instruments. Network reachability, endpoint settings, and message termination are separate from GPIB addressing and serial-port configuration; see `TCPInstrument.h` for the actual connection and transfer signatures.

### Instrument — GPIB instrument abstraction

`Instrument` encapsulates a GPIB instrument address and delegates protocol operations to `IEEE488`. It is not the generic configuration/profile database for all three transports; that responsibility belongs to `InstrumentConfiguration` and `CommandExecutor`.

## 6. Macro and script execution interfaces

### MacroExecutor

`MacroExecutor` receives `MacroConfiguration`, `CommandExecutor`, `SDCard`, and `Console` references. Its `execute(...)` overloads accept a macro filename, optional parameter overrides, an optional progress callback, an optional cancellation callback, and a command timeout.

The engine performs preflight checks and executes supported macro steps, including command dispatch, delays, captures, sweeps, and logging. Progress callbacks report completed, total, and current steps. Cancellation is cooperative through the supplied callback; execution returns a typed result including `Cancelled` and command/log/configuration failures.

### ScriptExecutor

`ScriptExecutor` is a separate script-related component retained in the source tree. Its public header includes script listing and result reporting. It should not be treated as synonymous with `MacroExecutor`; consult `ScriptExecutor.h` and its implementation before relying on additional script behavior.

## 7. Browser and user-interface interfaces

### WiFiManager

`WiFiManager` initializes the Wi-Fi connection and exposes connection status, SSID, hostname, IP address, and signal strength (`rssi()`). It does not interpret instrument commands.

### WebServerManager

`WebServerManager` is the HTTP interface and a `Subsystem`. Its constructor receives references to Wi-Fi, configuration, command, macro, activity, and console services. `initialize()` registers/starts web services; `update()` services the web server and pending operations.

Its handlers cover instrument commands, capabilities and function selection, control-panel configuration, workspace interactions, SD-card browsing/upload/editing/deletion, macro start/status/abort, and error pages. The browser UI is generated by the firmware and includes embedded HTML/JavaScript. Changes to these strings must preserve C++ and JavaScript escaping.

The web interface uses `CommandExecutor` for instrument actions and `MacroExecutor` for macro execution. It maintains progress and cancellation state for browser-initiated macros. The current header reserves `WebMacroParameterLimit = 16` slots for web-side macro override storage; this is distinct from `MacroConfiguration::MaxParameters`.

### OLED and ActivityMonitor

`OLED` presents network status, instrument-interface activity, and startup errors. `ActivityMonitor` maintains activity state for periodic status updates. Neither is an alternate instrument-command path.

## 8. Hardware-support interfaces

`SPIManager` manages SPI initialization and transactions. `SPIDevice` provides device-level chip-select and transfer helpers. `MCP23S17` implements register, GPIO direction, port, and pull-up operations for the SPI I/O expander. Their interfaces are defined in their respective headers; callers should avoid duplicating low-level register or chip-select logic in higher-level modules.

`EngineeringValue` parses and formats quantities and units used by instrument capabilities and macro parameters. It is a value-conversion utility, not a transport driver.

## 9. Interface conventions and error handling

- **Explicit dependencies:** Services receive references to the collaborators they need. Initialization and ownership are established by the application and constructors.
- **Typed results:** Instrument commands, macro execution, and IEEE-488 transfers use distinct result enumerations; do not assume that a result code from one subsystem is interchangeable with another.
- **Caller-owned buffers:** Text interfaces commonly accept a destination pointer and size. Callers must provide sufficient capacity and honor returned lengths and status codes.
- **Timeouts:** Transport and command operations use explicit timeouts where their interfaces provide them. Timeouts are not equivalent to successful empty responses.
- **Configuration separation:** Compile-time pin settings, SD-card instrument definitions, capability profiles, and macro definitions have different owners and should not be conflated.
- **Diagnostics:** Console output, low-level self-tests, command transport diagnostics, and web-visible status are separate concerns, connected by defined service interfaces.

## 10. Verification and maintenance

For changes to an interface, check its header and implementation together, then update dependent modules and this document as appropriate. Verify with a PlatformIO build and relevant hardware or browser tests. Documentation-only edits do not change firmware behavior.

Historical milestones and early design plans are preserved in Git and `docs/archive/`; they are not current interface contracts. For exact current APIs and configuration syntax, consult the source files and active configuration examples.
