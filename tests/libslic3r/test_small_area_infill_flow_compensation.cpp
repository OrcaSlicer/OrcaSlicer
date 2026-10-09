#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/SmallAreaInfillFlowCompensationModel.hpp"
#include "libslic3r/GCode/PchipInterpolatorHelper.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CurveModel.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using FlowModel = Slic3r::SmallAreaInfillFlowCompensationModel;

namespace {

class FixedInterpolationModel : public FlowModel
{
public:
    Interpolator make_interpolator(const std::vector<double>&, const std::vector<double>&) const override
    {
        return [](double) { return 0.75; };
    }
};

} // namespace

TEST_CASE("Curve models default to PCHIP and allow derived interpolation overrides", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const std::vector<double> x = {0, 1, 3}, y = {0, 0.8, 1};
    FlowModel flow_model;
    const CurveModel& base_model = flow_model;
    const auto default_curve = base_model.make_interpolator(x, y);
    const PchipInterpolatorHelper expected(x, y);
    const double length = GENERATE(0.0, 0.25, 0.5, 1.0, 2.0, 3.0, 4.0);
    CHECK_THAT(default_curve(length), WithinAbs(expected.interpolate(length), 1e-12));

    FixedInterpolationModel fixed_model;
    const CurveModel& overridden_model = fixed_model;
    const auto overridden_curve = overridden_model.make_interpolator(x, y);
    CHECK_THAT(overridden_curve(length), WithinAbs(0.75, 1e-12));
}

TEST_CASE("A curve model stays unchanged until accepted and clears its modified state when restored", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const std::vector<std::string> parameters = {"0,0", "\n5,0.5000000000000001", "\n10,1"};
    FlowModel flow_model(parameters);
    CurveModel& model = flow_model;
    auto rows = model.rows();
    rows[1].second = "0.75";
    CHECK_FALSE(model.is_modified());
    CHECK(model.parameters() == parameters);

    model.accept_rows(rows);
    CHECK(model.is_modified());
    CHECK(model.parameters()[1] == "\n5,0.75");
    CHECK(model.serialized_parameters() == CurveModel::serialize_parameters(model.parameters()));

    model.accept_rows(CurveModel::decode_rows(parameters));
    CHECK_FALSE(model.is_modified());
    CHECK(model.parameters() == parameters);
}

TEST_CASE("Curve numbers accept decimal commas signed values and scientific notation", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const std::vector<std::string> inputs = {" 2.125 ", "2,125", "+2.125", "2.125e0", "2125e-3"};
    const auto input = GENERATE_COPY(from_range(inputs));
    double value;
    REQUIRE(CurveModel::read_number(input, value));
    CHECK_THAT(value, WithinAbs(2.125, 1e-15));
    REQUIRE(CurveModel::read_number("-0,5", value));
    CHECK_THAT(value, WithinAbs(-0.5, 1e-15));
}

TEST_CASE("Curve numbers reject missing malformed and nonfinite input", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const std::string input = GENERATE(as<std::string>{}, "", " ", "nan", "inf", "-inf", "1e9999", "2mm", "1,2,3", "1.2.3");
    double value;
    CHECK_FALSE(CurveModel::read_number(input, value));
}

TEST_CASE("Reading curve rows reports numeric errors and validates model rules", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model;
    std::vector<double> x, y;
    int row = -1;
    REQUIRE(model.read_points({{"0", "0"}, {"2,5", "0,5"}, {"10", "1"}}, x, y, row) == nullptr);
    REQUIRE(x.size() == 3);
    CHECK_THAT(x[1], WithinAbs(2.5, 1e-12));
    CHECK_THAT(y[1], WithinAbs(0.5, 1e-12));

    CHECK(model.read_points({{"0", "0"}, {"2", "not a number"}, {"10", "1"}}, x, y, row) != nullptr);
    CHECK(row == 1);
    CHECK(model.read_points({{"0", "0"}, {"2", "0.5"}, {"10", "0.9"}}, x, y, row) != nullptr);
    CHECK(row == 2);
}

TEST_CASE("Opening and accepting a flow model preserves its saved precision and formatting", "[SmallAreaInfillFlowCompensation]")
{
    const std::vector<std::string> parameters = {" 0, 0.00000000000000001 ", "\n2.1234567890123457,0.6543210987654321", "\n10,1.0000"};
    FlowModel model(parameters);
    const auto rows = model.rows();
    REQUIRE(rows.size() == parameters.size());
    CHECK(rows.front().first == "0");
    CHECK(rows[1].first == "2.1234567890123457");
    model.accept_rows(rows);
    CHECK(model.parameters() == parameters);
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Malformed flow model points remain visible for repair", "[SmallAreaInfillFlowCompensation]")
{
    const auto rows = FlowModel::decode_rows({"missing separator", "\n,0.5", "2,", "3,0.8,extra"});
    REQUIRE(rows.size() == 4);
    CHECK(rows[0] == FlowModel::Row{"missing separator", ""});
    CHECK(rows[1] == FlowModel::Row{"", "0.5"});
    CHECK(rows[2] == FlowModel::Row{"2", ""});
    CHECK(rows[3] == FlowModel::Row{"3", "0.8,extra"});
}

TEST_CASE("Editing a flow model point normalizes decimal commas and preserves other points", "[SmallAreaInfillFlowCompensation]")
{
    const std::vector<std::string> parameters = {"0,0", "\n2,0.50", "\n10,1"};
    auto rows = FlowModel::decode_rows(parameters);
    rows[1] = {" 2,125 ", " 0,625 "};
    const auto edited = FlowModel::encode_rows(rows, parameters);
    CHECK(edited == std::vector<std::string>{parameters[0], "\n2.125,0.625", parameters[2]});

    ConfigOptionStrings saved;
    saved.values = edited;
    ConfigOptionStrings restored;
    REQUIRE(restored.deserialize(saved.serialize()));
    CHECK(restored.values == edited);
}

TEST_CASE("Flow model rows support adding removing and resetting points", "[SmallAreaInfillFlowCompensation]")
{
    const std::vector<std::string> parameters = {"0,0", "\n10,1"};
    auto rows = FlowModel::decode_rows(parameters);
    rows.insert(rows.begin() + 1, {"5", "0.5"});
    const auto expanded = FlowModel::encode_rows(rows, parameters);
    CHECK(expanded == std::vector<std::string>{"0,0", "\n5,0.5", "\n10,1"});
    rows.erase(rows.begin() + 1);
    CHECK(FlowModel::encode_rows(rows, expanded) == parameters);

    const auto* defaults = static_cast<const ConfigOptionStrings*>(
        print_config_def.get("small_area_infill_flow_compensation_model")->default_value.get());
    CHECK(FlowModel::encode_rows(FlowModel::decode_rows(defaults->values), parameters) == defaults->values);
}

TEST_CASE("Clearing a flow model serializes an empty option", "[SmallAreaInfillFlowCompensation]")
{
    FlowModel model({"0,0", "\n10,1"});
    model.accept_rows({});
    CHECK(model.parameters().empty());
    CHECK(model.serialized_parameters().empty());
    CHECK(model.is_modified());
    CHECK_FALSE(model.make_interpolator({}, {}));
    int row = -1;
    CHECK(FlowModel().validate_points({}, {}, row) == nullptr);
}

TEST_CASE("Default and seeded flow models produce valid preview curves", "[SmallAreaInfillFlowCompensation]")
{
    FlowModel model;
    const bool use_defaults = GENERATE(false, true);
    const auto rows = use_defaults ? model.default_rows() : model.seed_rows();
    std::vector<double> x, y;
    int row = -1;
    REQUIRE(model.read_points(rows, x, y, row) == nullptr);
    REQUIRE(x.size() >= 2);
    const auto interpolate = model.make_interpolator(x, y);
    CHECK_THAT(interpolate(x.back()), WithinAbs(1.0, 1e-12));
    for (int sample = 1; sample < 10; ++sample) {
        const double factor = interpolate(x.back() * sample / 10.0);
        CHECK(factor > 0.0);
        CHECK(factor < 1.0);
    }
}

TEST_CASE("Flow model validation identifies the invalid point", "[SmallAreaInfillFlowCompensation]")
{
    struct InvalidModel { std::vector<double> x, y; int row; };
    const std::vector<InvalidModel> models = {
        {{0}, {1}, -1},
        {{1, 2}, {0, 1}, 0},
        {{0, 0, 10}, {0, 0.5, 1}, 1},
        {{0, 5, 4}, {0, 0.5, 1}, 2},
        {{0, 5, 10}, {0, 0, 1}, 1},
        {{0, 5, 10}, {0, 0.5, 0.4}, 2},
        {{0, 10}, {0, 0.9}, 1},
        {{0, 10}, {0}, -1},
    };
    const size_t index = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(6), size_t(7));
    const auto& model = models[index];
    CAPTURE(index);
    int row = -1;
    CHECK(FlowModel().validate_points(model.x, model.y, row) != nullptr);
    CHECK(row == model.row);
}

TEST_CASE("Flow model validation rejects nonfinite coordinates", "[SmallAreaInfillFlowCompensation]")
{
    const double value = GENERATE(std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity());
    const int axis = GENERATE(0, 1);
    std::vector<double> x = {0, 5, 10}, y = {0, 0.5, 1};
    (axis == 0 ? x : y)[1] = value;
    int row = -1;
    CHECK(FlowModel().validate_points(x, y, row) != nullptr);
    CHECK(row == 1);
}

TEST_CASE("Flow model validation preserves the consumer endpoint tolerance", "[SmallAreaInfillFlowCompensation]")
{
    const double endpoint = GENERATE(1.0, std::nextafter(1.0, 0.0), std::nextafter(1.0, 2.0));
    int row = -1;
    CHECK(FlowModel().validate_points({0, 10}, {0, endpoint}, row) == nullptr);
    // Negative factors are permitted in existing profiles; editing must preserve that contract.
    CHECK(FlowModel().validate_points({0, 10}, {-0.1, endpoint}, row) == nullptr);
}

TEST_CASE("Fitting a flow curve includes every point without modifying the model", "[SmallAreaInfillFlowCompensation]")
{
    FlowModel model({"0,-0.1", "\n5,0.5", "\n10,1"});
    const std::vector<double> x = {0, 5, 10}, y = {-0.1, 0.5, 1};
    const auto view = model.fitted_view(x, y);
    REQUIRE(CurveModel::valid_view(view));
    for (size_t i = 0; i < x.size(); ++i) {
        CHECK(x[i] >= view.min_x);
        CHECK(x[i] <= view.max_x);
        CHECK(y[i] >= view.min_y);
        CHECK(y[i] <= view.max_y);
    }
    CHECK_FALSE(model.is_modified());
    CHECK(CurveModel::valid_view(model.fitted_view({}, {})));
    const double largest = std::numeric_limits<double>::max();
    CHECK(CurveModel::valid_view(model.fitted_view({0, largest}, {0, 1})));
}

TEST_CASE("Dragging flow points preserves endpoints strict ordering and the visible range", "[SmallAreaInfillFlowCompensation]")
{
    FlowModel model;
    const std::vector<double> x = {0, 5, 10}, y = {0, 0.5, 1};
    const CurveView view = {0, 12, 0, 1};
    const auto first = model.drag_bounds(0, x, y, view);
    CHECK_THAT(first.min_x, WithinAbs(0.0, 1e-12));
    CHECK_THAT(first.max_x, WithinAbs(0.0, 1e-12));
    CHECK(first.max_y < y[1]);
    const auto middle = model.drag_bounds(1, x, y, view);
    CHECK(middle.min_x > x[0]);
    CHECK(middle.max_x < x[2]);
    CHECK(middle.min_y > y[0]);
    CHECK(middle.max_y < y[2]);
    const auto last = model.drag_bounds(2, x, y, view);
    CHECK(last.min_x > x[1]);
    CHECK_THAT(last.min_y, WithinAbs(1.0, 1e-12));
    CHECK_THAT(last.max_y, WithinAbs(1.0, 1e-12));
    const auto clipped = model.drag_bounds(1, x, y, {2, 8, 0.2, 0.8});
    CHECK_THAT(clipped.min_x, WithinAbs(2.0, 1e-12));
    CHECK_THAT(clipped.max_x, WithinAbs(8.0, 1e-12));
    CHECK_THAT(clipped.min_y, WithinAbs(0.2, 1e-12));
    CHECK_THAT(clipped.max_y, WithinAbs(0.8, 1e-12));
}

TEST_CASE("Curve viewports reject reversed degenerate nonfinite and overflowing ranges", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const double largest = std::numeric_limits<double>::max();
    const std::vector<CurveView> views = {
        {0, 0, 0, 1}, {2, 1, 0, 1}, {0, 1, 1, 1}, {0, 1, 2, 1},
        {0, std::numeric_limits<double>::infinity(), 0, 1},
        {0, 1, std::numeric_limits<double>::quiet_NaN(), 1},
        {-largest, largest, 0, 1}, {0, 1, -largest, largest},
    };
    const auto view = GENERATE_COPY(from_range(views));
    CHECK_FALSE(CurveModel::valid_view(view));
    CHECK(FlowModel().validate_view({-1, 10, 0, 1}) != nullptr);
    CHECK(FlowModel().validate_view({0, 10, -0.1, 1}) == nullptr);
}

TEST_CASE("Flow viewports enforce useful bounds and a minimum visible span", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model;
    CHECK(model.validate_view({0, 1000, -1, 2}) == nullptr);
    CHECK(model.validate_view({999.999, 1000, 1.999, 2}) == nullptr);
    const std::vector<CurveView> invalid = {
        {-0.001, 10, 0, 1}, {0, 1000.001, 0, 1},
        {0, 10, -1.001, 1}, {0, 10, 0, 2.001},
        {0, 0.0001, 0, 1}, {0, 10, 0, 0.0001},
        {10, 10, 0, 1}, {10, 5, 0, 1}, {0, 10, 1, 0},
        {0, std::numeric_limits<double>::infinity(), 0, 1},
        {0, 10, std::numeric_limits<double>::quiet_NaN(), 1},
    };
    const auto view = GENERATE_COPY(from_range(invalid));
    CHECK(model.validate_view(view) != nullptr);
}

TEST_CASE("Panning curve views preserves scale and model values", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewPanning]")
{
    const std::vector<std::string> parameters = {"0,0", "\n10,1"};
    FlowModel model(parameters);
    const CurveView view = {2, 12, -0.1, 0.9};
    const double offset = GENERATE(-0.25, 0.25);
    const bool vertical = GENERATE(false, true);
    const double dx = vertical ? 0.0 : offset;
    const double dy = vertical ? offset : 0.0;
    const auto panned = model.panned_view(view, dx, dy);
    CHECK(model.validate_view(panned) == nullptr);
    CHECK_THAT(panned.min_x, WithinAbs(view.min_x + dx, 1e-12));
    CHECK_THAT(panned.max_x, WithinAbs(view.max_x + dx, 1e-12));
    CHECK_THAT(panned.min_y, WithinAbs(view.min_y + dy, 1e-12));
    CHECK_THAT(panned.max_y, WithinAbs(view.max_y + dy, 1e-12));
    CHECK_THAT(panned.max_x - panned.min_x, WithinAbs(view.max_x - view.min_x, 1e-12));
    CHECK_THAT(panned.max_y - panned.min_y, WithinAbs(view.max_y - view.min_y, 1e-12));
    CHECK(model.parameters() == parameters);
    CHECK(model.serialized_parameters() == CurveModel::serialize_parameters(parameters));
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Panning curve views stops whole intervals at each boundary", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewPanning]")
{
    FlowModel model;
    const CurveView view = {2, 12, -0.1, 0.9};
    const auto bounds = model.view_limits().bounds;
    const double offset = GENERATE(-1e100, 1e100);
    const bool vertical = GENERATE(false, true);
    const auto panned = model.panned_view(view, vertical ? 0.0 : offset, vertical ? offset : 0.0);
    CHECK(model.validate_view(panned) == nullptr);
    CHECK_THAT(panned.max_x - panned.min_x, WithinAbs(view.max_x - view.min_x, 1e-12));
    CHECK_THAT(panned.max_y - panned.min_y, WithinAbs(view.max_y - view.min_y, 1e-12));
    if (vertical) {
        CHECK_THAT(offset > 0 ? panned.max_y : panned.min_y, WithinAbs(offset > 0 ? bounds.max_y : bounds.min_y, 1e-12));
        CHECK_THAT(panned.min_x, WithinAbs(view.min_x, 1e-12));
        CHECK_THAT(panned.max_x, WithinAbs(view.max_x, 1e-12));
    } else {
        CHECK_THAT(offset > 0 ? panned.max_x : panned.min_x, WithinAbs(offset > 0 ? bounds.max_x : bounds.min_x, 1e-12));
        CHECK_THAT(panned.min_y, WithinAbs(view.min_y, 1e-12));
        CHECK_THAT(panned.max_y, WithinAbs(view.max_y, 1e-12));
    }
}

TEST_CASE("Panning full and minimum span curve views keeps valid ranges", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewPanning]")
{
    FlowModel model;
    const auto bounds = model.view_limits().bounds;
    const std::vector<CurveView> views = {{0, 0.001, 0, 0.001}, {999.999, 1000, 1.999, 2}, bounds};
    const auto view = GENERATE_COPY(from_range(views));
    REQUIRE(model.validate_view(view) == nullptr);
    const double offset = GENERATE(-1e100, 1e100);
    const auto panned = model.panned_view(view, offset, offset);
    CHECK(model.validate_view(panned) == nullptr);
    CHECK_THAT(panned.max_x - panned.min_x, WithinAbs(view.max_x - view.min_x, 1e-10));
    CHECK_THAT(panned.max_y - panned.min_y, WithinAbs(view.max_y - view.min_y, 1e-10));
    CHECK_THAT(offset > 0 ? panned.max_x : panned.min_x, WithinAbs(offset > 0 ? bounds.max_x : bounds.min_x, 1e-10));
    CHECK_THAT(offset > 0 ? panned.max_y : panned.min_y, WithinAbs(offset > 0 ? bounds.max_y : bounds.min_y, 1e-10));
}

TEST_CASE("Panning with nonfinite offsets leaves the curve view unchanged", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewPanning]")
{
    FlowModel model;
    const CurveView view = {2, 12, -0.1, 0.9};
    const double offset = GENERATE(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity());
    const bool vertical = GENERATE(false, true);
    const auto panned = model.panned_view(view, vertical ? 1.0 : offset, vertical ? offset : 1.0);
    CHECK_THAT(panned.min_x, WithinAbs(view.min_x, 1e-12));
    CHECK_THAT(panned.max_x, WithinAbs(view.max_x, 1e-12));
    CHECK_THAT(panned.min_y, WithinAbs(view.min_y, 1e-12));
    CHECK_THAT(panned.max_y, WithinAbs(view.max_y, 1e-12));
}

TEST_CASE("Panning an invalid curve view leaves its bounds unchanged", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewPanning]")
{
    FlowModel model;
    const std::vector<CurveView> views = {{-1, 10, 0, 1}, {0, 1001, 0, 1}, {0, 10, 1, 0}, {0, 0.0001, 0, 1}};
    const auto view = GENERATE_COPY(from_range(views));
    REQUIRE(model.validate_view(view) != nullptr);
    const auto panned = model.panned_view(view, 1, 0.1);
    CHECK_THAT(panned.min_x, WithinAbs(view.min_x, 1e-12));
    CHECK_THAT(panned.max_x, WithinAbs(view.max_x, 1e-12));
    CHECK_THAT(panned.min_y, WithinAbs(view.min_y, 1e-12));
    CHECK_THAT(panned.max_y, WithinAbs(view.max_y, 1e-12));
}

TEST_CASE("Zooming curve views preserves the cursor anchor and model values", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    const std::vector<std::string> parameters = {"0,0", "\n10,1"};
    FlowModel model(parameters);
    const CurveView view = {100, 200, 0, 1};
    const bool vertical = GENERATE(false, true);
    const double factor = GENERATE(0.5, 1.5, std::pow(1.1, -0.25));
    const double anchor = GENERATE(0.0, 0.25, 0.75, 1.0);
    const auto zoomed = model.zoomed_view(view, factor, anchor, vertical);
    REQUIRE(model.validate_view(zoomed) == nullptr);
    const double minimum = vertical ? view.min_y : view.min_x;
    const double span = vertical ? view.max_y - view.min_y : view.max_x - view.min_x;
    const double zoomed_minimum = vertical ? zoomed.min_y : zoomed.min_x;
    const double zoomed_span = vertical ? zoomed.max_y - zoomed.min_y : zoomed.max_x - zoomed.min_x;
    CHECK_THAT(zoomed_span, WithinAbs(span * factor, 1e-10));
    CHECK_THAT(zoomed_minimum + zoomed_span * anchor, WithinAbs(minimum + span * anchor, 1e-10));
    CHECK_THAT(vertical ? zoomed.min_x : zoomed.min_y, WithinAbs(vertical ? view.min_x : view.min_y, 1e-12));
    CHECK_THAT(vertical ? zoomed.max_x : zoomed.max_y, WithinAbs(vertical ? view.max_x : view.max_y, 1e-12));
    CHECK(model.parameters() == parameters);
    CHECK(model.serialized_parameters() == CurveModel::serialize_parameters(parameters));
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Zooming near curve boundaries shifts the interval to keep its requested scale", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    FlowModel model;
    const CurveView view = {0, 10, 1, 2};
    const bool vertical = GENERATE(false, true);
    const double anchor = GENERATE(0.0, 0.5, 1.0);
    const auto zoomed = model.zoomed_view(view, 2, anchor, vertical);
    REQUIRE(model.validate_view(zoomed) == nullptr);
    if (vertical) {
        CHECK_THAT(zoomed.min_y, WithinAbs(0.0, 1e-12));
        CHECK_THAT(zoomed.max_y, WithinAbs(2.0, 1e-12));
        CHECK_THAT(zoomed.min_x, WithinAbs(view.min_x, 1e-12));
        CHECK_THAT(zoomed.max_x, WithinAbs(view.max_x, 1e-12));
    } else {
        CHECK_THAT(zoomed.min_x, WithinAbs(0.0, 1e-12));
        CHECK_THAT(zoomed.max_x, WithinAbs(20.0, 1e-12));
        CHECK_THAT(zoomed.min_y, WithinAbs(view.min_y, 1e-12));
        CHECK_THAT(zoomed.max_y, WithinAbs(view.max_y, 1e-12));
    }
}

TEST_CASE("Zooming curve views stops at minimum and maximum spans", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    FlowModel model;
    const auto limits = model.view_limits();
    const std::vector<CurveView> views = {{0, 10, 0, 1}, {0, 0.001, 0, 0.001}, {999.999, 1000, 1.999, 2}, limits.bounds};
    const auto view = GENERATE_COPY(from_range(views));
    REQUIRE(model.validate_view(view) == nullptr);
    const bool vertical = GENERATE(false, true);
    const double factor = GENERATE(std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    const double anchor = GENERATE(0.0, 0.5, 1.0);
    const auto zoomed = model.zoomed_view(view, factor, anchor, vertical);
    REQUIRE(model.validate_view(zoomed) == nullptr);
    const double span = vertical ? zoomed.max_y - zoomed.min_y : zoomed.max_x - zoomed.min_x;
    const double maximum_span = vertical ? limits.bounds.max_y - limits.bounds.min_y : limits.bounds.max_x - limits.bounds.min_x;
    CHECK_THAT(span, WithinAbs(factor < 1 ? limits.minimum_span : maximum_span, 1e-10));
    CHECK_THAT(vertical ? zoomed.min_x : zoomed.min_y, WithinAbs(vertical ? view.min_x : view.min_y, 1e-12));
    CHECK_THAT(vertical ? zoomed.max_x : zoomed.max_y, WithinAbs(vertical ? view.max_x : view.max_y, 1e-12));
}

TEST_CASE("Zooming in and out around the same cursor anchor restores the curve view", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    FlowModel model;
    const CurveView view = {100, 200, -0.25, 0.25};
    const bool vertical = GENERATE(false, true);
    const double anchor = GENERATE(0.0, 0.25, 0.75, 1.0);
    const auto restored = model.zoomed_view(model.zoomed_view(view, 0.5, anchor, vertical), 2, anchor, vertical);
    CHECK_THAT(restored.min_x, WithinAbs(view.min_x, 1e-10));
    CHECK_THAT(restored.max_x, WithinAbs(view.max_x, 1e-10));
    CHECK_THAT(restored.min_y, WithinAbs(view.min_y, 1e-10));
    CHECK_THAT(restored.max_y, WithinAbs(view.max_y, 1e-10));
}

TEST_CASE("Invalid or neutral zoom requests leave curve bounds unchanged", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    FlowModel model;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const std::vector<std::array<double, 2>> requests = {{1, 0.5}, {0, 0.5}, {-1, 0.5}, {nan, 0.5}, {infinity, 0.5},
                                                      {0.5, -0.1}, {0.5, 1.1}, {0.5, nan}, {0.5, infinity}};
    const auto request = GENERATE_COPY(from_range(requests));
    const CurveView view = {999.999, 1000, 1.999, 2};
    const bool vertical = GENERATE(false, true);
    const auto zoomed = model.zoomed_view(view, request[0], request[1], vertical);
    CHECK_THAT(zoomed.min_x, WithinAbs(view.min_x, 1e-12));
    CHECK_THAT(zoomed.max_x, WithinAbs(view.max_x, 1e-12));
    CHECK_THAT(zoomed.min_y, WithinAbs(view.min_y, 1e-12));
    CHECK_THAT(zoomed.max_y, WithinAbs(view.max_y, 1e-12));
}

TEST_CASE("Zooming invalid curve views leaves their bounds unchanged", "[SmallAreaInfillFlowCompensation][CurveModel][CurveViewZooming]")
{
    FlowModel model;
    const std::vector<CurveView> views = {{-1, 10, 0, 1}, {0, 1001, 0, 1}, {0, 10, 1, 0}, {0, 0.0001, 0, 1}};
    const auto view = GENERATE_COPY(from_range(views));
    REQUIRE(model.validate_view(view) != nullptr);
    const bool vertical = GENERATE(false, true);
    const auto zoomed = model.zoomed_view(view, 0.5, 0.5, vertical);
    CHECK_THAT(zoomed.min_x, WithinAbs(view.min_x, 1e-12));
    CHECK_THAT(zoomed.max_x, WithinAbs(view.max_x, 1e-12));
    CHECK_THAT(zoomed.min_y, WithinAbs(view.min_y, 1e-12));
    CHECK_THAT(zoomed.max_y, WithinAbs(view.max_y, 1e-12));
}

TEST_CASE("Reading invalid visible bounds preserves the current viewport", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model;
    const std::string input = GENERATE(as<std::string>{}, "", "letters", "nan", "inf", "1e9999", "1,2,3");
    const size_t index = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3));
    std::array<std::string, 4> bounds = {"0", "10", "0", "1"};
    bounds[index] = input;
    CurveView current = {2, 20, -0.1, 1.5};
    CHECK(model.read_view(bounds, current) != nullptr);
    CHECK_THAT(current.min_x, WithinAbs(2.0, 1e-12));
    CHECK_THAT(current.max_x, WithinAbs(20.0, 1e-12));
    CHECK_THAT(current.min_y, WithinAbs(-0.1, 1e-12));
    CHECK_THAT(current.max_y, WithinAbs(1.5, 1e-12));
}

TEST_CASE("Reading numeric visible bounds validates the complete range before applying it", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model({"0,0", "\n10,1"});
    CurveView view;
    REQUIRE(model.read_view({"0", "10,5", "-0,1", "+1.5e0"}, view) == nullptr);
    CHECK_THAT(view.max_x, WithinAbs(10.5, 1e-12));
    CHECK_THAT(view.min_y, WithinAbs(-0.1, 1e-12));
    CHECK_THAT(view.max_y, WithinAbs(1.5, 1e-12));

    const std::vector<std::array<std::string, 4>> invalid = {
        {"0", "1001", "0", "1"}, {"0", "10", "-2", "1"},
        {"10", "10", "0", "1"}, {"0", "10", "1", "0"},
        {"0", "0.0001", "0", "1"},
    };
    const auto bounds = GENERATE_COPY(from_range(invalid));
    CHECK(model.read_view(bounds, view) != nullptr);
    CHECK_THAT(view.max_x, WithinAbs(10.5, 1e-12));
    CHECK_THAT(view.min_y, WithinAbs(-0.1, 1e-12));
    CHECK_THAT(view.max_y, WithinAbs(1.5, 1e-12));
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Fitting an oversized legacy curve includes every point without changing the model", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::vector<std::string> parameters = {"0,-3", "\n100000000,1"};
    FlowModel model(parameters);
    const std::vector<double> x = {0, 100000000}, y = {-3, 1};
    int row = -1;
    REQUIRE(model.validate_points(x, y, row) == nullptr);
    const auto view = model.fitted_view(x, y);
    CHECK(model.validate_view(view) == nullptr);
    CHECK_THAT(view.max_x, WithinAbs(110000000.0, 1e-7));
    CHECK_THAT(view.min_y, WithinAbs(-1.0, 1e-12));
    CHECK(model.parameters() == parameters);
    CHECK_FALSE(model.is_modified());
    CHECK(model.validate_view(model.fitted_view({0, 1e-10}, {0, 1})) == nullptr);
    CHECK(model.validate_view(model.fitted_view({}, {})) == nullptr);
}

TEST_CASE("The flow editor derives its navigation range from the bed diagonal", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const double width = GENERATE(180.0, 256.0, 1000.0);
    const double height = GENERATE(180.0, 250.0, 600.0);
    const double diagonal = std::hypot(width, height);
    const std::vector<std::string> parameters = {"0,0", "\n10,1"};
    FlowModel model(parameters, diagonal);
    CHECK_THAT(model.view_limits().bounds.max_x, WithinAbs(diagonal * 1.1, 1e-10));
    CHECK(model.validate_view(model.view_limits().bounds) == nullptr);
    CHECK(model.parameters() == parameters);
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Points beyond the bed diagonal remain visible and valid", "[SmallAreaInfillFlowCompensation][Regression]")
{
    const std::vector<std::string> parameters = {"0,0", "\n1500,1"};
    FlowModel model(parameters, 300.0);
    std::vector<double> x, y;
    int row = -1;
    REQUIRE(model.read_points(model.rows(), x, y, row) == nullptr);
    const auto view = model.fitted_view(x, y);
    CHECK(view.max_x > x.back());
    CHECK(model.validate_view(view) == nullptr);
    CHECK(model.parameters() == parameters);
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Longer edited points expand the viewport limits without shrinking the current view", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model({"0,0", "\n10,1"}, 300.0);
    CurveModel& editor_model = model;
    const CurveView current = {300, 330, 0, 1};
    REQUIRE(model.validate_view(current) == nullptr);
    CHECK(editor_model.expand_view_limits({0, 1500}));
    CHECK_THAT(model.view_limits().bounds.max_x, WithinAbs(1650.0, 1e-10));
    CHECK(model.validate_view(current) == nullptr);
    CHECK_FALSE(editor_model.expand_view_limits({0, 10}));
    CHECK_THAT(model.view_limits().bounds.max_x, WithinAbs(1650.0, 1e-10));
    CHECK(model.fitted_view({0, 1500}, {0, 1}).max_x > 1500);
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Missing or invalid bed dimensions retain a usable flow editor range", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    const double diagonal = GENERATE(0.0, -300.0, std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::infinity());
    FlowModel model({}, diagonal);
    CHECK_THAT(model.view_limits().bounds.max_x, WithinAbs(1000.0, 1e-12));
    CHECK(model.validate_view(model.fitted_view({}, {})) == nullptr);
}

TEST_CASE("Invalid lengths do not expand the flow viewport and extreme finite lengths do not overflow it", "[SmallAreaInfillFlowCompensation][CurveModel]")
{
    FlowModel model({}, 300.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK_FALSE(model.expand_view_limits({nan, infinity, -infinity, -1, 0}));
    CHECK_THAT(model.view_limits().bounds.max_x, WithinAbs(330.0, 1e-10));
    CHECK(model.expand_view_limits({std::numeric_limits<double>::max()}));
    CHECK(std::isfinite(model.view_limits().bounds.max_x));
    CHECK(model.validate_view(model.view_limits().bounds) == nullptr);
    CHECK_FALSE(model.is_modified());
}

TEST_CASE("Flow model changes are reported as one complete preset option", "[SmallAreaInfillFlowCompensation][PresetDiff][Regression]")
{
    const std::string key = "small_area_infill_flow_compensation_model";
    const std::vector<std::string> parameters = {"0,0", "\n5,0.5", "\n10,1"};
    const std::vector<std::vector<std::string>> edits = {
        {"0,0.1", parameters[1], parameters[2]},
        {parameters[0], "\n5,0.6", parameters[2]},
        {parameters[0], parameters[1], "\n12,1"},
        {parameters[0], parameters[1]},
        {parameters[0], parameters[2]},
        {},
        {parameters[0], "\n2,0.2", parameters[1], parameters[2]},
    };
    const size_t index = GENERATE(size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(6));
    const bool deep_compare = GENERATE(false, true);
    CAPTURE(index, deep_compare);
    Preset saved(Preset::TYPE_PRINT, "saved");
    auto* option = new ConfigOptionStrings;
    option->values = parameters;
    saved.config.set_key_value(key, option);
    Preset edited = saved;
    edited.config.option<ConfigOptionStrings>(key)->values = edits[index];

    const auto dirty = PresetCollection::dirty_options(&edited, &saved, deep_compare);
    REQUIRE(dirty == std::vector<std::string>{key});
    CHECK(PresetCollection::is_dirty(&edited, &saved));

    DynamicPrintConfig transferred = saved.config;
    transferred.apply_only(edited.config, dirty);
    CHECK(transferred.option<ConfigOptionStrings>(key)->values == edits[index]);

    edited.config.apply_only(saved.config, dirty);
    CHECK(PresetCollection::dirty_options(&edited, &saved, deep_compare).empty());
    CHECK_FALSE(PresetCollection::is_dirty(&edited, &saved));
}
