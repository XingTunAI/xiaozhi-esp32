Upstream: https://github.com/Averyy/esp-uac2-host
Commit: a072adb6a2410f9f93474637a483724c93d5fc6f
License: MIT (LICENSE retained).
Local changes: IDF 6 USB component dependency; restrict stream timing to full-speed for the H2 path. P4 hardware validation is recorded in the board HARDWARE-VALIDATION.md.

P4 adaptation now handles FS 1ms and HS 125/250/500/1000us data service intervals. Packet size uses actual interval; HS feedback samples/microframe are scaled to the data endpoint interval. HS high-bandwidth multiple-transaction endpoints are rejected. RX hardware validation passed on reSpeaker Flex at HS, 16kHz stereo S16, 32 bytes per 500us interval. FS physical wiring, playback feedback, simultaneous streams and hotplug stress remain unverified. See board HARDWARE-VALIDATION.md for exact evidence. Earlier full-speed-only restriction was superseded by this change.
