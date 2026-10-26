# Example microSD card configuration

These files demonstrate instrument inventory entries, instrument profiles, and macros for the AK3Y Instrument Controller. They are **examples**, not a universal configuration. Edit the addresses and connection parameters to match your instruments before using them.

## Contents

```text
sdcard/
├── instruments.cfg
├── instruments/
│   ├── marconi_2024.cfg
│   ├── marconi_2024_rs232.cfg
│   ├── e4407b.cfg
│   └── lan_simulator.cfg
└── macros/
    ├── FrequencySweep.cfg
    ├── FindSignal.cfg
    └── lan_instrument_sweep.cfg
```

### Inventory (`instruments.cfg`)

| Identifier | Example device | Interface | Example setting |
| --- | --- | --- | --- |
| `RFGEN` | Marconi 2024 RF generator | GPIB | Address 20 |
| `SERIAL1` | Marconi 2024 RF generator | RS-232 | Port 1, 9600 baud, 8N1 |
| `SPECTRUM` | HP/Agilent E4407B spectrum analyzer | GPIB | Address 18 |
| `LANSIM` | AK3Y LAN instrument simulator | TCP | `192.168.1.155:5025` |

The inventory references the profile filenames under `instruments/`. The two Marconi entries illustrate alternative interfaces; you need not configure both if you use only one connection.

### Macros

- **`FrequencySweep.cfg`** sweeps `RFGEN` (Marconi over GPIB), measures marker frequency and amplitude using `SPECTRUM` (E4407B), and logs results to `/logs/frequency_sweep.csv`.
- **`FindSignal.cfg`** uses `SPECTRUM` to search for and refine a peak, then reports its frequency and amplitude.
- **`lan_instrument_sweep.cfg`** exercises `LANSIM` over TCP, steps frequency, reads values back, and logs to `/logs/lan_sweep.csv`.

These macros require the corresponding instrument entries and functioning connected devices or simulator. They are not expected to run unchanged on a controller without that equipment.

## Using the examples

1. Prepare a microSD card using the format specified in the User's Manual.
2. Copy the **contents of this `sdcard/` directory** to the **root of the microSD card**, retaining the filenames and subdirectories shown above. Do not copy the outer repository `examples/` directory onto the card.
3. Edit `instruments.cfg` to match your equipment: GPIB addresses, RS-232 port and serial settings, and TCP host/port. In particular, `192.168.1.155` is only the author's example LAN address; it is not a default address for other installations.
4. Keep the `profile=` filenames consistent with the files under `instruments/`. Adjust instrument-specific profile commands only if required for your equipment.
5. Insert the card, start the controller, and confirm that the configured instruments are recognized before running macros.
6. Refer to the User's Manual for configuration syntax, instrument operation, macro commands, and file management.

## Security and troubleshooting

No Wi-Fi credentials are needed in these examples. **Do not publish passwords or private network configuration files** when sharing a modified microSD image. TCP instruments must be reachable from the controller's network, and GPIB/RS-232 instruments must be connected and configured with matching interface settings.

The macros write CSV logs under `/logs/`; check that the microSD card is mounted and writable. If a command fails, verify the instrument identifier, profile filename, physical interface, and device-specific command support.
