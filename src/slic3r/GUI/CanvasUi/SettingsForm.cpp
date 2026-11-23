#include "SettingsForm.hpp"

#include <functional>
#include <string>
#include <vector>

#include "Availability.hpp"
#include "Collapsible.hpp"
#include "Context.hpp"
#include "Field.hpp"
#include "FontMeasure.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

namespace {

bool on_tab_shown(const SettingsFormOptions& form, const SettingsField& field)
{
    return form.tabs.empty() || field.tab == (form.tab != nullptr ? *form.tab : 0);
}

float content_height(const Metrics& m, const TextMeasure& measure, const TextMeasure& choices, const SettingsFormOptions& form)
{
    const float        inner = form.width - 2.f * m.card_padding;
    std::vector<float> heights;
    for (const SettingsField& field : form.fields)
        if (on_tab_shown(form, field))
            heights.push_back(field.options.empty() ? field_switch_height(m, measure, field.label, inner) :
                                                      field_toggle_group_height(m, measure, choices, field.label, field.options, inner));
    return stack_height(m, heights);
}

} // namespace

SettingsField settings_switch(const char* id, const std::string& label, bool& value)
{
    SettingsField field;
    field.id    = id;
    field.label = label;
    field.get   = [&value]() { return value ? 1 : 0; };
    field.set   = [&value](int on) { value = on != 0; };
    return field;
}

float settings_form_height(const Metrics& metrics, const TextMeasure& measure, const TextMeasure& choices, const SettingsFormOptions& form)
{
    return collapsible_panel_height(metrics, content_height(metrics, measure, choices, form));
}

bool settings_form(const char* id, const Point& pos, const SettingsFormOptions& form)
{
    CollapsiblePanelOptions panel;
    panel.width          = form.width;
    panel.height         = form.height;
    panel.content_height = content_height(Context::current()->metrics(), FontMeasure(), ChoiceMeasure(), form);
    panel.tabs           = form.tabs;
    panel.tab            = form.tab;
    panel.changed_count  = form.changed > 0 ? form.changed_count : std::string();
    panel.reset_label    = form.reset_label;
    panel.can_reset      = form.changed > 0;
    panel.below          = form.below;
    const CollapsiblePanel settings(id, pos, panel);
    bool                   changed = false;
    if (settings.reset() && form.reset) {
        form.reset();
        changed = true;
    }
    for (const SettingsField& field : form.fields) {
        if (!on_tab_shown(form, field))
            continue;
        const FieldOptions options{field.label, settings.content_width(), field.availability ? field.availability() : Availability()};
        if (field.options.empty()) {
            bool on = field.get() != 0;
            if (field_switch(field.id, on, options)) {
                field.set(on ? 1 : 0);
                changed = true;
            }
        } else {
            int chosen = field.get();
            if (field_toggle_group(field.id, chosen, field.options, options)) {
                field.set(chosen);
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
