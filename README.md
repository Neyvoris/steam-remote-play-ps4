# Steam Remote Play PS4

An experimental, unofficial Steam Remote Play client for PlayStation 4 running GoldHEN. Your PC runs the game and streams it over your local network. The PS4 plays the video and stereo Opus audio and sends input from one DualShock 4.

## Quick start

1. On your PC, enable **Steam > Settings > Remote Play** and leave Steam running. Connect the PC and PS4 to the same local network.
2. Download the PKG from [GitHub Releases](https://github.com/ivanopolo30-dot/steam-remote-play-ps4/releases), install it using Package Installer, and open **Steam Remote Play PS4**.
3. Select your PC with the D-pad and press **Cross**. If the PS4 displays a PIN, enter it in **Steam > Settings > Remote Play > Pair Steam Link**. The client saves the pairing and starts Steam Big Picture. Press **Square** in the host list to pair again.
4. Launch a controller-compatible game in Big Picture. Press **OPTIONS + touchpad click** to end the stream. During gameplay, a touchpad click sends Back.

## Video profiles

Press **R1** in the host list to cycle profiles. Your selection is saved and applied on the next connection. A fresh installation defaults to **720p / 30 FPS (HW)**.

| Profile | Requested bitrate limit | Decoder |
| --- | ---: | --- |
| 720p / 30 FPS (HW) | 6 Mbps | Videodec2 |
| 720p / 30 FPS | 6 Mbps | FFmpeg |
| 720p / 60 FPS | 11 Mbps | FFmpeg |
| 1080p / 30 FPS | 14 Mbps | FFmpeg |
| 1080p / 60 FPS | 20 Mbps | FFmpeg |
| 1080p / 60 FPS (HW) | 20 Mbps | Videodec2 |

HW profiles use the PS4 system module `libSceVideodec2` for H.264 and fall back to FFmpeg if hardware decoding fails. Actual resolution and frame rate depend on Steam, the network, and the PS4.

## Screen fit

Open display calibration with **OPTIONS** in the host list or **L3 + R3** during a stream. Use **L1** to select the top-left corner, **R1** to select the bottom-right corner, and the D-pad to move the selected corner. Press **Triangle** to switch between **Fill**, which crops to fill the area while preserving aspect ratio, and **Fit**, which shows the entire frame and may leave black bars.

Version 0.2.4 defaults to **Fit** and applies it automatically as the incoming frame changes aspect ratio. Upgrades switch older configurations to Fit while retaining calibration within the display bounds. A 16:9 stream fills a 16:9 screen, while ultrawide frames may have black bars above and below. Press **Square** to restore full-screen defaults, **Cross** to save, or **Circle** to exit without saving.

## Diagnostics

The in-stream overlay shows the current decoder (**HW**, **SW**, or **SW fallback**) and the rates for frames received (**RX**), decoded (**DEC**), and submitted to VideoOut (**OUT**). It also shows average video packet processing and frame preparation times in microseconds. In 0.2.2 and later, the frame preparation time excludes the final copy to the display buffer. **LOG open** means the log file was opened successfully.

The PS4 log at `/data/steam-remote-ps4/startup.log` records connection stages, decoder fallback errors, and periodic frame rates. If RX drops, compare that interval with `logs/streaming_log.txt` in the Steam installation on your PC. If RX and DEC stay steady while OUT drops, investigate the PS4 display path. Share relevant log excerpts without pairing files.

- `Pairing (7)` means Steam rejected the pairing request.
- `Stream (7)` means Steam reported missing drivers on the PC.
- If the PS4 cannot open the audio output, video continues and the audio error is logged.

## Build on Linux x86-64

Install Clang/LLD, NASM, CMake, GNU Make, Git, curl, tar, and [OpenOrbis 0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4). The SDK's `PkgTool.Core` may require OpenSSL 1.1. If needed, point `LD_LIBRARY_PATH` to a local directory containing `libcrypto.so.1.1` and `libssl.so.1.1`.

```bash
export OO_PS4_TOOLCHAIN=/absolute/path/to/OpenOrbis/PS4Toolchain
export PS4_DEPS="$PWD/deps"
bash scripts/dependencies.sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/scripts/orbis.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
bash scripts/package.sh
```

The dependency script fetches pinned sources and builds FFmpeg. The package script writes and validates the PKG in `dist/`. GitHub Actions can also build and validate the package. Performance, firmware compatibility, and input latency require testing on a physical PS4.

## Source and licenses

- The application is licensed under GPL-3.0-only. See [LICENSE](LICENSE).
- The Steam Remote Play protocol implementation uses [IHSlib](https://github.com/kxn/ihslib/tree/04688d19951c68bedc4370234cc1752043632cb3) from the [IHSplay](https://github.com/mariotaku/ihsplay) project, under LGPL-3.0-or-later.
- The package uses [FFmpeg](https://ffmpeg.org/) 6.1.4 for H.264 decoding (LGPL), [Opus](https://github.com/xiph/opus) 1.5.2 (BSD-3-Clause), [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 2.28.10 (Apache-2.0 or GPL-2.0-or-later), and [protobuf-c](https://github.com/protobuf-c/protobuf-c) 1.5.2 (BSD-2-Clause). See the upstream sources for license terms.
- The package includes helper modules from [OpenOrbis 0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/tree/v0.5.4).
- References for the PS4 hardware decoder ABI include [Moonlight DX PS4](https://git.fourletterworkshop.com/moonlight-dx-ps4/blob/main/src/moonlight/VideoDecoder.cpp) and [shadPS4](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/videodec/videodec2.h).
- The UI uses the public domain [font8x8](https://github.com/dhepper/font8x8) font. This project is unaffiliated with Valve or Sony.
