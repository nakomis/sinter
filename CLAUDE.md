# Sinter

Agentic compute rig: Phenom II X4 965 (AM3) + Tang Primer 20K FPGA + ESP32 + Forth + LLM orchestration.

## Architecture

The ESP32 is the orchestrator. It:
1. Accepts a natural-language problem description
2. Sends it to a local LLM (Ollama) and receives Forth code
3. Compiles the Forth to x86-64 machine code (on-device compiler)
4. Injects the binary into the Phenom II's DDR3 via the FPGA/PCIe bridge
5. Triggers execution on the Phenom II
6. Reads back the result and validates it
7. Iterates with the LLM on failure

The Tang Primer 20K FPGA sits between the ESP32 (SPI) and the Phenom II (PCIe), arbitrating access to shared DDR3 SO-DIMM. It also provides a SATA Gen 1 controller and HDMI framebuffer output.

## Repository Layout

```
embedded/
  esp32/      # ESP32 firmware: orchestration, Forth-to-x86-64 compiler, Ollama client
  fpga/       # HDL (Verilog): DDR3 controller, PCIe Gen1 endpoint, SATA, SPI slave, HDMI
  phenom/     # Bare-metal x86: bootloader, kernel modules, MSR tooling, BIOS analysis
docs/
  architecture/   # draw.io source + generated SVGs
```

## Toolchains

- **ESP32**: ESP-IDF (C/C++)
- **FPGA**: GOWIN EDA toolchain (`gw_sh`) for Tang Primer 20K
- **Phenom II bare-metal**: cross-compiled x86-64 with `x86_64-elf-gcc`
- **Phenom II kernel modules**: standard Linux kernel module build system (Ubuntu)

## Architecture diagrams

Source: `docs/architecture/sinter.drawio` — SVG auto-regenerated on commit by `.githooks/pre-commit`.

To activate the hook after cloning:
```bash
git config core.hooksPath .githooks
```

## Taiga project

Project prefix: SINT — `http://taiga.nakom.is`
