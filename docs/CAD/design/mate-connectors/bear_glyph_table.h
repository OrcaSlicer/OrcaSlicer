// Emitted by doc/design/mate-connectors/emit_glyph_table.py from bear.step — do not hand-edit.
// Normalised to the part's bounding span and centred: the renderer scales by one radius.
static const Vec2d kBearOutline[] = {        // 12 verts, RDP eps 0.030, CCW
    {+0.3842, +0.3294}, {+0.3156, +0.4002}, {+0.2424, +0.3294},
    {-0.2524, +0.3294}, {-0.3377, +0.3877}, {-0.3693, +0.3298},
    {-0.3256, +0.2631}, {-0.4893, -0.3337}, {-0.3960, -0.4002},
    {+0.4151, -0.4002}, {+0.5000, -0.3154}, {+0.3156, +0.2631},
};
static const Vec2d kBearChin[] = {            // the CHIN BAR, flat. The muzzle is relief — see kBearCrest.
    {-0.2682, -0.3578}, {+0.2628, -0.3578}, {+0.2237, -0.1786},
};
// {cx, cy, r}: two eyes, then the cheek dot that carries handedness (snaporca-wi3z).
static const Vec3d kBearMarks[] = {
    {-0.1997, +0.1760, +0.0590},
    {+0.1947, +0.1760, +0.0590},
    {+0.2797, +0.0760, +0.0380},
};
// THE MUZZLE, lifted off the mesh: a tapered wedge, base quad + crest edge, 6 facets.
// This is the only feature standing along +Z and the only one still legible edge-on.
static const double kBearPlateZ = +0.0360;
static const Vec2d kBearSnoutBase[] = {   // CCW from the nose end
    {-0.0727, -0.2417},
    {+0.0630, -0.2417},
    {+0.0259, +0.1939},
    {-0.0356, +0.1939},
};
static const Vec3d kBearCrest[] = {       // nose (tall) -> tail (short)
    {-0.0048, -0.1793, +0.2073},
    {-0.0048, +0.1605, +0.1279},
};
