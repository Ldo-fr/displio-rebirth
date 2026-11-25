# Security Policy

## Network Security Model

The Displio V1.0 runs on an **STM32F103xB** (20 KiB SRAM) with a legacy **TI CC3000** Wi-Fi module. Due to these hardware constraints:

- The HTTP REST API (`GET /status`, `POST /display`, `POST /beep`) runs over **unencrypted HTTP** without authentication.
- **Do not expose the Displio directly to the public internet** (no port forwarding).
- Deploy the device on a **trusted local network or isolated IoT VLAN**.
- Wi-Fi credentials are stored locally in STM32 Flash (`0x0801F800`) and are never transmitted over HTTP or to any external server.

## Reporting a Vulnerability

If you discover a security issue in the firmware or host tools, please open a GitHub Security Advisory (via the **Security → Advisories** tab of this repository) or contact the maintainer privately rather than posting exploit details in a public issue.
