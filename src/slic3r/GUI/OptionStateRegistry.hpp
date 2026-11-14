#pragma once

#include <string>
#include <unordered_map>
#include <wx/string.h>

namespace Slic3r::GUI {

// Records only restrictions reported by a Tab's public toggle entry points and the pages it hides.
// Keys are kept verbatim.
class OptionStateRegistry
{
public:
    void set_visible(const std::string& key, bool visible, const wxString& reason = {})
    {
        update(m_hidden, key, visible, reason);
    }

    // Takes precedence over set_visible() and survives clear(), as only showing the page lifts it.
    void set_page_hidden(const std::string& key, bool hidden, const wxString& reason = {})
    {
        update(m_page_hidden, key, !hidden, reason);
    }

    void set_enabled(const std::string& key, bool enabled, const wxString& reason = {})
    {
        update(m_disabled, key, enabled, reason);
    }

    const wxString* hidden_reason(const std::string& key) const
    {
        const wxString* reason = find(m_page_hidden, key);
        return reason ? reason : find(m_hidden, key);
    }
    const wxString* disabled_reason(const std::string& key) const { return find(m_disabled, key); }

    wxString description(const std::string& key) const;

    void clear()
    {
        m_hidden.clear();
        m_disabled.clear();
    }

private:
    using Reasons = std::unordered_map<std::string, wxString>;
    Reasons m_hidden;
    Reasons m_page_hidden;
    Reasons m_disabled;

    static void update(Reasons& reasons, const std::string& key, bool available, const wxString& reason)
    {
        if (available)
            reasons.erase(key);
        else
            reasons.insert_or_assign(key, reason);
    }

    static const wxString* find(const Reasons& reasons, const std::string& key)
    {
        auto it = reasons.find(key);
        return it == reasons.end() ? nullptr : &it->second;
    }
};

} // namespace Slic3r::GUI
