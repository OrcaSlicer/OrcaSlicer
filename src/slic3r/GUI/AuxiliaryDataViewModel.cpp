#include "AuxiliaryDataViewModel.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"


#include <boost/log/trivial.hpp>

#include <wx/log.h>

const static std::array<wxString, 4> s_default_folders = {
    _L("Model Pictures"),
    _L("Bill of Materials"),
    _L("Assembly Guide"),
    _L("Others")
};

AuxiliaryModel::AuxiliaryModel()
{
    m_root = nullptr;
}

void AuxiliaryModel::Init(wxString aux_path)
{
    m_root = new AuxiliaryModelNode();
    m_root_dir = aux_path;

    if (wxDirExists(m_root_dir)) {
        fs::path path_to_del(m_root_dir.ToStdWstring());
        try {
            fs::remove_all(path_to_del);
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
        }
    }

    fs::path top_dir_path(m_root_dir.ToStdWstring());
    fs::create_directory(top_dir_path);

    for (auto folder : s_default_folders)
        CreateFolder(folder);
}

AuxiliaryModel::~AuxiliaryModel()
{
    if (wxDirExists(m_root_dir)) {
        fs::path path_to_del(m_root_dir.ToStdWstring());
        try {
            fs::remove_all(path_to_del);
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
        }
        m_root_dir = "";
    }
    delete m_root;
    m_root = nullptr;
}

void AuxiliaryModel::Reload(wxString aux_path)
{
    fs::path new_aux_path(aux_path.ToStdWstring());

    // Clean
    try {
        fs::remove_all(fs::path(m_root_dir.ToStdWstring()));
    }
    catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
    }

    if (m_root) {
        delete m_root;
        m_root = nullptr;
    }
    Cleared();

    // Create new root.
    m_root = new AuxiliaryModelNode();
    m_root_dir = aux_path;

    // Check new path. If not exist, create a new one.
    if (!fs::exists(new_aux_path)) {
        fs::create_directory(new_aux_path);
        // Create default folders if they are not loaded
        wxDataViewItemArray default_items;
        for (auto folder : s_default_folders) {
            wxString folder_path = aux_path + "\\" + folder;
            if (fs::exists(folder_path.ToStdWstring())) continue;

            fs::create_directory(folder_path.ToStdWstring());
            AuxiliaryModelNode *node = new AuxiliaryModelNode(m_root,
                                                              folder_path,
                                                              true);
            default_items.Add(wxDataViewItem(node));
        }
        ItemsAdded(wxDataViewItem(nullptr), default_items);
        return;
    }

    // Load from new path
    std::map<fs::path, AuxiliaryModelNode *> dir_cache;
    fs::directory_iterator iter_end;
    wxDataViewItemArray items;
    for (fs::directory_iterator iter(new_aux_path); iter != iter_end; iter++) {
        wxString path = iter->path().generic_wstring();
        AuxiliaryModelNode* node = new AuxiliaryModelNode(m_root, path, fs::is_directory(iter->path()));
        items.Add(wxDataViewItem(node));

        if (node->IsContainer()) {
            dir_cache.insert({ iter->path(), node });
        }
    }
    ItemsAdded(wxDataViewItem(nullptr), items);

    items.Clear();
    for (auto dir : dir_cache) {
        for (fs::directory_iterator iter(dir.first); iter != iter_end; iter++) {
            if (fs::is_directory(iter->path()))
                continue;

            wxString file_path = iter->path().generic_wstring();
            AuxiliaryModelNode* file = new AuxiliaryModelNode(dir.second, file_path, false);
            items.Add(wxDataViewItem(file));
        }
        ItemsAdded(wxDataViewItem(dir.second), items);
    }

    // Create default folders if they are not loaded
    wxDataViewItemArray default_items;
    for (auto folder : s_default_folders) {
        wxString folder_path = aux_path + "\\" + folder;
        if (fs::exists(folder_path.ToStdWstring()))
            continue;

        fs::create_directory(folder_path.ToStdWstring());
        AuxiliaryModelNode* node = new AuxiliaryModelNode(m_root, folder_path, true);
        default_items.Add(wxDataViewItem(node));
    }
    ItemsAdded(wxDataViewItem(nullptr), default_items);
}

int AuxiliaryModel::Compare(const wxDataViewItem& item1, const wxDataViewItem& item2,
    unsigned int column, bool ascending) const
{
    wxASSERT(item1.IsOk() && item2.IsOk());
    // should never happen

    if (IsContainer(item1) && IsContainer(item2))
    {
        wxVariant value1, value2;
        GetValue(value1, item1, 0);
        GetValue(value2, item2, 0);

        wxString str1 = value1.GetString();
        wxString str2 = value2.GetString();
        int res = str1.Cmp(str2);
        if (res) return res;

        // items must be different
        wxUIntPtr litem1 = (wxUIntPtr)item1.GetID();
        wxUIntPtr litem2 = (wxUIntPtr)item2.GetID();

        return litem1 - litem2;
    }

    return wxDataViewModel::Compare(item1, item2, column, ascending);
}

void AuxiliaryModel::GetValue(wxVariant& variant,
    const wxDataViewItem& item, unsigned int col) const
{
    wxASSERT(item.IsOk());

    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    switch (col)
    {
    case 0:
        variant = node->name;
        break;

    default:
        wxLogError("AuxiliaryModel::GetValue: wrong column %d", col);
    }
}

bool AuxiliaryModel::SetValue(const wxVariant& variant,
    const wxDataViewItem& item, unsigned int col)
{
    wxASSERT(item.IsOk());

    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    switch (col)
    {
    case 0:
        node->name = variant.GetString();
        return true;

    default:
        wxLogError("AuxiliaryModel::SetValue: wrong column");
    }
    return false;
}

bool AuxiliaryModel::IsEnabled(const wxDataViewItem& item,
    unsigned int col) const
{
    return true;
}

wxDataViewItem AuxiliaryModel::GetParent(const wxDataViewItem& item) const
{
    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();

    return wxDataViewItem(GetParent(node));
}

bool AuxiliaryModel::IsContainer(const wxDataViewItem& item) const
{
    // the invisible root node can have children
    // (in our model always "MyMusic")
    if (!item.IsOk())
        return true;

    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    return node->IsContainer();
}

static unsigned int count = 0;

unsigned int AuxiliaryModel::GetChildren(const wxDataViewItem& parent,
    wxDataViewItemArray& array) const
{
    if (m_root == nullptr)
        return 0;

    AuxiliaryModelNode* node = (AuxiliaryModelNode*)parent.GetID();
    if (!node)
    {
        node = m_root;
    }

    count = node->GetChildren().GetCount();
    for (unsigned int pos = 0; pos < count; pos++)
    {
        AuxiliaryModelNode* child = node->GetChildren().Item(pos);
        array.Add(wxDataViewItem((void*)child));
    }

    return count;
}

wxDataViewItem AuxiliaryModel::CreateFolder(wxString name)
{
    wxString folder_name = name;
    if (folder_name == wxEmptyString) {
        folder_name = _L("New Folder");
        for (int i = 1; i <= 1000; i++) {
            bool exist = false;
            for (AuxiliaryModelNode* node : m_root->GetChildren()) {
                if (!node->IsContainer())
                    continue;

                if (node->name == folder_name) {
                    exist = true;
                    break;
                }
            }

            if (!exist)
                break;

            folder_name = _L("New Folder");
            folder_name << "(" << i << ")";
        }
    }
    else {
        for (AuxiliaryModelNode* node : m_root->GetChildren()) {
            if (!node->IsContainer())
                continue;

            if (node->name == folder_name) {
                return wxDataViewItem(nullptr);
            }
        }
    }

    // Create folder in file system
    fs::path bfs_path((m_root_dir + "\\" + folder_name).ToStdWstring());
    if (fs::exists(bfs_path)) {
        try {
            bool is_done = fs::remove_all(bfs_path);
            if (!is_done)
                return wxDataViewItem(nullptr);
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
        }
    }
    fs::create_directory(bfs_path);

    // Create model node
    AuxiliaryModelNode* folder = new AuxiliaryModelNode(m_root, bfs_path.generic_wstring(), true);

    // Notify wxDataViewCtrl to update ui
    wxDataViewItem folder_item(folder);
    ItemAdded(wxDataViewItem(NULL), folder_item);
    return folder_item;
}

wxDataViewItemArray AuxiliaryModel::ImportFile(AuxiliaryModelNode* sel, wxArrayString file_paths)
{
    if (sel == nullptr) {
        sel = m_root;
    }

    wxDataViewItemArray added_items;
    AuxiliaryModelNode* parent = sel->IsContainer() ? sel : sel->GetParent();
    for (wxString file_path : file_paths) {
        bool exists = false;
        for (AuxiliaryModelNode* node : parent->GetChildren()) {
            if (node->path == file_path) {
                exists = true;
                break;
            }
        }

        if (exists)
            continue;

        // Copy imported file to project temp directory
        fs::path src_bfs_path(file_path.ToStdWstring());
        wxString dir_path = m_root_dir;
        if (sel != m_root)
            dir_path += "\\" + sel->name;
        dir_path += "\\" + src_bfs_path.filename().generic_wstring();

        boost::system::error_code ec;
        if (!fs::copy_file(src_bfs_path, fs::path(dir_path.ToStdWstring()), fs::copy_options::overwrite_existing, ec))
            continue;

        // Update model data
        AuxiliaryModelNode* file = new AuxiliaryModelNode(parent, dir_path, false);

        // Notify wxDataViewCtrl to update ui
        wxDataViewItem file_item(file);
        if (parent == m_root)
            parent = nullptr;
        Slic3r::put_other_changes();
        wxDataViewItem parent_item(parent);
        ItemAdded(parent_item, file_item);
        added_items.push_back(file_item);
    }

    return added_items;
}

void AuxiliaryModel::Delete(const wxDataViewItem& item)
{
    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    if (!node)      // happens if item.IsOk()==false
        return;

    bool is_done = false;
    if (node->IsContainer()) {
        fs::path bfs_path((m_root_dir + "\\" + node->name).ToStdWstring());
        try {
            is_done = fs::remove_all(bfs_path);
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
        }
    }
    else {
        fs::path bfs_path(node->path.ToStdWstring());
        is_done = fs::remove(bfs_path);
    }

    if (!is_done)
        return;

    node->GetParent()->GetChildren().Remove(node);

    Slic3r::put_other_changes();
    wxDataViewItem parent_item = GetParent(item);
    ItemDeleted(parent_item, item);
    delete node;
}

void AuxiliaryModel::MoveItem(const wxDataViewItem& dropped_item, const wxDataViewItem& dragged_item)
{
    AuxiliaryModelNode* dropped = (AuxiliaryModelNode*)dropped_item.GetID();
    AuxiliaryModelNode* dragged = (AuxiliaryModelNode*)dragged_item.GetID();

    if (dragged == nullptr || dragged->IsContainer())
        return;

    AuxiliaryModelNode* target_folder = nullptr;
    if (dropped == nullptr) {
        target_folder = m_root;
    }
    else if (dropped->IsContainer()) {
        target_folder = dropped;
    }
    else {
        target_folder = dropped->GetParent();
    }

    if (dragged->GetParent() == target_folder)
        return;

    for (AuxiliaryModelNode* node : target_folder->GetChildren()) {
        if (node->path == dragged->path)
            return;
    }

    // Generate new path
    wxString new_path = m_root_dir;
    if (target_folder != m_root)
        new_path += "\\" + target_folder->name;
    new_path += "\\" + dragged->name;

    // Perform file movement in file system
    fs::path bfs_new_path(new_path.ToStdWstring());
    fs::path bfs_old_path(dragged->path.ToStdWstring());
    boost::system::error_code err;
    fs::rename(bfs_old_path, bfs_new_path, err);
    if (err.failed())
        return;

    // Reparent dragged node
    wxDataViewItem old_parent_item = this->GetParent(dragged_item);
    this->Reparent(dragged, target_folder);
    dragged->path = new_path;

    // Notify wxDataViewCtrl to update ui
    Slic3r::put_other_changes();
    ItemDeleted(old_parent_item, wxDataViewItem(dragged));
    ItemAdded(wxDataViewItem(target_folder == m_root ? nullptr : target_folder), wxDataViewItem(dragged));
}

bool AuxiliaryModel::IsOrphan(const wxDataViewItem& item)
{
    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    return node->GetParent() != m_root;
}

bool AuxiliaryModel::Rename(const wxDataViewItem& item, const wxString& name)
{
    AuxiliaryModelNode* node = (AuxiliaryModelNode*)item.GetID();
    AuxiliaryModelNode* parent = node->GetParent();

    if (node->IsContainer())
        return false;

    if (!parent->IsContainer())
        return false;

    for (AuxiliaryModelNode* cur_node : parent->GetChildren()) {
        if (cur_node->name == name)
            return false;
    }

    boost::system::error_code err;
    fs::path old_path((m_root_dir + "\\" + parent->name + "\\" + node->name).ToStdWstring());
    fs::path new_path((m_root_dir + "\\" + parent->name + "\\" + name).ToStdWstring());
    fs::rename(old_path, new_path, err);
    if (err.failed())
        return false;

    Slic3r::put_other_changes();
    node->name = name;
    node->path = m_root_dir + "\\" + parent->name + "\\" + name;
    return true;
}

AuxiliaryModelNode* AuxiliaryModel::GetParent(AuxiliaryModelNode* node) const
{
    if (node == m_root || node->GetParent() == m_root)
        return nullptr;

    return node->GetParent();
}

void AuxiliaryModel::Reparent(AuxiliaryModelNode* node, AuxiliaryModelNode* new_parent)
{
    if (node->IsContainer())
        return;

    node->GetParent()->GetChildren().Remove(node);
    node->SetParent(new_parent);
    new_parent->Append(node);
}
