#pragma once

#include <wx/dialog.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <vector>
#include <string>



namespace Slic3r { namespace GUI {

struct PrinterItem
{
    std::string id;
    std::string name;
    std::string model;
};

class MyPrinterCheckItem : public wxPanel
{
public:
    MyPrinterCheckItem(
        wxWindow* parent, const std::string& name, const std::string& id, const std::string& model, wxWindowID winid = wxID_ANY);

    bool               IsChecked() const { return checkbox->IsChecked(); }
    const std::string& GetPrinterId() const { return m_id; }
    const std::string& GetPrinterName() const { return m_name; }

private:
    wxCheckBox*   checkbox;
    wxStaticText* nameText;
    wxStaticText* modelText;
    std::string   m_id;
    std::string   m_name;
    wxStaticText* idText;
    void          OnPaint(wxPaintEvent& event);
    wxFont        setFont(int size, bool bold=false);
};

class UploadDialog : public wxDialog
{
public:
    UploadDialog(wxWindow* parent, const std::string& username, const std::string& password);

    ~UploadDialog();

    std::vector<std::string> GetSelectedOptions() const;
    bool                     run();
    void                     onClose(wxCloseEvent& event);

private:
    std::string              u;
    std::string              p;
    // Replace the original options and checkboxes members
    std::vector<PrinterItem>         printers; // Each item contains an ID and name
    std::vector<MyPrinterCheckItem*> checkboxes;
    wxBoxSizer*                      checkboxSizer;
    void                             getOnlinePrinter();
    // void                             onlySendEvt();
    const std::string url_str = "https://cloud.iemai3d.com/api/get-online-printer/";
    //const std::string url_str    = "http://192.168.31.254:8000/api/get-online-printer/";
    wxButton*         refreshBtn;
    wxScrolledWindow* scrollWin;
    wxBoxSizer*       topBarSizer;
    wxBoxSizer*       mainSizer;
    wxBoxSizer*       buttonSizer;
    wxButton*         okBtn;
    wxButton*         onlySendBtn;
    wxButton*         cancelBtn;
    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI
