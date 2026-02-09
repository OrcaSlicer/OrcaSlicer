#pragma once

#include <string>
#include <unordered_map>

namespace Slic3r {

// Singleton class that provides display-time translation for vendor profile strings.
// Translations are loaded from {vendor}/I18N/{lang}.json files.
// The internal preset names remain unchanged; only the displayed text is translated.
class ProfileTranslator
{
public:
    static ProfileTranslator& instance();

    // Load vendor translations for the given language code from a profiles directory.
    // Scans all vendor subdirectories for I18N/{lang}.json files.
    // This is additive - call clear() first if you want to start fresh.
    void load_translations(const std::string& profiles_dir, const std::string& language_code);

    // Translate a profile string. Returns the translation if found, otherwise returns the original.
    const std::string& translate(const std::string& source) const;

    // Clear all loaded translations.
    void clear();

    bool has_translations() const { return !m_translations.empty(); }
    const std::string& current_language() const { return m_current_language; }

private:
    ProfileTranslator() = default;
    ProfileTranslator(const ProfileTranslator&) = delete;
    ProfileTranslator& operator=(const ProfileTranslator&) = delete;

    // Load translations from a single vendor directory.
    void load_vendor_translations(const std::string& vendor_dir, const std::string& language_code);

    // Try to parse a translation JSON file. Returns true if found and loaded.
    bool try_load_file(const std::string& filepath);

    // source string -> translated string
    std::unordered_map<std::string, std::string> m_translations;
    std::string m_current_language;
};

} // namespace Slic3r
