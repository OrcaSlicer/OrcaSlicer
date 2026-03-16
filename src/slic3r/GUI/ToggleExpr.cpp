
#include "ToggleExpr.hpp"

namespace Slic3r { namespace GUI {

std::string ToggleExpr::Leaf::get_reason(const bool inverted) const
{
    return get_prefix(inverted) + m_name + get_postfix(inverted);
}

std::string ToggleExpr::Leaf::get_prefix(const bool inverted) const
{
    if (!m_has_prefixes)
        return "";
    std::string p = inverted ? m_inverted_prefix : m_standard_prefix;
    if (!p.empty() && p.back() != ' ')
        p += " ";
    return p;
}

std::string ToggleExpr::Leaf::get_postfix(const bool inverted) const
{
    if (m_disable_postfix)
        return "";
    if (!m_comparison_val.empty() && m_comp_type != CompareType::NO_CT) {
        return " " + comparison_type_to_string(m_comp_type, !inverted) + " " + m_comparison_val;
    }
    std::string p = inverted ? m_inverted_postfix : m_standard_postfix;
    if (!p.empty() && p.front() != ' ')
        p.insert(p.begin(), ' ');
    return p;
}

std::pair<bool, std::set<std::string>> ToggleExpr::Node::evaluate(const bool inverted) const
{
    struct EvalVisitor
    {
        bool inverted;
        std::pair<bool, std::set<std::string>> operator()(const And& data_and) const
        {
            auto [lhs_val, lhs_reasons] = data_and.lhs->evaluate(inverted);
            auto [rhs_val, rhs_reasons] = data_and.rhs->evaluate(inverted);

            bool result = lhs_val && rhs_val;
            // Result was a success, ignore any reason strings
            if (result)
                return {result, {}};

            std::set<std::string> reasons;
            if (!lhs_val)
                reasons.merge(lhs_reasons);
            if (!rhs_val)
                reasons.merge(rhs_reasons);
            return {result, std::move(reasons)};
        }
        std::pair<bool, std::set<std::string>> operator()(const Or& data_or) const
        {
            auto [lhs_val, lhs_reasons] = data_or.lhs->evaluate(inverted);
            auto [rhs_val, rhs_reasons] = data_or.rhs->evaluate(inverted);

            bool result = lhs_val || rhs_val;

            std::set<std::string> reasons;
            if (!lhs_val) {
                reasons.merge(lhs_reasons);
            }
            if (!rhs_val)
                reasons.merge(rhs_reasons);
            return {result, std::move(reasons)};
        }
        std::pair<bool, std::set<std::string>> operator()(const Not& data_not) const
        {
            return data_not.child->evaluate(!inverted);
        }
        std::pair<bool, std::set<std::string>> operator()(const Leaf& data_leaf) const
        {
            bool value = inverted ? !data_leaf.m_value : data_leaf.m_value;

            std::set<std::string> reasons;
            if (!value)
                reasons = {data_leaf.get_reason(inverted)};

            return {value, std::move(reasons)};
        }
    };
    EvalVisitor obj{inverted};
    return std::visit(obj, data);
}

std::string ToggleExpr::comparison_type_to_string(const CompareType type, const bool inverted)
{
    if (!inverted) {
        switch (type) {
        case CompareType::GT: return ">";
        case CompareType::LT: return "<";
        case CompareType::GTE: return ">=";
        case CompareType::LTE: return "<=";
        case CompareType::EQ: return "==";
        case CompareType::NEQ: return "!=";
        default: return "";
        }
    } else {
        switch (type) {
        case CompareType::GT: return "<=";
        case CompareType::LT: return ">=";
        case CompareType::GTE: return "<";
        case CompareType::LTE: return ">";
        case CompareType::EQ: return "!=";
        case CompareType::NEQ: return "==";
        default: return "";
        }
    }
}

ToggleExpr ToggleExpr::FromConfigBool(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx)
{
    auto val = opt_idx == -1 ? config->opt_bool(opt_key) : config->opt_bool(opt_key, opt_idx);
    return {val, opt_key};
}

ToggleExpr ToggleExpr::FromConfigInt(
    const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, int comp_val, unsigned opt_idx)
{
    auto val = opt_idx == -1 ? config->opt_int(opt_key) : config->opt_int(opt_key, opt_idx);
    return ToggleExpr(compare(val, comp_type, comp_val), opt_key).set_comparison(comp_type, std::to_string(comp_val));
}

ToggleExprFragment<int> ToggleExpr::FromConfigInt(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx)
{
    return {config, opt_key, opt_idx};
}

ToggleExpr ToggleExpr::FromConfigFloat(
    const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, double comp_val, unsigned opt_idx)
{
    auto val = opt_idx == -1 ? config->opt_float(opt_key) : config->opt_float(opt_key, opt_idx);
    return ToggleExpr(compare(val, comp_type, comp_val), opt_key).set_comparison(comp_type, std::to_string(comp_val));
}

ToggleExprFragment<double> ToggleExpr::FromConfigFloat(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx)
{
    return {config, opt_key, opt_idx};
}

ToggleExpr ToggleExpr::FromConfigString(
    const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, const std::string& comp_val, unsigned opt_idx)
{
    auto val = opt_idx == -1 ? config->opt_string(opt_key) : config->opt_string(opt_key, opt_idx);
    return ToggleExpr(compare(val, comp_type, comp_val), opt_key).set_comparison(comp_type, comp_val);
}

ToggleExprFragment<std::string> ToggleExpr::FromConfigString(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx)
{
    return {config, opt_key, opt_idx};
}

std::string ToggleExpr::build_reasons_string(std::string beginning_message, const std::set<std::string>& reasons)
{
    if (reasons.empty()) return "";
    auto message = std::move(beginning_message);
    if (!message.empty()) {
        boost::trim(message);
        message += " ";
    }
    message += "Reasons:\n";

    for (auto& reason : reasons) {
        message += reason;
        message += "\n";
    }

    return message;
}
}} // namespace Slic3r::GUI