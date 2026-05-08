# Send print job — layout stub

Standalone stub to iterate on the **Send print job** plate-changer table layout without building OrcaSlicer or running a real slice.

## Placeholders

- **Top:** Thumbnail placeholder (grey box).
- **Middle:** Left column = project name (“clip plates”), “Printing 3 plates.”; right = table with header (Plate, Time, Weight), horizontal separator lines, “All” row, then plate rows 1–3 with dummy data.
- **Bottom:** Printer label and value placeholders.

Colours and spacing mirror the real dialog (dark background, grey text, header/“All” brighter). You can move the stub to `tests/sandboxes/send_dialog_layout` if you prefer it there.

## Build and run

Uses **system wxWidgets** (no OrcaSlicer deps).

```bash
cd send_dialog_layout_stub
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
./send_dialog_layout    # or send_dialog_layout.exe on Windows
```

**macOS (Homebrew wxWidgets):**

```bash
cmake .. -DCMAKE_PREFIX_PATH=$(brew --prefix wxwidgets)
cmake --build .
./send_dialog_layout
```

**Linux:** Install `libwxgtk3.0-dev` (or 3.2) then run the cmake/build steps above.

## Layout changes

Edit `main.cpp` to change sizers, alignment, DIP values, or colours. Rebuild and run to see the result. When satisfied, port the same changes into `src/slic3r/GUI/SendToPrinter.cpp` (and optionally `SelectMachine.cpp`).
