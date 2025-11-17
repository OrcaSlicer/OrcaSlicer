#include "FlushVolPredictor.hpp"
#include "Utils.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include <optional>

namespace FlushPredict
{
    static double rad_to_deg(double rad) {
        return 180.0 / M_PI * rad;
    }

    static double deg_to_rad(double deg) {
        return deg * M_PI / 180.0;
    }

    LABColor RGB2LAB(const RGBColor& color) {
        using XYZColor = std::tuple<double, double, double>;
        auto gamma = [](double x) {
            if (x > 0.04045)
                return pow((x + 0.055) / 1.055, 2.4);
            else
                return x / 12.92;
            };
        auto RGB2XYZ = [gamma](const RGBColor& color)->XYZColor {
            double R = gamma(static_cast<double>(color.r) / 255.0) * 100;
            double G = gamma(static_cast<double>(color.g) / 255.0) * 100;
            double B = gamma(static_cast<double>(color.b) / 255.0) * 100;

            double x = 0.412453 * R + 0.357580 * G + 0.180423 * B;
            double y = 0.212671 * R + 0.715160 * G + 0.072169 * B;
            double z = 0.019334 * R + 0.119193 * G + 0.950227 * B;
            return { x,y,z };
            };

        static const double XN = 95.0489;
        static const double YN = 100;
        static const double ZN = 108.8840;

        auto f = [](double t) {
            static const double threshold = 0.008856f;
            if (t > threshold)
                return pow(t, 1.0 / 3.0);
            else
                return 7.787 * t + 0.137931;
            };

        auto xyz_color = RGB2XYZ(color);
        double x = std::get<0>(xyz_color);
        double y = std::get<1>(xyz_color);
        double z = std::get<2>(xyz_color);
        double xn = f(x / XN);
        double yn = f(y / YN);
        double zn = f(z / ZN);

        double L = 116.0 * yn - 16.0;
        double A = 500.0 * (xn - yn);
        double B = 200.0 * (yn - zn);
        return LABColor(L, A, B);
    }

    float calc_color_distance(const LABColor& lab1, const LABColor& lab2)
    {
        static const double pow_25_to_7 = pow(25, 7);

        const double C1 = sqrt(lab1.a * lab1.a + lab1.b * lab1.b);
        const double C2 = sqrt(lab2.a * lab2.a + lab2.b * lab2.b);
        const double CMean = (C1 + C2) / 2.0;
        const double pow_CMean_to_7 = pow(CMean, 7);
        const double G = 0.5 * (1 - sqrt(pow_CMean_to_7 / (pow_CMean_to_7 + pow_25_to_7)));

        const double p_l1 = lab1.l;
        const double p_l2 = lab2.l;
        const double p_a1 = (1. + G) * lab1.a;
        const double p_a2 = (1. + G) * lab2.a;
        const double p_b1 = lab1.b;
        const double p_b2 = lab2.b;
        const double p_c1 = sqrt(p_a1 * p_a1 + p_b1 * p_b1);
        const double p_c2 = sqrt(p_a2 * p_a2 + p_b2 * p_b2);
        double p_h1;
        if (p_a1 == 0 && p_b1 == 0)
            p_h1 = 0;
        else {
            p_h1 = atan2(p_b1, p_a1);
            if (p_h1 < 0)
                p_h1 += M_PI * 2;
        }
        double p_h2;
        if (p_a2 == 0 && p_b2 == 0)
            p_h2 = 0;
        else {
            p_h2 = atan2(p_b2, p_a2);
            if (p_h2 < 0)
                p_h2 += M_PI * 2;
        }

        const double delta_L = p_l2 - p_l1;
        const double delta_C = p_c2 - p_c1;

        double delta_H;
        const double p_c_multi = p_c1 * p_c2;
        if (p_c_multi == 0)
            delta_H = 0;
        else {
            delta_H = p_h2 - p_h1;
            if (delta_H < -M_PI)
                delta_H += 2 * M_PI;
            else if (delta_H > M_PI)
                delta_H -= 2 * M_PI;
            delta_H = 2 * sqrt(p_c_multi) * sin(delta_H / 2.);
        }


        double p_L_mean = (p_l1 + p_l2) / 2.0;
        double p_C_mean = (p_c1 + p_c2) / 2.0;

        double p_H_mean, p_H_sum = p_h1 + p_h2;
        if (p_c1 * p_c2 == 0) {
            p_H_mean = p_H_sum;
        }
        else {
            if (fabs(p_h1 - p_h2) <= M_PI)
                p_H_mean = p_H_sum / 2;
            else {
                if (p_H_sum < 2 * M_PI)
                    p_H_mean = (p_H_sum + 2 * M_PI) / 2.0;
                else
                    p_H_mean = (p_H_sum - 2 * M_PI) / 2.0;
            }
        }

        const double T = 1 - 0.17 * cos(p_H_mean - deg_to_rad(30)) + 0.24 * cos(2 * p_H_mean) + 0.32 * cos(3 * p_H_mean + deg_to_rad(6)) - 0.2 * cos(4 * p_H_mean - deg_to_rad(63));
        const double dtheta = deg_to_rad(30) * exp(-pow((p_H_mean - deg_to_rad(275)) / deg_to_rad(25), 2));

        const double pow_p_cmean_to_7 = pow(p_C_mean, 7);
        const double R_C = 2 * sqrt(pow_p_cmean_to_7 / (pow_p_cmean_to_7 + pow_25_to_7));

        const double pow_p_lmean_to_2 = pow(p_L_mean - 50, 2);
        const double S_L = 1 + (0.015 * pow_p_lmean_to_2) / sqrt(20 + pow_p_lmean_to_2);
        const double S_C = 1 + 0.045 * p_C_mean;
        const double S_H = 1 + 0.015 * p_C_mean * T;
        const double R_T = -sin(2 * dtheta) * R_C;

        const double K_L = 1.0, K_C = 1.0, K_H = 1.0;

        double de = sqrt(
            pow(delta_L / (K_L * S_L), 2) + pow(delta_C / (K_C * S_C), 2) + pow(delta_H / (K_H * S_H), 2) + (R_T * (delta_C / (K_C * S_C)) * (delta_H / (K_H * S_H)))
        );
        return de;
    }

    float calc_color_distance(const RGBColor& color1, const RGBColor& color2) {
        LABColor lab1 = RGB2LAB(color1);
        LABColor lab2 = RGB2LAB(color2);
        return calc_color_distance(lab1, lab2);
    }

    bool is_similar_color(const RGBColor& from, const RGBColor& to, float distance_threshold)
    {
        float color_distance = calc_color_distance(from, to);
        if (color_distance > distance_threshold)
            return false;
        return true;
    }

}


class FlushVolPredictor
{
    using RGB = FlushPredict::RGBColor;
public:
    bool predict(const RGB& from,const RGB& to , float& flush);
    FlushVolPredictor(const std::string& data_file);
    int get_min_flush_volume();
    FlushVolPredictor() = default;
private:
    uint64_t generate_hash_key(const RGB& from, const RGB& to);
    std::unordered_map<uint64_t, float> m_flush_map;
    std::vector<RGB> m_colors;
    bool m_valid{ false };
};

int FlushVolPredictor::get_min_flush_volume()
{
    if(!m_valid)
        return std::numeric_limits<int>::max();
    return static_cast<int>(std::min_element(m_flush_map.begin(), m_flush_map.end(), [](const auto& a, const auto& b) {return a.second < b.second; })->second);
}

uint64_t FlushVolPredictor::generate_hash_key(const RGB& from, const RGB& to)
{
    uint64_t key = 0;
    key |= (static_cast<uint64_t>(from.r) << 40);
    key |= (static_cast<uint64_t>(from.g) << 32);
    key |= (static_cast<uint64_t>(from.b) << 24);
    key |= (static_cast<uint64_t>(to.r) << 16);
    key |= (static_cast<uint64_t>(to.g) << 8);
    key |= static_cast<uint64_t>(to.b);
    return key;
}

FlushVolPredictor::FlushVolPredictor(const std::string& data_file)
{
    auto rgb_hex_to_dec = [](const std::string& hexstr, FlushPredict::RGBColor& color)->bool
        {
            if (hexstr.empty() || hexstr.length() != 7 || hexstr[0] != '#')
            {
                assert(false);
                color.r = 0, color.g = 0, color.b = 0;
                return false;
            }

            auto hexToByte = [](const std::string& hex)->int
                {
                    unsigned int byte;
                    std::istringstream(hex) >> std::hex >> byte;
                    return byte;
                };
            color.r = hexToByte(hexstr.substr(1, 2));
            color.g = hexToByte(hexstr.substr(3, 2));
            color.b = hexToByte(hexstr.substr(5, 2));
            return true;
        };

    std::ifstream in(data_file);
    if (!in.is_open()) {
        m_valid = false;
        return;
    }
    std::string line;
    std::getline(in, line); //skip color description line
    std::getline(in, line);
    // read and save color lists
    {
        std::istringstream in(line);
        std::string color;
        while (in >> color) {
            RGB c;
            if (!rgb_hex_to_dec(color, c)) {
                m_valid = false;
                return;
            }
            m_colors.emplace_back(c);
        }
    }
    std::getline(in, line); // skip colume name line
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string rgb_from, rgb_to;
        float value;
        if (iss >> rgb_from >> rgb_to >> value) {
            RGB from,to;
            // transfer hex str to rgb format
            if (!rgb_hex_to_dec(rgb_from, from)) {
                m_valid = false;
                return;
            }
            if (!rgb_hex_to_dec(rgb_to, to)) {
                m_valid = false;
                return;
            }
            // generate hash key for two rgb color
            uint64_t key = generate_hash_key(from,to);
            m_flush_map.emplace(key, value);
        }
        else {
            m_valid = false;
            return;
        }
    }
    m_valid = true;
}

bool FlushVolPredictor::predict(const RGB& from, const RGB& to, float& flush)
{
    if (!m_valid)
        return false;

    // find similar colors in color list
    std::optional<RGB> similar_from, similar_to;
    for (auto& color : m_colors) {
        if (FlushPredict::is_similar_color(color, from)) {
            similar_from = color;
            break;
        }
    }
    for (auto& color : m_colors) {
        if (FlushPredict::is_similar_color(color, to)) {
            similar_to = color;
            break;
        }
    }

    // `from` and `to` should have similar colors in list
    if (!similar_from || !similar_to)
        return false;

    uint64_t key = generate_hash_key(*similar_from,*similar_to);
    auto iter = m_flush_map.find(key);
    if (iter == m_flush_map.end())
        return false;

    flush = iter->second;
    return true;
}


static std::unordered_map<int, FlushVolPredictor> predictor_instances;

GenericFlushPredictor::GenericFlushPredictor(const int dataset_value)
{
    auto iter = predictor_instances.find(dataset_value);
    if (iter != predictor_instances.end())
        predictor = &iter->second;
    else {
        std::string path = Slic3r::resources_dir();
        if (dataset_value == 0)
            path += "/flush/flush_data_standard.txt";
        else if (dataset_value == 1)
            path += "/flush/flush_data_dual_standard.txt";
        else if (dataset_value == 2)
            path += "/flush/flush_data_dual_highflow.txt";
        predictor_instances[dataset_value] = FlushVolPredictor(path);

        predictor = &predictor_instances[dataset_value];
    }
}


bool GenericFlushPredictor::predict(const RGB& from, const RGB& to, float& flush)
{
    if (!predictor)
        return false;
    return predictor->predict(from, to, flush);
}

int GenericFlushPredictor::get_min_flush_volume()
{
    if (!predictor)
        return std::numeric_limits<int>::max();
    return predictor->get_min_flush_volume();
}