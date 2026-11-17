#pragma once

#include <string>
#include <wx/file.h>
#include <wx/grid.h>
#include <wx/strconv.h>
#include <wx/string.h>

// Steps of the curve editor dialog that need no application, so the GUI tests use them without the dialog.
namespace Slic3r::GUI::CurveEditorUtils {

// Escape first abandons only the active cell edit; other dialog changes remain intact.
inline bool cancel_cell_edit(wxGrid& grid)
{
    if (!grid.IsCellEditControlEnabled()) return false;
    auto* editor = grid.GetCellEditor(grid.GetGridCursorRow(), grid.GetGridCursorCol());
    editor->Reset();
    editor->DecRef();
    grid.DisableCellEditControl();
    grid.SetFocus();
    return true;
}

// The file steps of CSV import and export, apart from the dialogs that choose the path.
inline bool read_csv_file(const wxString& path, std::string& text)
{
    wxFile file(path);
    wxString content;
    if (!file.IsOpened() || !file.ReadAll(&content, wxConvUTF8)) return false;
    text = content.utf8_string();
    return true;
}

// Commit a complete temporary file so a failed write preserves an existing CSV.
inline bool write_csv_file(const wxString& path, const std::string& text)
{
    wxTempFile file(path);
    return file.IsOpened() && file.Write(wxString::FromUTF8(text), wxConvUTF8) && file.Commit();
}

} // namespace Slic3r::GUI::CurveEditorUtils
