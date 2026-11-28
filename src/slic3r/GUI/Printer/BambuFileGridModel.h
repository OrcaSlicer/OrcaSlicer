#pragma once

#include "IFileGridModel.h"
#include "PrinterFileSystem.h"

#include <boost/smart_ptr/shared_ptr.hpp>
#include <wx/event.h>
#include <cstddef>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// IFileGridModel over PrinterFileSystem, behavior-identical to ImageGrid's direct use.
class BambuFileGridModel : public wxEvtHandler, public IFileGridModel
{
public:
    explicit BambuFileGridModel(boost::shared_ptr<PrinterFileSystem> fs);
    ~BambuFileGridModel() override;

    size_t GetCount() const override;
    const FileGridCard& GetFile(size_t index, bool& selected) const override;
    const FileGridCard& GetFile(size_t index) const override;

    FileGridType  GetFileType() const override;
    void          SetFileType(FileGridType type, const std::string& storage) override;
    FileGridGroup GetGroupMode() const override;
    void          SetGroupMode(FileGridGroup mode) override;

    size_t GetIndexAtTime(time_t time) const override;
    size_t EnterSubGroup(size_t index) override;

    void ToggleSelect(size_t index) override;
    void SelectAll(bool select) override;
    void SetFocusRange(size_t start, size_t count) override;

    void Retry() override;
    int  GetLastError() const override;
    void DownloadCheckFiles(const std::string& path) override;

    void SetChangeHandler(std::function<void(FileGridChange)> handler) override;

private:
    void EnsureCards() const;
    void notify(FileGridChange change);

    void onFileChanged(wxCommandEvent& evt);
    void onModeChanged(wxCommandEvent& evt);
    void onThumbnail(wxCommandEvent& evt);
    void onDownload(wxCommandEvent& evt);

    boost::shared_ptr<PrinterFileSystem> m_fs;
    std::function<void(FileGridChange)>  m_handler;
    mutable std::vector<FileGridCard>    m_cards;
    mutable bool                         m_dirty = true;
};

}}
