#pragma once

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct CurveView {
    double min_x = 0.0;
    double max_x = 1.0;
    double min_y = 0.0;
    double max_y = 1.0;
};

struct CurveViewLimits {
    CurveView bounds;
    double x_step = 1.0;
    double y_step = 0.01;
    double minimum_span = 0.001;
};

// Numeric curve rules and editable text, independent of wxWidgets. Dialogs own
// presentation; models supply interpolation, defaults and editing constraints.
class CurveModel
{
public:
    using Row = std::pair<std::string, std::string>;
    using Rows = std::vector<Row>;
    using Interpolator = std::function<double(double)>;

    explicit CurveModel(std::vector<std::string> parameters = {});
    virtual ~CurveModel() = default;

    Rows rows() const { return decode_rows(m_parameters); }
    void accept_rows(const Rows& rows);
    bool is_modified() const { return m_parameters != m_initial_parameters; }
    const std::vector<std::string>& parameters() const { return m_parameters; }
    const std::string& serialized_parameters() const { return m_serialized_parameters; }

    // Preserve unchanged config rows verbatim; normalize only edited numeric cells.
    static Rows decode_rows(const std::vector<std::string>& parameters);
    static std::vector<std::string> encode_rows(const Rows& rows, const std::vector<std::string>& initial_parameters);
    // Legacy GUI value format. ConfigOptionStrings handles serialization to files.
    static std::string serialize_parameters(const std::vector<std::string>& parameters);
    static bool read_number(std::string text, double& value);
    static bool valid_view(const CurveView& view);

    // A null message means success; row identifies the offending point, or -1.
    const char* read_points(const Rows& rows, std::vector<double>& x, std::vector<double>& y, int& row) const;
    const char* read_view(const std::array<std::string, 4>& bounds, CurveView& view) const;
    // Translate the viewport as a whole, stopping at its limits without changing scale.
    CurveView panned_view(const CurveView& view, double x_offset, double y_offset) const;
    virtual const char* validate_points(const std::vector<double>& x, const std::vector<double>& y, int& row) const = 0;
    // Shared by previews and numeric consumers. Override the default PCHIP
    // factory when a feature uses a different interpolation algorithm.
    virtual Interpolator make_interpolator(const std::vector<double>& x, const std::vector<double>& y) const;
    virtual CurveView fitted_view(const std::vector<double>& x, const std::vector<double>& y) const = 0;
    virtual CurveViewLimits view_limits() const = 0;
    // Equal bounds lock an axis. Inverted bounds mean no move is possible in this viewport.
    virtual CurveView drag_bounds(int row, const std::vector<double>& x, const std::vector<double>& y,
                                  const CurveView& view) const = 0;
    virtual Rows default_rows() const = 0;
    virtual Rows seed_rows() const = 0;
    virtual const char* empty_message() const { return nullptr; }
    virtual const char* validate_view(const CurveView& view) const;

private:
    std::vector<std::string> m_initial_parameters;
    std::vector<std::string> m_parameters;
    std::string m_serialized_parameters;
};

} // namespace Slic3r
