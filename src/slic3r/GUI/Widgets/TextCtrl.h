#ifdef __WXMSW__
class TextCtrl : public wxTextCtrl
{
public:
    using wxTextCtrl::wxTextCtrl;
    WXHBRUSH DoMSWControlColor(WXHDC pDC, wxColour colBg, WXHWND hWnd) { return wxTextCtrl::DoMSWControlColor(pDC, wxColour(), hWnd); }
};
#else
typedef wxTextCtrl TextCtrl;
#endif
