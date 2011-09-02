#include "FileArchiveDialog.hpp"

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "ExtraRenderers.hpp"
#include "format.hpp"
#include <regex>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r {
namespace GUI {

#define BTN_SIZE                wxSize(FromDIP(58), FromDIP(24))
#define BTN_GAP                 FromDIP(20)

ArchiveViewModel::ArchiveViewModel(wxWindow* parent)
    :m_parent(parent)
{}
ArchiveViewModel::~ArchiveViewModel()
{}

std::shared_ptr<ArchiveViewNode> ArchiveViewModel::AddFile(std::shared_ptr<ArchiveViewNode> parent, const wxString& name,  bool container)
{
    std::shared_ptr<ArchiveViewNode> node = std::make_shared<ArchiveViewNode>(ArchiveViewNode(name));
    node->set_container(container);
    
    if (parent.get() != nullptr) {
        parent->get_children().push_back(node);
        node->set_parent(parent);
        parent->set_is_folder(true);
    } else {
        m_top_children.emplace_back(node);
    }
     
    wxDataViewItem child = wxDataViewItem((void*)node.get());
    wxDataViewItem parent_item= wxDataViewItem((void*)parent.get());
    ItemAdded(parent_item, child);
    
    if (parent)
        m_ctrl->Expand(parent_item);
    return node;
}

wxString ArchiveViewModel::GetColumnType(unsigned int col) const 
{
    if (col == 0)
        return "bool";
    return "string";//"DataViewBitmapText";
}

void ArchiveViewModel::Rescale()
{
    // There should be no pictures rendered
}

void  ArchiveViewModel::Delete(const wxDataViewItem& item)
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    assert(node->get_parent() != nullptr);
    for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
    {
        Delete(wxDataViewItem((void*)child.get()));
    }
    delete [] node;
}
void  ArchiveViewModel::Clear()
{
}

wxDataViewItem ArchiveViewModel::GetParent(const wxDataViewItem& item) const 
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    return wxDataViewItem((void*)node->get_parent().get());
}
unsigned int ArchiveViewModel::GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const
{
    if (!parent.IsOk()) {
        for (std::shared_ptr<ArchiveViewNode>child : m_top_children) {
            array.push_back(wxDataViewItem((void*)child.get()));
        }
        return m_top_children.size();
    }
       
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(parent.GetID());
    for (std::shared_ptr<ArchiveViewNode> child : node->get_children()) {
        array.push_back(wxDataViewItem((void*)child.get()));
    }
    return node->get_children().size();
}

void ArchiveViewModel::GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const 
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    if (col == 0) {
        variant = node->get_toggle();
    } else {
        variant = node->get_name();
    }
}

void ArchiveViewModel::untoggle_folders(const wxDataViewItem& item)
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    node->set_toggle(false);
    if (node->get_parent().get() != nullptr)
        untoggle_folders(wxDataViewItem((void*)node->get_parent().get()));
}

bool ArchiveViewModel::SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col) 
{
    assert(item.IsOk());
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    if (col == 0) {
        node->set_toggle(variant.GetBool());
        // if folder recursivelly check all children
        for (std::shared_ptr<ArchiveViewNode> child : node->get_children()) {
            SetValue(variant, wxDataViewItem((void*)child.get()), col);
        }
        if(!variant.GetBool() && node->get_parent())
            untoggle_folders(wxDataViewItem((void*)node->get_parent().get()));
    } else {
        node->set_name(variant.GetString());
    }
    m_parent->Refresh();
    return true;
}
bool ArchiveViewModel::IsEnabled(const wxDataViewItem& item, unsigned int col) const 
{
    // As of now, all items are always enabled.
    // Returning false for col 1 would gray out text.
    return true;
}

bool ArchiveViewModel::IsContainer(const wxDataViewItem& item) const 
{
    if(!item.IsOk())
        return true;
    ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
    return node->is_container();
}

ArchiveViewCtrl::ArchiveViewCtrl(wxWindow* parent, wxSize size)
    : wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, size, wxDV_VARIABLE_LINE_HEIGHT | wxDV_ROW_LINES
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    )
    //, m_em_unit(em_unit(parent))
{
    wxGetApp().UpdateDVCDarkUI(this);

    m_model = new ArchiveViewModel(parent);
    this->AssociateModel(m_model);
    m_model->SetAssociatedControl(this);
}

ArchiveViewCtrl::~ArchiveViewCtrl()
{
    if (m_model) {
        m_model->Clear();
        m_model->DecRef();
    }
}

FileArchiveDialog::FileArchiveDialog(wxWindow* parent_window, mz_zip_archive* archive, std::vector<std::pair<boost::filesystem::path, size_t>>& selected_paths_w_size)
    : DPIDialog(parent_window, wxID_ANY, _(L("Archive preview")), wxDefaultPosition,
        wxSize(45 * wxGetApp().em_unit(), 40 * wxGetApp().em_unit()),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , m_selected_paths_w_size (selected_paths_w_size)
{
#ifdef _WIN32
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDlgDarkUI(this);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif

    int em = em_unit();

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    m_avc = new ArchiveViewCtrl(this, wxSize(45 * em, 30 * em));
    wxDataViewColumn*  toggle_column = m_avc->AppendToggleColumn(L"\u2714", 0, wxDATAVIEW_CELL_ACTIVATABLE, 6 * em);
    m_avc->AppendTextColumn("filename", 1);
    
    std::vector<std::shared_ptr<ArchiveViewNode>> stack;

    std::function<void(std::vector<std::shared_ptr<ArchiveViewNode> >&, size_t)> reduce_stack = [] (std::vector<std::shared_ptr<ArchiveViewNode>>& stack, size_t size) {
        if (size == 0) {
            stack.clear();
            return;
        }
        while (stack.size() > size)
            stack.pop_back();
    };
    // recursively stores whole structure of file onto function stack and synchoronize with stack object.
    std::function<size_t(const boost::filesystem::path&, std::vector<std::shared_ptr<ArchiveViewNode>>&)> adjust_stack = [&adjust_stack, &reduce_stack, &avc = m_avc](const boost::filesystem::path& const_file, std::vector<std::shared_ptr<ArchiveViewNode>>& stack)->size_t {
        boost::filesystem::path file(const_file);
        size_t struct_size = file.has_parent_path() ? adjust_stack(file.parent_path(), stack) : 0;

        if (stack.size() > struct_size && (file.has_extension() || file.filename().string() != stack[struct_size]->get_name()))
        {
            reduce_stack(stack, struct_size);
        }
        if (!file.has_extension() && stack.size() == struct_size)
            stack.push_back(avc->get_model()->AddFile((stack.empty() ? std::shared_ptr<ArchiveViewNode>(nullptr) : stack.back()), boost::nowide::widen(file.filename().string()), true)); // filename string to wstring?
        return struct_size + 1;
    };

    const std::regex pattern_drop(".*[.](stl|obj|amf|3mf|step|stp)", std::regex::icase);
    mz_uint num_entries = mz_zip_reader_get_num_files(archive);
    mz_zip_archive_file_stat stat;
    std::vector<std::pair<boost::filesystem::path, size_t>> filtered_entries; // second is unzipped size
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (mz_zip_reader_file_stat(archive, i, &stat)) {
            std::string extra(1024, 0);
            boost::filesystem::path path;
            size_t extra_size = mz_zip_reader_get_filename_from_extra(archive, i, extra.data(), extra.size());
            if (extra_size > 0) {
                path = boost::filesystem::path(extra.substr(0, extra_size));
            } else {
                wxString wname = boost::nowide::widen(stat.m_filename);
                std::string name = into_u8(wname);
                path = boost::filesystem::path(name);
            }
            assert(!path.empty());
            if (!path.has_extension())
                continue;
            // filter out MACOS specific hidden files
            if (boost::algorithm::starts_with(path.string(), "__MACOSX"))
                continue;
            filtered_entries.emplace_back(std::move(path), stat.m_uncomp_size);
        }
    }
    // sorting files will help adjust_stack function to not create multiple same folders
    std::sort(filtered_entries.begin(), filtered_entries.end(), [](const std::pair<boost::filesystem::path, size_t>& p1, const std::pair<boost::filesystem::path, size_t>& p2){ return p1.first.string() < p2.first.string(); });
    size_t entry_count = 0;
    size_t depth = 1;
    for (const auto& entry : filtered_entries)
    {
        const boost::filesystem::path& path = entry.first;
        std::shared_ptr<ArchiveViewNode> parent(nullptr);

        depth = std::max(depth, adjust_stack(path, stack));
        if (!stack.empty())
            parent = stack.back();
        if (std::regex_match(path.extension().string(), pattern_drop)) { // this leaves out non-compatible files 
            std::shared_ptr<ArchiveViewNode> new_node = m_avc->get_model()->AddFile(parent, boost::nowide::widen(path.filename().string()), false);
            new_node->set_fullpath(/*std::move(path)*/path); // filename string to wstring?
            new_node->set_size(entry.second);
            entry_count++;
        }
    }
    if (entry_count == 1)
        on_all_button();

    toggle_column->SetWidth((4 + depth) * em);

    wxBoxSizer* btn_sizer = create_btn_sizer();

    topSizer->Add(m_avc, 1, wxEXPAND | wxALL, 10);
    topSizer->Add(btn_sizer, 0, wxEXPAND | wxALL, 10);
    this->SetSizer(topSizer);
    SetMinSize(wxSize(40 * em, 30 * em));

    for (auto btn : m_button_list)
        wxGetApp().UpdateDarkUI(btn);
}

void FileArchiveDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    int em = em_unit();
    BOOST_LOG_TRIVIAL(error) << "on_dpi_changed";

    for (auto btn : m_button_list) {
        btn->SetMinSize(BTN_SIZE);
        btn->SetCornerRadius(FromDIP(12));
    }

    const wxSize& size = wxSize(45 * em, 40 * em);
    SetSize(size);
    //m_tree->Rescale(em);

    Fit();
    Refresh();
}

void FileArchiveDialog::on_open_button()
{
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);
    
    std::function<void(ArchiveViewNode*)> deep_fill = [&paths = m_selected_paths_w_size, &deep_fill](ArchiveViewNode* node){
        if (node == nullptr)
            return;
        if (node->get_children().empty()) {
            if (node->get_toggle()) 
                paths.emplace_back(node->get_fullpath(), node->get_size());
        } else { 
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
    {
        ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
        deep_fill(node);
    }
    this->EndModal(wxID_OK);
}

void FileArchiveDialog::on_all_button()
{
    
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);

    std::function<void(ArchiveViewNode*)> deep_fill = [&deep_fill](ArchiveViewNode* node) {
        if (node == nullptr)
            return;
        node->set_toggle(true);
        if (!node->get_children().empty()) {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
    {
        ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
        deep_fill(node);
        // Fix for linux, where Refresh or Update wont help to redraw toggle checkboxes.
        // It should be enough to call ValueChanged for top items. 
        m_avc->get_model()->ValueChanged(item, 0);
    }

    Refresh();
}

void FileArchiveDialog::on_none_button()
{
    wxDataViewItemArray top_items;
    m_avc->get_model()->GetChildren(wxDataViewItem(nullptr), top_items);

    std::function<void(ArchiveViewNode*)> deep_fill = [&deep_fill](ArchiveViewNode* node) {
        if (node == nullptr)
            return;
        node->set_toggle(false);
        if (!node->get_children().empty()) {
            for (std::shared_ptr<ArchiveViewNode> child : node->get_children())
                deep_fill(child.get());
        }
    };

    for (const auto& item : top_items)
    {
        ArchiveViewNode* node = static_cast<ArchiveViewNode*>(item.GetID());
        deep_fill(node);
        // Fix for linux, where Refresh or Update wont help to redraw toggle checkboxes.
        // It should be enough to call ValueChanged for top items. 
        m_avc->get_model()->ValueChanged(item, 0);
    }

    this->Refresh();
}

//Orca: Apply buttons style
wxBoxSizer* FileArchiveDialog::create_btn_sizer()
{
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    Button* all_btn = new Button(this, _L("All"));
    all_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_all_button(); });
    btn_sizer->Add(all_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_button_list.push_back(all_btn);

    Button* none_btn = new Button(this, _L("None"));
    none_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    none_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_none_button(); });
    btn_sizer->Add(none_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::WindowButtonGap()));
    m_button_list.push_back(none_btn);

    btn_sizer->AddStretchSpacer();

    Button* open_btn = new Button(this, _L("Open"));
    open_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    open_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { on_open_button(); });
    open_btn->SetFocus();
    open_btn->SetId(wxID_OK);
    btn_sizer->Add(open_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
    m_button_list.push_back(open_btn);

    Button* cancel_btn = new Button(this, _L("Cancel"));
    cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) { this->EndModal(wxID_CANCEL); });
    cancel_btn->SetId(wxID_CANCEL);
    btn_sizer->Add(cancel_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_button_list.push_back(cancel_btn);

    return btn_sizer;
}

} // namespace GUI
} // namespace Slic3r