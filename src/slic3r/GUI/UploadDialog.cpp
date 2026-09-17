#include "UploadDialog.hpp"

#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <curl/curl.h>

#include <wx/fileconf.h>


#include <boost/cast.hpp>


#include <nlohmann/json.hpp>
#include "MainFrame.hpp"
#include <boost/dll.hpp>


#include <slic3r/GUI/Widgets/WebView.hpp>

#include "IconManager.hpp"

#define DESIGN_SELECTOR_NOMORE_COLOR wxColour(248, 248, 248)
#define DESIGN_GRAY900_COLOR wxColour(38, 46, 48)
#define DESIGN_GRAY800_COLOR wxColour(50, 58, 61)
#define DESIGN_GRAY600_COLOR wxColour(144, 144, 144)
#define DESIGN_GRAY400_COLOR wxColour(166, 169, 170)
using namespace std;

using namespace nlohmann;

namespace Slic3r {
namespace GUI {

#define NETWORK_OFFLINE_TIMER_ID 10001

BEGIN_EVENT_TABLE(UploadDialog, wxDialog)
END_EVENT_TABLE()

MyPrinterCheckItem::MyPrinterCheckItem(
    wxWindow* parent, const std::string& name, const std::string& id, const std::string& model, wxWindowID winid)
    : wxPanel(parent, winid, wxDefaultPosition, wxSize(400, 80), wxBORDER_SIMPLE), m_id(id), m_name(name)
{
    wxBoxSizer* mainSizer = new wxBoxSizer(wxHORIZONTAL);
    SetMinSize(wxSize(400, -1));
    nameText = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(name));

    wxBoxSizer* checkbox_sizer = new wxBoxSizer(wxVARIABLE);
    checkbox_sizer->SetMinSize(wxSize(50, -1));
    wxBoxSizer* text_sizer = new wxBoxSizer(wxVARIABLE);

    checkbox = new wxCheckBox(this, wxID_ANY, ""); // Do not display text
    idText   = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(id));

    checkbox_sizer->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_CENTER_HORIZONTAL, 5);

    modelText = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(model));

    nameText->SetFont(setFont(16,true));
    nameText->SetForegroundColour(DESIGN_GRAY600_COLOR);
    idText->SetFont(setFont(10,false));
    idText->SetForegroundColour(DESIGN_GRAY600_COLOR);
    modelText->SetFont(setFont(12,false));
    modelText->SetForegroundColour(DESIGN_GRAY600_COLOR);
    text_sizer->Add(nameText);
    text_sizer->Add(idText);
    text_sizer->Add(modelText);

    mainSizer->Add(checkbox_sizer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    mainSizer->Add(text_sizer, 0, wxBOTTOM, 2);

    SetSizerAndFit(mainSizer);
    // wxGetApp().UpdateDlgDarkUI(this);
}

wxFont MyPrinterCheckItem::setFont(int size, bool bold){
#ifndef __APPLE__
    size = size * 4 / 5;
#endif

    wxString face = "HarmonyOS Sans SC";

    // Check if the current locale is Korean
    if (wxLocale::GetSystemLanguage() == wxLANGUAGE_KOREAN) {
        face = "NanumGothic";
    }

    wxFont font{size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL, false, face};
    font.SetFaceName(face);
    if (!font.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("Can't find %1% font") % face;
        font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        BOOST_LOG_TRIVIAL(warning) << boost::format("Use system font instead: %1%") % font.GetFaceName();
        if (bold)
            font.MakeBold();
        font.SetPointSize(size);
    }
    return font;
};

UploadDialog::UploadDialog(wxWindow* parent, const std::string& username, const std::string& password)
    : wxDialog(parent, wxID_ANY, _("Objects List"), wxDefaultPosition, wxSize(600, 800)), u(username), p(password)
{
    SetBackgroundColour(*wxWHITE);
    
    mainSizer   = new wxBoxSizer(wxVERTICAL);

    // Line 1: Top left button
    topBarSizer = new wxBoxSizer(wxHORIZONTAL);
    refreshBtn  = new wxButton(this, wxID_HIGHEST + 1, _("Refresh"));
    topBarSizer->Add(refreshBtn, 0, wxLEFT | wxTOP | wxBOTTOM, 8);
    mainSizer->Add(topBarSizer, 0, wxEXPAND | wxLEFT, 5);

    // Create a scrollable area
    scrollWin = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scrollWin->SetScrollRate(0, 10);          // Scroll pixels every time
    scrollWin->SetBackgroundColour(*wxWHITE);            // Adaptable to dark themes

    // Add sizer to scrollWin
    checkboxSizer = new wxBoxSizer(wxVERTICAL);
    scrollWin->SetSizer(checkboxSizer);
    scrollWin->SetMinSize(wxSize(420, 600));
    // checkbox Area
    
    mainSizer->Add(scrollWin, 1, wxEXPAND | wxALL, 10);

    // Bottom confirm/cancel button
    buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    okBtn       = new wxButton(this, wxID_OK, _("Print"));
    onlySendBtn = new wxButton(this, wxID_HIGHEST + 2, _("Only Send"));
    cancelBtn   = new wxButton(this, wxID_CANCEL, _("Cancel"));

    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(okBtn, 0, wxALL, 5);
    buttonSizer->Add(onlySendBtn, 0, wxALL, 5);
    buttonSizer->Add(cancelBtn, 0, wxALL, 5);

    mainSizer->Add(buttonSizer, 0, wxEXPAND | wxBOTTOM | wxRIGHT, 10);

    SetSizer(mainSizer);
    Layout();
    Centre();

    // Bind refresh button event
    refreshBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { this->getOnlinePrinter(); });
    onlySendBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_HIGHEST + 2);});
    // Automatically load during initialization
    getOnlinePrinter();
    wxGetApp().UpdateDlgDarkUI(this);
    // wxGetApp().UpdateDarkUI(refreshBtn);
}


UploadDialog::~UploadDialog()
{
}

std::vector<std::string> UploadDialog::GetSelectedOptions() const
{
    std::vector<std::string> selected_ids;
    for (size_t i = 0; i < checkboxes.size(); ++i) {
        if (checkboxes[i]->IsChecked()) {
            selected_ids.push_back(printers[i].id);
        }
    }
    return selected_ids;
}

void UploadDialog::onClose(wxCloseEvent& event)
{
    Destroy();
}



bool UploadDialog::run()
{

    if (this->ShowModal() == wxID_OK) {
        return true;
    } else {
        return false;
    }
}

void UploadDialog::getOnlinePrinter()
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return;
    refreshBtn->Enable(false);
    curl_mime* mime = curl_mime_init(curl);

    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "username");
    curl_mime_data(part, u.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "password");
    curl_mime_data(part, p.c_str(), CURL_ZERO_TERMINATED);

    std::string response_str;
    curl_easy_setopt(curl, CURLOPT_URL, url_str.c_str());
    auto pem = resources_dir() + "/cert/iemai3d.com.pem";
    curl_easy_setopt(curl, CURLOPT_CAINFO, pem.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wxUploader/1.0");

    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            std::string* stream = static_cast<std::string*>(userdata);
            stream->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);

    CURLcode res = curl_easy_perform(curl);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        wxLogError("network error: %s", curl_easy_strerror(res));
        return;
    }

    // Parse JSON and update options
    try {
        auto json_data = nlohmann::json::parse(response_str);
        auto printers_json = json_data["online_printer"];

        printers.clear();
        for (const auto& printer : printers_json) {
            PrinterItem item;
            item.id   = printer["printer_id"]; // Assuming the backend field name is printer_id
            item.name = printer["printer_name"];
            item.model = printer["printer_model"];
            printers.push_back(item);
        }

        // Clear the old ones checkbox
        for (auto cb : checkboxes) {
            checkboxSizer->Detach(cb);
            cb->Destroy();
        }
        checkboxes.clear();
        scrollWin->Freeze();
        for (const auto& label : printers) {
            MyPrinterCheckItem* cb = new MyPrinterCheckItem(scrollWin, label.name, label.id, label.model);
            checkboxes.push_back(cb);
            checkboxSizer->Add(cb, 0, wxEXPAND | wxALL , 5);
        }

        checkboxSizer->Layout();
        scrollWin->FitInside();
        scrollWin->SetScrollRate(0, 10);
        scrollWin->Thaw();
        GetSizer()->Layout();
        // Fit(); // Automatically adjust the size of the dialog box

    } catch (const std::exception& e) {
        wxLogError("JSON Analysis failed: %s", e.what());
    }
    refreshBtn->Enable(true);
}

}}

