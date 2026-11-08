# Vendor send dialogs

When the selected printer comes from a vendor that has its own slicer app, **Print** opens a send dialog laid out like that app's, so sending a job feels the same as it does there.

| Vendor | Dialog modelled on | Printers listed | Filament mapping |
|---|---|---|---|
| Bambu Lab | Bambu Studio (`SelectMachineDialog`, unchanged) | Bambu cloud / LAN | AMS |
| Elegoo | ElegooSlicer's print-send page | Elegoo cloud printers of the signed-in account, plus the LAN printer set in the preset | Canvas |
| Creality | Creality Print's send dialog | LAN printer set in the preset, plus the printers of the signed-in Creality Cloud account | CFS ("Enable CFS") |
| Markforged | Eiger's print panel | Eiger organization printers (state, material, queue) | none (material comes from the Eiger settings preset) |

## Pieces

- `GUI/VendorSendDialog.*`: `current_send_vendor()` (vendor of the selected preset), the backend interface `VendorSendBackend`, the backend registry, and one dialog class that takes each vendor's colours and layout. The dialog only collects choices: printer, file name, options, slot mapping.
- `GUI/VendorSendBackends.cpp`: the Elegoo, Creality and Markforged backends. A backend lists printers and slots (on a worker thread), declares its options, and turns the choices into a `PrintHostJob`.
- `Plater::send_to_vendor()`: builds the job summary (thumbnail, time, weight, layers, plate filaments), shows the dialog, then exports the G-code and runs the job through the regular upload queue, so progress and errors appear in the upload queue dialog like any other host upload. It returns false when the vendor has no backend, and the caller falls back to the regular flow.
- `Utils/ElegooCloud.*`: native Elegoo cloud client and `ElegooCloudHost`, the `PrintHost` behind cloud jobs.
- `GUI/ElegooLoginDialog.*`: Elegoo account sign-in.
- `Utils/MarkforgedEiger.*`: Eiger REST client and `MarkforgedEigerHost`.
- Creality Cloud jobs use `CrealityCloudHost` (in `VendorSendBackends.cpp`) over the `Utils/CrealityCloud` client: upload, the self-test flag, then `addSingleTask` with the CFS mapping. The session is the one the Creality Cloud account agent keeps (`CrealityCloud::SessionStore`).

Other vendors plug in with `register_vendor_send_backend(SendVendor::X, factory)`. A factory returns null to leave a printer on the regular flow.

## Elegoo cloud

The protocol follows Elegoo's open-source elegoo-link SDK.

1. REST on `matrix.elegoo.com` (`matrix.elegoo.com.cn` for China) with a Bearer token: device list, last status report, Agora token, object-storage upload slot. Tokens are refreshed with the refresh token.
2. The G-code is `PUT` to the storage URL the API hands out.
3. Over Agora RTM 2.x, the app messages the user channel `<agoraUserId><serial>`: 1057 makes the printer download the file (progress arrives as 6006), 2004 sets auto refill, 1020 starts the print with plate side, bed leveling, timelapse and the Canvas `slot_map`. 2005 reads the Canvas slots for the mapping UI.

Agora RTM is a proprietary runtime. It is never bundled: it is loaded at run time from an ElegooSlicer install, or downloaded from the elegoo-link repository into `<data_dir>/elegoo_cloud/agora`. It is called through its C++ ABI (vtable indices and struct layouts of RTM 2.2.8) because no SDK headers are vendored.

Sign-in hosts the Elegoo account site's slicer login page, which reports the tokens back over the WebView bridge (`report.userInfo`). On Windows the dialog can also copy ElegooSlicer's saved sign-in. The account is stored in `<data_dir>/elegoo_cloud/account.dat`, encrypted with DPAPI on Windows and readable only by the user elsewhere.

The cloud allows one messaging session per account, so RTM sessions are serialized with a mutex.

## Markforged Eiger

Eiger slices on its own servers and accepts STL only, so a Markforged job carries no G-code. The backend writes the current plate's printable instances, placed as on the plate, to one binary STL and marks the job `vendor_source_ready`, which makes the plater queue it without exporting G-code. `MarkforgedEigerHost` then uploads the STL with the chosen settings preset (`POST /parts/upload_stl`), polls the slice job until it completes, and looks up the build Eiger made for the part (the API cannot create builds). It starts the build (`POST /devices/{id}`), queues it, or sends it to the backlog. When the printer is not Ready, "Print now" queues instead. When Eiger made no build, the job fails with a message and Eiger opens in the browser.

Keys come from the printer preset (`printhost_user` / `printhost_password`, or `printhost_apikey` as `ACCESS:SECRET`), else from `<data_dir>/orca_plugins/markforged_eiger_data/settings.json`, which the Markforged Device tab agent shares. Signing in from the dialog writes there.
