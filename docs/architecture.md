# Architecture and release workflow

The project is a native C application built with OpenOrbis. Its stream comes from Steam on the PC. The PS4 client discovers and pairs with the host, receives compressed video and audio, presents frames, and forwards controller input.

| Component | Responsibility |
| --- | --- |
| `src/main.c` | Startup, discovery, pairing, host selection, stream profiles, session state, and diagnostics sampling |
| `src/media.c` | H.264 and Opus handling, frame tracking, CPU color conversion and scaling, and display submission |
| `src/ps4_video.c` | Native Videodec2 integration and staging decoded NV12 surfaces for the renderer |
| `src/screen.c` | VideoOut buffers, display calibration, menus, and overlays |
| `src/gamepad.c` | DualShock input and Steam gamepad reports |
| `src/platform.c` | PS4 platform integration for IHSlib |
| `src/startup.c` | Startup and session logging |
| `patches/` | Changes applied to the pinned IHSlib revision |
| `scripts/` | Dependency preparation, cross-compilation configuration, and packaging |

## Video and input flow

1. Steam captures and encodes the game or desktop. The selected profile requests maximum resolution, frame rate, and bitrate. Steam negotiates the active stream.
2. IHSlib assembles video access units. The software profile uses FFmpeg H.264. HW profiles attempt Videodec2 and fall back to software if the native path fails.
3. The media path keeps the latest ready frame for presentation. A superseded image can be dropped before display. The renderer scales and converts YUV to RGB in cached memory, then copies to the VideoOut buffer.
4. The three display buffers allow preparation while a prior frame is queued. Buffers are reused only after VideoOut reports a newer displayed frame. The renderer recalculates aspect and crop for the incoming frame.
5. Opus stereo is decoded into the audio queue. DualShock state is sent through the session input channel, with local shortcuts for disconnect and calibration.

## Builds and releases

- Follow the Linux x86-64 build steps in the [README](../README.md). The build uses pinned dependencies, FFmpeg with x86 assembly and pthreads, and OpenOrbis 0.5.4.
- Run **Build PS4 package** from the repository's GitHub Actions tab to build a chosen branch. The artifact contains the PKG and SHA-256 checksum. The packaging script runs the OpenOrbis validator.
- Package validation checks the file. Check installation, discovery, pairing, controller input, audio, Fit/Fill, and sustained streaming on a physical PS4 before declaring a build stable.
- When investigating regressions, record build version, game and scene, selected profile, received resolution, active decoder, RX/DEC/OUT, and whether the same issue occurs with an official Steam Link client.
- Preserve the PS4 title ID when shipping updates. Check the package filename, version strings, and release notes together.
