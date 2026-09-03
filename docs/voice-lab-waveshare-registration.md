# Voice Lab Waveshare Registration

This note records the ESP-side Voice Lab registration baseline for the
Waveshare ESP32-S3-AUDIO-Board.

## Current Device Record

The device has been registered in the Voice Lab console at:

```text
https://voice-lab.cloud/
```

Use this identity consistently on the server and in firmware:

```text
deviceKey: waveshare-esp32s3-audio-288485b2adec
device name: Waveshare ESP32-S3 Audio Board
hardwareProfile: waveshare-esp32s3-audio-board
```

The server currently shows the device as waiting for secure pairing. That is
expected until the firmware exchanges an enrollment code for its own device
credential.

Do not reuse any reSpeaker/XVF3800 device identity for this board.

## Firmware Defaults

Voice Lab defaults are defined in `main/Kconfig.projbuild` and have compile-time
fallbacks in `main/voice_lab_client.cc`.

Production endpoint:

```text
host: voice-lab.cloud
port: 1883
tls: true
control: /api/v1/devices/{deviceKey}/control
audio: /api/v1/devices/{deviceKey}/audio
```

The firmware chooses `ws://` or `wss://` only from the `tls` setting. It does not
guess the protocol from the port.

Explicit IP debugging can be configured at runtime if needed:

```text
host: 106.55.21.5
port: 18100
tls: false
```

Do not use IP addresses with TLS certificate validation, and do not bypass
certificate errors with insecure TLS settings.

## Runtime Settings

The `voice_lab` NVS namespace stores:

```text
server_host
server_port
server_tls
device_key
hw_profile
audio_frontend
enrollment_code
device_token
auto_connect
```

The old `server_url` key is treated as legacy and is erased by
`self.voice_lab.configure`.

The firmware does not print or return the long-term `device_token` in
`self.voice_lab.get_status`. Status only reports whether pairing has completed
and whether a short enrollment code is still stored.

## Pairing Flow

After flashing firmware and connecting Wi-Fi:

1. Open the device detail page in `https://voice-lab.cloud/`.
2. Click `生成设备配对码`.
3. Send the short code to the device with `self.voice_lab.pair`.
4. The firmware calls:

```http
POST /api/v1/devices/waveshare-esp32s3-audio-288485b2adec/enroll
Content-Type: application/json

{"code":"XXXX-XXXX"}
```

5. The server returns a one-time `vld_...` device credential.
6. The firmware stores that credential in NVS as `device_token`.
7. The firmware deletes `enrollment_code` from NVS.
8. The control WebSocket reconnects using `X-Device-Token`.

The enrollment code is short-lived and one-time use. Generate it only when the
firmware is ready to pair.

Never write an enrollment code or device token into source code, Git, logs,
URLs, screenshots, or long-lived documentation.

## MCP Tools

`self.voice_lab.configure` updates the endpoint, device identity, hardware
identity, optional enrollment code, and auto-connect behavior:

```json
{
  "server_host": "voice-lab.cloud",
  "server_port": 1883,
  "server_tls": true,
  "device_key": "waveshare-esp32s3-audio-288485b2adec",
  "hardware_profile": "waveshare-esp32s3-audio-board",
  "audio_frontend": "waveshare-es7210",
  "enrollment_code": "",
  "auto_connect": true
}
```

If the device key changes, or a new enrollment code is provided, the firmware
clears any old stored device credential so it cannot connect as another device.

`self.voice_lab.pair` exchanges a newly generated short code for a long-term
device credential:

```json
{
  "enrollment_code": "XXXX-XXXX"
}
```

`self.voice_lab.get_status` reports connection state, endpoint settings, device
identity, hardware identity, audio format, `paired`, and `has_enrollment_code`.
It does not expose the credential itself.

## Hello Report

After pairing, the control socket sends `hello` with at least:

```text
deviceKey
deviceId
clientId
firmware
firmwareBuild
firmwareSha256
sourceCommit
hardwareProfile
boardName
capabilitySchemaVersion
controlCapabilities
audioFrontend
audioFrontendFirmware
audioFormat
rssi
wifiChannel
localIp
serverHost
serverPort
serverTls
freeHeapBytes
psramFound
psramTotalBytes
freePsramBytes
```

Current audio upload format is `device-audio-v2` with `pcm_s16le`, 16 kHz,
mono, 20 ms frames. Multi-channel capture is mixed down before upload.

## Acceptance Checklist

After pairing and reconnecting, the Voice Lab device detail should show:

```text
admissionState: paired
canConnect: true
controlOnline: true
configurationInSync: true
contractState: compatible
```

Then run a real test from the console and verify:

```text
Test Run completed
audio saved by server
transcript exists
transcript belongs to waveshare-esp32s3-audio-288485b2adec
device reconnects after Voice Lab server restart
device reconnects after short Wi-Fi interruption
no Wi-Fi re-entry required
no re-enrollment required
```

## Build Note

Build is intentionally left to the operator for this step. The expected board is:

```powershell
py scripts\build.py waveshare/esp32-s3-audio-board --language zh-CN --wake-word disabled
```

Load the ESP-IDF PowerShell profile first if `idf.py` is not available in the
current shell.
