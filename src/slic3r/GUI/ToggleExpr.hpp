
#ifndef ORCASLICER_TOGGLEEXPR_HPP
#define ORCASLICER_TOGGLEEXPR_HPP

namespace Slic3r::GUI {

enum class CompareType { NO_CT, GT, LT, GTE, LTE, EQ, NEQ };

template<typename T> class ToggleExprFragment;

class ToggleExpr
{
    struct Result
    {
        bool value;
        std::set<std::string> reasons;
    };

    struct Node;
    using NodePtr = std::shared_ptr<Node>;

    struct And
    {
        NodePtr lhs, rhs;
    };
    struct Or
    {
        NodePtr lhs, rhs;
    };
    struct Not
    {
        NodePtr child;
    };
    struct Leaf
    {
        bool m_value;
        std::string m_name;

        bool        m_has_prefixes{};
        std::string m_standard_prefix{};
        std::string m_inverted_prefix{};

        bool        m_disable_postfix{};
        std::string m_standard_postfix{" disabled"};
        std::string m_inverted_postfix{" enabled"};

        CompareType m_comp_type{CompareType::NO_CT};
        std::string m_comparison_val{};

        Leaf(const bool value, std::string name) : m_value(value), m_name(std::move(name)) {}

        [[nodiscard]] std::string get_reason(bool inverted) const;
    private:
        [[nodiscard]] std::string get_prefix(bool inverted) const;
        [[nodiscard]] std::string get_postfix(bool inverted) const;
    };

    struct Node
    {
        using NodeData = std::variant<And, Or, Not, Leaf>;
        NodeData data;

        explicit Node(NodeData _data) : data(std::move(_data)) {}

        std::pair<bool, std::set<std::string>> evaluate(bool inverted = false) const;
    };

    template<class... Ts>
    struct visitor : Ts... { using Ts::operator()...; };
    template<class... Ts>
    visitor(Ts...) -> visitor<Ts...>;

    NodePtr m_node;

public:
    ToggleExpr(const bool value, std::string name)
    {
        m_node = std::make_shared<Node>(Leaf{value, std::move(name)});
    }

    ToggleExpr(std::shared_ptr<Node> node) : m_node(std::move(node)) {}

    explicit operator bool() const
    {
        return get_value();
    }

    friend ToggleExpr operator&&(const ToggleExpr& lhs, const ToggleExpr& rhs)
    {
        return ToggleExpr(std::make_shared<Node>(And{lhs.m_node, rhs.m_node}));
    }

    friend ToggleExpr operator||(const ToggleExpr& lhs, const ToggleExpr& rhs)
    {
        return ToggleExpr(std::make_shared<Node>(Or{lhs.m_node, rhs.m_node}));
    }

    friend ToggleExpr operator!(const ToggleExpr& rhs)
    {
        return ToggleExpr(std::make_shared<Node>(Not{rhs.m_node}));
    }

    // For ToggleExpr objects that can be LHS
    friend ToggleExpr& operator==(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::EQ, std::move(rhs)); }
    friend ToggleExpr& operator!=(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::NEQ, std::move(rhs)); }
    friend ToggleExpr& operator>(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::GT, std::move(rhs)); }
    friend ToggleExpr& operator<(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::LT, std::move(rhs)); }
    friend ToggleExpr& operator>=(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::GTE, std::move(rhs)); }
    friend ToggleExpr& operator<=(ToggleExpr& lhs, std::string rhs) { return lhs.set_comparison(CompareType::LTE, std::move(rhs)); }

    // For newly created ToggleExpr objects
    friend ToggleExpr& operator==(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::EQ, std::move(rhs)); }
    friend ToggleExpr& operator!=(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::NEQ, std::move(rhs)); }
    friend ToggleExpr& operator>(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::GT, std::move(rhs)); }
    friend ToggleExpr& operator<(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::LT, std::move(rhs)); }
    friend ToggleExpr& operator>=(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::GTE, std::move(rhs)); }
    friend ToggleExpr& operator<=(ToggleExpr lhs, std::string rhs) { return lhs.set_comparison(CompareType::LTE, std::move(rhs)); }

    ///
    /// \param value_false_prefix When the provided value is false, what should the prefix be?
    /// \param opposite_prefix When the provided value is false when inverted ('!' operator), what should the prefix be?
    /// \return self
    ToggleExpr& set_prefixes(std::string value_false_prefix, std::string opposite_prefix)
    {
        std::visit(visitor{[](auto& data) { static_assert("Calling set_prefixes on a non leaf node"); },
                           [&](Leaf& leaf) {
                               leaf.m_standard_prefix = std::move(value_false_prefix);
                               leaf.m_inverted_prefix = std::move(opposite_prefix);
                           }},
                   m_node->data);
        return *this;
    }

    ToggleExpr& disable_postfix()
    {
        std::visit(visitor{[](auto&) { static_assert("Calling disable_postfix on a non leaf node"); },
                           [&](Leaf& leaf) { leaf.m_disable_postfix = true; }},
                   m_node->data);
        return *this;
    }

    ToggleExpr& set_postfixes(std::string value_false_postfix, std::string opposite_postfix)
    {
        std::visit(visitor{[](auto&) { static_assert("Calling set_postfixes on a non leaf node"); },
                           [&](Leaf& leaf) {
                               leaf.m_standard_postfix = std::move(value_false_postfix);
                               leaf.m_inverted_postfix = std::move(opposite_postfix);
                           }},
                   m_node->data);

        return *this;
    }

    /// Set the comparison that is occurring to generate the provided value
    /// \return self
    ToggleExpr& set_comparison(CompareType comp_type, std::string comparison_val)
    {
        std::visit(visitor{[](auto& data) { static_assert("Calling set_comparison on a non leaf node"); },
                           [&](Leaf& leaf) {
                               leaf.m_comp_type      = comp_type;
                               leaf.m_comparison_val = std::move(comparison_val);
                           }},
                   m_node->data);

        return *this;
    }

    [[nodiscard]] bool get_value() const { return m_node->evaluate().first; }

    [[nodiscard]] std::set<std::string> get_reasons() const
    {
        return m_node->evaluate().second;
    }

    [[nodiscard]] std::pair<bool, std::set<std::string>> get_result() const
    {
        return m_node->evaluate();
    }

    template<typename T> static bool compare(T value, CompareType comp_type, T comp_value)
    {
        switch (comp_type) {
        case CompareType::NO_CT: throw std::invalid_argument("ToggleExpr::FromConfig cannot accept a condition type of NO_CT");
        case CompareType::GT: return value > comp_value;
        case CompareType::LT: return value < comp_value;
        case CompareType::GTE: return value >= comp_value;
        case CompareType::LTE: return value <= comp_value;
        case CompareType::EQ: return value == comp_value;
        case CompareType::NEQ: return value != comp_value;
        }
        return false;
    }

    static std::string comparison_type_to_string(CompareType type, bool inverted);

    static ToggleExpr FromConfigBool(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx = -1);

    static ToggleExpr FromConfigInt(
        const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, int comp_val, unsigned opt_idx = -1);
    static ToggleExprFragment<int> FromConfigInt(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx = -1);

    template<typename EnumT>
    static ToggleExpr FromConfigEnum(
        const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, EnumT comp_value, unsigned opt_idx = -1)
    {
        bool ret_val;
        if (opt_idx == -1) {
            auto      enum_opt = config->option<ConfigOptionEnum<EnumT>>(opt_key);
            auto val      = enum_opt->value;
            ret_val = compare(val, comp_type, comp_value);
        } else {
            auto      enum_opt = config->option<ConfigOptionEnumsGeneric>(opt_key);
            auto val      = enum_opt->get_at(opt_idx);
            ret_val = compare(static_cast<EnumT>(val), comp_type, comp_value);
        }
        return ToggleExpr(ret_val, opt_key).set_comparison(comp_type, ConfigOptionEnum<EnumT>::get_enum_names().at(static_cast<int>(comp_value)));
    }

    template<typename EnumT>
    static ToggleExprFragment<EnumT> FromConfigEnum(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx = -1)
    { return {config, opt_key, opt_idx}; }

    static ToggleExpr FromConfigFloat(
        const DynamicPrintConfig* config, const std::string& opt_key, CompareType comp_type, double comp_val, unsigned opt_idx = -1);
    static ToggleExprFragment<double> FromConfigFloat(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx = -1);

    static ToggleExpr                      FromConfigString(const DynamicPrintConfig* config,
                                                            const std::string&        opt_key,
                                                            CompareType               comp_type,
                                                            const std::string&        comp_val,
                                                            unsigned                  opt_idx = -1);
    static ToggleExprFragment<std::string> FromConfigString(const DynamicPrintConfig* config,
                                                            const std::string&        opt_key,
                                                            unsigned                  opt_idx = -1);

    static std::string build_reasons_string(std::string beginning_message, const std::set<std::string>& reasons);
};

template<typename T> class ToggleExprFragment
{
    const DynamicPrintConfig* config;
    std::string               opt_key;
    unsigned                  opt_idx;

    ToggleExpr build_expr(CompareType comp_type, T comp_val) const
    {
        if constexpr (std::is_same_v<T, int>) {
            return ToggleExpr::FromConfigInt(config, opt_key, comp_type, comp_val, opt_idx);
        } else if constexpr (std::is_same_v<T, double>) {
            return ToggleExpr::FromConfigFloat(config, opt_key, comp_type, comp_val, opt_idx);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return ToggleExpr::FromConfigString(config, opt_key, comp_type, comp_val, opt_idx);
        } else if constexpr (std::is_enum_v<T>) {
            return ToggleExpr::FromConfigEnum(config, opt_key, comp_type, comp_val, opt_idx);
        } else {
            // Unsupported
            static_assert(false, "The provided type is unsupported by this class");
        }
        return ToggleExpr(false, "");
    }

public:
    ToggleExprFragment(const DynamicPrintConfig* config, const std::string& opt_key, unsigned opt_idx)
        : config(config), opt_key(opt_key), opt_idx(opt_idx)
    {}

    friend ToggleExpr operator==(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::EQ, rhs); }
    friend ToggleExpr operator!=(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::NEQ, rhs); }
    friend ToggleExpr operator>(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::GT, rhs); }
    friend ToggleExpr operator<(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::LT, rhs); }
    friend ToggleExpr operator>=(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::GTE, rhs); }
    friend ToggleExpr operator<=(const ToggleExprFragment& lhs, T rhs) { return lhs.build_expr(CompareType::LTE, rhs); }

    friend class ToggleExpr;
};
} // namespace Slic3r::GUI

#endif // ORCASLICER_TOGGLEEXPR_HPP
