# Setup and troubleshooting

## Install or update

- Use a PS4 with GoldHEN running, a DualShock 4, and a PC with Steam Remote Play enabled. Connect both devices to the same local network.
- Download the PKG from the release assets. Put it on a USB drive and install it using Package Installer on PS4. A source archive is for building the client.
- Launch Steam on the PC before opening the PS4 app. Select the host with the D-pad and press Cross. For first-time pairing, enter the displayed PIN under Steam > Settings > Remote Play > Pair Steam Link.
- Press Square to pair again if the saved pairing fails. Press Circle to cancel pairing or a connection attempt. During a stream, press OPTIONS and click the touchpad together to disconnect.
- The client requests Big Picture. Start the game there. Check controller support in the game if input is missing.

## Configure Steam on the PC

Open Steam > Settings > Remote Play and enable advanced host options. Leave hardware encoding enabled as a starting point. Steam's log reports the actual encoder, such as NVIDIA NVENC H.264.

For a 16:9 TV and a 2560x1080 desktop, try **Display resolution for streaming > Custom settings**, with width **1920** and height **1080**. Reconnect the stream after changing this. This is a Steam setting and requires no additional Windows program. It requests a display mode for streaming. The game or Steam may still switch capture resolution, so check the log after launching the game.

Steam can capture the Windows desktop when a game loses focus. If Alt+Tab exposes your browser or desktop on the TV, return focus to the game or disconnect before switching apps. Big Picture on connection does not pin the capture source throughout the session.

## Match the TV

| Goal | Setting | Result |
| --- | --- | --- |
| Show every pixel with correct proportions | Fit | A wide stream may show bars above and below |
| Fill the TV while keeping correct proportions | Fill | Parts of the frame can be cropped |
| Show everything and fill a 16:9 TV | Fit with a 16:9 source | A full 1920x1080 stream matches the display |

For example, Steam can scale a 2560x1080 source to 1920x810. Fit shows the whole frame with bars. Fill crops the sides. If custom 1920x1080 in Steam does not resolve it, confirm the game and capture display are both using 16:9. In borderless mode, a game can follow the desktop resolution. The PS4 app does not change the Windows display mode itself.

Open screen calibration with OPTIONS from the host list or L3 + R3 during a stream. Square resets the bounds. Triangle changes Fit/Fill. L1/R1 select a corner and the D-pad moves it. Cross saves and Circle cancels.

## Diagnose FPS drops

Use the same game, scene, profile, and screen mode for comparisons. Record the overlay while the drop is happening.

| Overlay | What it measures | Where to investigate |
| --- | --- | --- |
| RX | Frames assembled by the client | Steam capture/encoding, connection, or sender throttling |
| DEC | Decoded frames | Decoder, fallback, or decode queue |
| OUT | Frames submitted to VideoOut | Frame preparation, buffer availability, and display submission |
| HW / SW / SW fallback | Active decoder path | Native Videodec2, FFmpeg, or a transition to FFmpeg |
| Two values in us | Average packet processing and frame preparation times | CPU work per packet and per prepared frame |
| LOG open / failed | Whether opening the PS4 log succeeded | Local logging availability |

At 60 FPS, frames arrive about every 16.7 ms. The frame preparation metric excludes the final display-buffer copy and waiting for VideoOut. OUT counts submissions, so it is not a direct measurement of HDMI refresh. Steam timing summaries can contain anomalous network values and should be compared with the on-screen counters.

One observed 1080p/60 software session showed RX 59, DEC 59, OUT 11. A later build that staged frames in cached memory showed OUT around 41-42 in sampled scenes. These captures establish that output can bottleneck even when decode keeps up. They do not establish a guaranteed rate across games or consoles. Compare software and HW profiles in the same scene. A prior HW session reported high decode-stage latency, so HW should be measured rather than assumed to be faster.

## Logs and common errors

- PC log: `logs/streaming_log.txt` under the Steam installation. Look near the time of the test for PS4 client identification, capture resolution, encoder, and frame-rate changes.
- PS4 log: `/data/steam-remote-ps4/startup.log`. It records startup, pairing, connection, decoder changes, and frame rates. The overlay works even if FTP cannot list the directory.
- If an FTP directory listing fails, that alone does not tell you whether the file exists. Avoid changing permissions on pairing data. Share only relevant log excerpts and redact addresses, device names, and account identifiers as needed.
- Pairing error 7: Steam rejected or could not complete pairing. Retry from the host list and enter the new PIN.
- Streaming error 2: unlock the Windows desktop. Error 7: Steam reports missing host drivers. Error 8: enable Remote Play. Error 11: Steam requires its Remote Play security PIN.
- If video works without sound, check the PS4 audio output error. The client keeps video running if audio initialization fails.
- If no host appears, check that Steam is open, Remote Play is enabled, and both devices are on the same network. Recheck firewall and device isolation settings if discovery still fails.
