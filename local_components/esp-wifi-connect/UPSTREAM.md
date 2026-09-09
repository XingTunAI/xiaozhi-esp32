# Maintained Wi-Fi component for XingTun

This directory is project-owned source, selected by the `78/esp-wifi-connect`
`override_path` in `main/idf_component.yml`. Edit this directory; do not edit the
Component Manager output under `managed_components`.

The starting point is the locally installed `78/esp-wifi-connect` 3.2.2 package,
whose manifest identifies upstream commit
`c24b97c194e6b4a1d7be0237b3c28980661cac1e`:
https://github.com/78/esp-wifi-connect/tree/c24b97c194e6b4a1d7be0237b3c28980661cac1e

The installed package already contained project Voice Lab engineering form and
server-setting adaptations; these are retained for the legacy noncustomer mode.
Only five implementation files, five public headers, two HTML assets, CMake,
the original README and component manifest were copied (15 files; 174,437 bytes).
Screenshot images, generated hashes, checksums and build/cache files were omitted.
The original README's screenshot links therefore refer to upstream-only files.

The preserved upstream manifest declares `license: MIT`. Neither the installed
package nor the referenced upstream root contains a separate LICENSE file or
copyright notice. No attribution or licensing metadata has been removed or
invented; retain the manifest and source attribution when redistributing.

## Customer mode contract

`WifiManagerConfig::customer_mode` defaults to false. The Waveshare audio board
in Voice Lab standalone mode enables it and a 600-second configuration window.

- `/` serves the self-contained Chinese customer page. Only `/scan`, `/submit`,
  `/exit` and captive-portal redirects are available. Legacy engineering and
  saved-network management routes are not registered in customer mode.
- `/submit` accepts only `ssid` and `password`; additional fields return HTTP 400
  before Wi-Fi or NVS operations. The Voice Lab service/enrollment writer calls
  are also guarded by `!customer_mode_` as an independent boundary.
- Successful configuration writes only Wi-Fi credentials through SsidManager,
  retains previously saved networks as fallbacks and schedules AP shutdown after
  two seconds. A failed attempt keeps the page available for another attempt.
- `wifi_provision/ap_password` stores a device-specific 8-digit password
  generated with `esp_fill_random` and rejection sampling. Earlier 12-character
  passwords are migrated once to the spoken numeric format. It is separate from both `wifi` and
  `voice_lab` namespaces and is never derived from the MAC address or device key.
  WPA2-PSK protects the AP. Failure to load or persist the password prevents
  customer Wi-Fi initialization; it never falls back to an open AP.
- `GetApPassword()` is for the physical spoken prompt/internal provisioning only.
  It must not be logged or included in device status, HTTP responses or captures.
- Timeout emits `ConfigModeExpired`; WifiBoard closes the AP and retries saved
  networks without automatically opening repeated windows. A BOOT long press
  opens another 10-minute window. A later genuine Wi-Fi loss gets a 60-second
  reconnection attempt before opening a new bounded recovery window.
- Customer-mode NVS initialization does not erase the whole NVS partition when
  its format/storage needs maintenance. The main application's standalone boot
  path also preserves NVS and stops startup for internal maintenance in this case.

## Physical regression checks

Station events are dispatched through a 16-record fixed-size nonblocking queue.
ESP event handlers never lock WifiManager or invoke application callbacks, which
avoids deadlock against Wi-Fi handler unregistration during reconfiguration.
An outer recursive notification mutex serializes mode changes and callbacks,
while the manager mutex is released before invoking callbacks. Station generations
discard notifications from an earlier connection lifecycle. On critical queue
overflow, the producer's bounded connection snapshot is consumed by the worker:
it reports an interruption then the latest connected state, rather than silently
losing a disconnect. Shutdown stops event sources before joining the worker, and
joins without either manager/notification lock held.

Before delivery, run the affected board build and verify on the device:

1. With no saved Wi-Fi, the AP requires the spoken password and serves only the
   network form. Attempt `/advanced/config`, `/advanced/submit`, `/saved/delete`
   and `/done.html`: none may expose or mutate engineering settings.
2. Record only pass/fail comparisons of all `voice_lab` keys; never print their
   values. Correct and wrong-password submissions, then a crafted `/submit`
   containing `voice_lab_device_key`, enrollment, host, port, TLS or OTA fields
   must leave every identity/service value unchanged. Extra-field submissions
   must fail before replacing network credentials.
3. Retry a wrong password successfully. Verify normal 32-byte SSIDs, quoted or
   backslash-containing SSIDs, and multibyte SSIDs; scan JSON must remain valid.
4. Close the browser immediately after a successful submit. The AP must still
   close and reconnect to the saved network. Disconnect/reconnect power and
   verify the independent AP password and device identity persist.
5. Let a window expire with and without saved networks. The AP must stay closed
   while disconnected, and BOOT long press must reopen it with the same password.
6. Verify the password is announced only by the physical configuration prompt
   and does not appear in logs, HTTP responses, device status or screen capture.
7. Repeatedly trigger long-press reconfiguration during scan/connect/disconnect,
   including the 60-second fallback boundary. Neither the main loop nor the ESP
   event loop may freeze; the AP must open and close on every attempt. Exercise
   an intentionally stalled event consumer to force overflow and verify that an
   active recording is interrupted and the final connection state is restored.
