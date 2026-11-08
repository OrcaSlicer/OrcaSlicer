# Creality Cloud printing

Prints to Creality printers (K1, K2, Hi, Ender-3 V3 and other models on current firmware) that are bound to the user's Creality Cloud account, over the internet. LAN printing to the same printers is a separate path: `CrealityPrint` (print host) and `CrealityPrintAgent` (port 9999 WebSocket and CFS sync).

## Parts

| Part | Role |
| --- | --- |
| `Utils/CrealityCloud.{hpp,cpp}` | HTTP client and pure helpers: request bodies, reply parsers, OSS signing, the session store, and translation of device records into the ssdp and `push_status` messages the device panels read. No GUI dependencies, so it is unit tested in `tests/slic3rutils/test_creality_cloud.cpp`. |
| `Utils/CrealityCloudServiceAgent` | `ICloudServiceAgent` for provider `creality`. Signs in and out only; every other cloud service is unsupported. |
| `Utils/CrealityCloudPrinterAgent` | `IPrinterAgent` with id `creality_cloud`, registered with cloud provider `creality`. Lists the account's printers, polls the connected one, uploads and starts prints, and maps pause, resume, stop, G-code and temperature commands to RPC calls. |
| `resources/web/creality/login.html` | Sign-in page the shared login dialog loads from `get_cloud_login_url()`. |

The account agent is always created, whatever `cloud_providers` the app config lists, because the Device tab offers Creality as a built-in service.

## Session

`CrealityCloud::SessionStore` holds the signed-in user (user id, token, nickname, region) for the whole process and keeps it in `<data_dir>/creality_cloud.json`. The printer agent reads the store instead of its cloud agent pointer, since the Device tab builds its own printer agent instances. The token has no refresh flow; when the API answers code 4 the user signs in again. The password is never stored.

Sign-in posts `{"command":"user_login","data":{account, password, account_type, region}}` from the page; the dialog closes and `change_user()` calls `loginV2` on the UI thread, then shows a message box if it fails, because the dialog is already gone.

## Protocol

Taken from Creality Print 6 (AGPL-3.0), which is what the printers' cloud firmware is built to talk to. Every request carries the `__CXY_*` headers from `base_headers()`; signed-in requests add `__CXY_UID_` and `__CXY_TOKEN_`. Hosts are `api.crealitycloud.com` (Global) and `api.crealitycloud.cn` (China), picked by the region chosen at sign-in.

1. **Devices.** `POST /api/rest/print/cluster/devices/getDevices` pages through the account's printers. Types 1 and 9 are cloud printers. Each has a `deviceName` (dn), used by the device list and print tasks, and a `tbId`, used by the RPC and telemetry endpoints. `deviceState` -1 is offline.
2. **Upload.** `getAliyunInfo` returns temporary STS credentials and `getOssInfo` returns the endpoint and bucket. The G-code is gzipped and sent with a single signed OSS `PUT` to `model/slice/<MD5(name ":" md5(file))>.gcode.gz`, the key Creality Print uses. `gcode/uploadGcode` then registers it and returns its id.
3. **Start.** `POST /api/rest/print/cluster/addSingleTask` with the dn, the upload id and `https://file-cdn.creality.com/<key>`. The printer downloads the file itself. On a printer with a CFS, `enableCfs` and `filamentsList` map each G-code extruder (`T1A`, `T1B`, ...) to a CFS slot, which is named the same way (`T<box><slot letter>`, boxes from 1). The send dialog's `ams_mapping` tray index `t` becomes slot `T(t/4+1)(A+t%4)`.
4. **Control and status.** `POST /api/rest/iotrouter/rpc/twoway/<tbId>` with `{"method":"set","params":{...}}`: `pause` 1/0, `stop` 1, `gcodeCmd`, `bedTempControl`, `nozzleTempControl`, and `enableSelfTest`, which is sent right before a task. `GET /api/rest/iotrouter/<tbId>/values/timeseries` returns telemetry (`state`, `printProgress`, `printLeftTime`, `nozzleTemp`, `bedTemp0`, ...). Telemetry `state` is 0 idle, 1 printing, 2 finished, 3 failed, 4 stopped, 5 paused.

"Send" without printing (`start_send_gcode_to_sdcard`) uploads to the account's cloud G-code library only.

## Constraints

- Discovery polls every 15 s and the connected printer every 5 s, through the same REST endpoints. Creality Print also listens on an MQTT feed for status, which this client does not use.
- Camera streaming (WebRTC through the cloud) is not supported.
- Accounts created through Google, Apple or a phone code have no password and must set one in the Creality Cloud app before signing in here.
