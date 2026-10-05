#pragma once

#include <wx/bitmap.h>
#include <cstddef>
#include <ctime>
#include <functional>
#include <string>

namespace Slic3r {
namespace GUI {

// Mirrors PrinterFileSystem::FileType ordering (F_TIMELAPSE=0, F_VIDEO=1, F_MODEL=2).
enum class FileGridType { Timelapse = 0, Video = 1, Model = 2 };
// Mirrors PrinterFileSystem::GroupMode ordering (G_NONE=0, G_MONTH=1, G_YEAR=2).
enum class FileGridGroup { All = 0, Month = 1, Year = 2 };
// Mirrors the PrinterFileSystem wx events ImageGrid listens to.
enum class FileGridChange { Files, Mode, Thumbnail, Download };

// One rendered card. Neutral: no Bambu transfer internals, no filesystem paths beyond id.
struct FileGridCard {
    std::string id;
    std::string name;
    std::string title;
    std::string time_text;
    std::string weight_text;
    time_t      time = 0;
    bool        downloading = false;
    int         download_progress = 0; // -1 waiting, <0 failed, 0..100 progress
    bool        selected = false;
    bool        printable = true;
    wxBitmap    thumbnail;
};

// Data source behind ImageGrid. Implemented today by BambuFileGridModel (wrapping
// PrinterFileSystem); a non-Bambu model can implement the same surface later.
class IFileGridModel {
public:
    virtual ~IFileGridModel() = default;

    virtual size_t GetCount() const = 0;
    virtual const FileGridCard& GetFile(size_t index, bool& selected) const = 0;
    virtual const FileGridCard& GetFile(size_t index) const = 0;

    virtual FileGridType  GetFileType() const = 0;
    virtual void          SetFileType(FileGridType type, const std::string& storage) = 0;
    virtual FileGridGroup GetGroupMode() const = 0;
    virtual void          SetGroupMode(FileGridGroup mode) = 0;

    // When true, the thumbnail is fitted inside the tile instead of stretched to fill it.
    virtual bool preserve_thumbnail_aspect() const { return false; }

    virtual size_t GetIndexAtTime(time_t time) const = 0;
    virtual size_t EnterSubGroup(size_t index) = 0;

    virtual void ToggleSelect(size_t index) = 0;
    virtual void SelectAll(bool select) = 0;
    virtual void SetFocusRange(size_t start, size_t count) = 0;

    virtual void Retry() = 0;
    virtual int  GetLastError() const = 0;
    virtual void DownloadCheckFiles(const std::string& path) = 0;

    // ImageGrid sets one handler; the model invokes it on the UI thread on change.
    virtual void SetChangeHandler(std::function<void(FileGridChange)>) = 0;
};

}}
