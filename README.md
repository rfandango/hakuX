# X1 BOX

X1 BOX is an original Xbox emulator for Android, based on xemu.

> [!IMPORTANT]
> X1 BOX is resource-heavy. A device with **8 GB RAM or more is recommended**.  
> Lower-end devices may run poorly or fail to run games correctly.  
> **Vulkan support is required.**

## Project Origin

- This project is forked from: https://github.com/izzy2lost/xemu
- Upstream xemu project: https://xemu.app

## What It Does

X1 BOX packages xemu for Android with an Android-first flow and launcher UI.

## Key Features

- Setup wizard to configure required Xbox files
- Game library browser with list and cover-grid views
- Optional online box-art lookup in the game library
- On-screen virtual Xbox controller for touch devices
- Automatic hide/show of on-screen controls when a physical controller is connected
- Support for physical USB/Bluetooth game controllers
- ARM64 (`arm64-v8a`) Android target

## Device Requirements

- Android 8.0+ (API 26 or newer)
- 64-bit ARM device (`arm64-v8a`)
- Vulkan-capable GPU/device
- Recommended: 8 GB RAM or higher for better stability and performance

## Setup Requirements (User Files)

To use the emulator, you must provide your own legally obtained original Xbox files, such as:

- MCPX Boot ROM
- Flash ROM / BIOS
- Xbox hard disk image
- Game files/images from your own discs

X1 BOX does not include copyrighted BIOS or game content.

## License

This repository inherits xemu/QEMU licensing:

- Emulator code is primarily under **GNU GPL v2**
- Some components are under other GPLv2-compatible licenses (for example BSD/MIT in parts of TCG)

See these files for details:

- `LICENSE`
- `COPYING`
- `COPYING.LIB`

## Notes

- X1 BOX is not affiliated with Microsoft.
- Compatibility and performance vary by device and game.
