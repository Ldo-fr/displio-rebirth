# Contributing to displio-rebirth

Thank you for helping give Displio V1.0 displays a second life! This is an independent, non-profit, volunteer-driven project and contributions of all kinds (code, documentation, hardware findings, bug reports) are welcome.

## Development Setup

```bash
# Install ARM toolchain (Debian/Ubuntu)
sudo apt install gcc-arm-none-eabi make

# Build custom firmware
make -C firmware

# Build hardware probe (optional)
make -C probe

# Verify Python host tools syntax
python3 -m py_compile tools/displio_tool.py tools/displio_app.py
```

## Guidelines

1. **Branch from `main`**: create a dedicated branch (`feat/<slug>` or `fix/<slug>`).
2. **Conventional Commits**: use clear commit messages (`feat(epd): ...`, `fix(cc3000): ...`, `docs: ...`).
3. **Zero warnings**: firmware must compile cleanly with `-Wall -Wextra` and fit within STM32F103xB Flash (112 KiB app area) and SRAM (20 KiB).
4. **Never touch the bootloader**: keep all Flash writes strictly within `0x08004000`–`0x0801FFFF` and never corrupt the bootloader CRC trailer at `0x0801FFFC`.
5. **Update docs**: if your change affects CLI commands, pinout, protocol, or HTTP API, update `README.md` and `docs/*` in the same pull request.
6. **Never commit personal data**: do not commit Flash dumps (`dumps/`), Wi-Fi credentials, or private IP/MAC data.

## Pull Requests

Open a Pull Request against `main` describing what changed and how it was tested (including whether it was verified on live Displio hardware).
