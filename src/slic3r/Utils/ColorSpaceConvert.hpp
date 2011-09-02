#ifndef slic3r_Utils_ColorSpaceConvert_hpp_
#define slic3r_Utils_ColorSpaceConvert_hpp_
#include <string>
const int CUSTOM_COLOR_COUNT = 16;

#include <tuple>

std::tuple<int, int, int> rgb_to_yuv(float r, float g, float b);
double PivotRGB(double n);
double PivotXYZ(double n);
void XYZ2RGB(float X, float Y, float Z, float* R, float* G, float* B);
void Lab2XYZ(float L, float a, float b, float* X, float* Y, float* Z);
void Lab2RGB(float L, float a, float b, float* R, float* G, float* B);
void RGB2XYZ(float R, float G, float B, float* X, float* Y, float* Z);
void XYZ2Lab(float X, float Y, float Z, float* L, float* a, float* b);
void RGB2Lab(float R, float G, float B, float* L, float* a, float* b);
void RGB2HSV(float r, float g, float b, float* h, float* s, float* v);

float DeltaE00(float l1, float a1, float b1, float l2, float a2, float b2);
float DeltaE94(float l1, float a1, float b1, float l2, float a2, float b2);
float DeltaE76(float l1, float a1, float b1, float l2, float a2, float b2);

class wxColour;
std::string color_to_string(const wxColour &color);
wxColour    string_to_wxColor(const std::string &str);
#endif /* slic3r_Utils_ColorSpaceConvert_hpp_ */
