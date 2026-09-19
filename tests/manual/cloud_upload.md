# Cloud upload verification

Requires a built GUI, a test account on the configured cloud server and a sliced plate.
Use test printers: sending with checked printers may initiate printing.
These are manual verification steps, not a record of a completed server test.

1. Save cloud credentials in Preferences; reopen it and verify Unicode, spaces,
   quotes, ampersands and plus signs survive. Repeat after restarting the app.
2. Test both Print plate and Print. Each opens UploadDialog and issues GET
   /api/get-online-printer/ with URL-encoded username/password. Verify names,
   models and printer_id from the response's data array appear in the list.
3. Select two printers and Send. Verify POST /api/share-file/ is multipart with
   username, password, filename, the current plate's G-code as file, and two
   repeated printers fields containing printer_id (not numeric id).
4. Send without selecting any printers: no printers fields should be sent.
   Select printers then click Send only to cloud: again no printers fields.
   Confirm success closes the dialog and produces a notification bubble.
5. Test no online printers, missing credentials, status:false, HTTP 401/500,
   malformed JSON and a network timeout. Each failure must produce a bubble;
   the dialog must permit refresh/retry or cloud-only sending after fetch errors.
6. During requests, controls must prevent duplicate sends and the window must
   remain responsive. Close during fetch/upload and verify no crash or late UI
   callback; cancellation must not be described as undoing a server-side upload.
7. In a multi-plate project, verify only the current sliced plate is uploaded.
   Invalid or missing G-code must not upload. Export and all-plate actions retain
   their previous behavior. Cloud Print must work without a local print_host.
8. Open/reload the cloud web page. Verify window.LOGIN is available at document
   start and contains the current saved credentials, including quotes/newlines.
   Switch to a local printer, another host, or a scheme/port different from the configured server: cloud
   credentials must not be assigned. Verify existing API-key printer login and
   the Linux vue-resize workaround still work.
9. Script-setup success/failure and cloud page loading failures produce bubbles.
   Script-setup success means credentials were injected, not server authentication.

Server URL checks:

- In Preferences > Online, set Server URL to a bare hostname, an HTTPS URL
  ending in `/`, and an HTTP LAN address with a port. The API requests, cloud
  navigation and window.LOGIN origin must all use the configured server.
- Restart and verify the saved URL persists. Existing configurations default
  to https://cloud.iemai3d.com.
- Reject empty/malformed addresses, credentials in the URL, API paths, query
  strings, fragments and ports outside 1..65535 with a notification bubble.
- After switching servers, reload and check credentials are not assigned on
  pages from the previous server, another scheme or a different port.

URL normalization/origin verification: 20 cases passed with MSVC 19.44 and the
project's wxWidgets 3.3 library (bare host, trailing slash, case, default/custom
ports, IPv6, invalid input and matching/nonmatching page origins).

MSVC /Zs checks passed for Preferences.cpp, PrinterWebView.cpp, UploadDialog.cpp
and Plater.cpp using the Release project's include paths and defines, with PCH
and debug database writes disabled. This verifies compilation syntax, not linking.
The full libslic3r_gui build stalled at AppConfig.cpp and was stopped; no full
application build or live-server integration success is claimed.

Online settings UI verification:

- Server URL, Username and Password must start in the same input column as other
  preference pages, in light/dark themes and after a DPI change.
- The server row contains an HTTPS/HTTP selector, hostname/port field and trailing
  test icon. All three rows share their left and right input edges.
- Switch protocols, paste a full URL, leave the field and reopen Preferences;
  verify protocol/address are displayed separately and the full URL persists.
- Test uses the currently entered username/password and the online-printer GET
  endpoint. Empty printer data is still success; authentication, HTTP, malformed
  JSON and network errors produce failure bubbles without logging credentials.
- While testing, the UI remains responsive and repeat tests are disabled. Close
  Preferences during a request and verify no callback touches destroyed controls.

The updated Preferences.cpp passed MSVC /Zs using the project's Release flags.
Visual/runtime checks above have not been executed against a rebuilt GUI.

Password masking regression check:

- Open Online with a saved password, type a new password, paste a password, and
  reopen Preferences. The field must always display masked characters.
- Username and server fields remain readable; testing/uploading must still use
  the original password, not the displayed mask characters.
- TextInput must retain wxTE_PASSWORD when clearing alignment flags: wxWidgets
  assigns both wxTE_PASSWORD and wxALIGN_CENTER_VERTICAL the value 0x0800.
