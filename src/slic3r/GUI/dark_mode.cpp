// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#include "wx/settings.h"
#include "wx/font.h"

#include "wx/msw/colour.h"
#include "dark_mode.hpp"
#include "dark_mode/dark_mode.hpp"
#include "dark_mode/UAHMenuBar.hpp"

#include <Shlwapi.h>
#include "windowsx.h"
#include "stdlib.h"

#ifdef __GNUC__
#include <cmath>
#define WINAPI_LAMBDA WINAPI
#else
#define WINAPI_LAMBDA
#endif

#ifdef __BORLANDC__
#pragma comment(lib, "uxtheme.lib")
#endif

namespace NppDarkMode
{
	bool IsEnabled()
	{
		return g_darkModeEnabled;
	}

    bool IsSupported()
    {
        return g_darkModeSupported;
    }

    bool IsSystemMenuEnabled()
    {
        return g_SystemMenuEnabled;
    }

	COLORREF InvertLightness(COLORREF c)
	{
		WORD h = 0;
		WORD s = 0;
		WORD l = 0;
		ColorRGBToHLS(c, &h, &l, &s);

		l = 240 - l;

		COLORREF invert_c = ColorHLSToRGB(h, l, s);

		return invert_c;
	}

	COLORREF InvertLightnessSofter(COLORREF c)
	{
		WORD h = 0;
		WORD s = 0;
		WORD l = 0;
		ColorRGBToHLS(c, &h, &l, &s);

		l = std::min(240 - l, 211);
		
		COLORREF invert_c = ColorHLSToRGB(h, l, s);

		return invert_c;
	}

	COLORREF GetBackgroundColor()
	{
		return IsEnabled() ? RGB(0x2B, 0x2B, 0x2B) : wxSystemSettings::GetColour(wxSYS_COLOUR_MENUBAR).GetRGB();
	}

	COLORREF GetSofterBackgroundColor()
	{
		return IsEnabled() ? RGB(0x40, 0x40, 0x40) : RGB(0xD9, 0xD9, 0xD9); //RGB(0x78, 0x78, 0x78);
	}

	COLORREF GetTextColor()
	{
		return IsEnabled() ? RGB(0xF0, 0xF0, 0xF0) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT).GetRGB();
	}

	COLORREF GetHotTextColor()
	{
		return IsEnabled() ? RGB(0xFF, 0xFF, 0xFF) : wxSystemSettings::GetColour(wxSYS_COLOUR_ACTIVEBORDER).GetRGB();
	}

	COLORREF GetSofterTextColor()
	{
		return IsEnabled() ? RGB(0xF0, 0xF0, 0xF0) : RGB(0x64, 0x64, 0x64);
	}

	COLORREF GetDarkerTextColor()
	{
		return RGB(0xC0, 0xC0, 0xC0);
	}

	COLORREF GetEdgeColor()
	{
		return RGB(0x80, 0x80, 0x80);
	}

	HBRUSH GetBackgroundBrush()
	{
		static HBRUSH g_hbrBackground = ::CreateSolidBrush(GetBackgroundColor());
		return g_hbrBackground;
	}

	HPEN GetDarkerTextPen()
	{
		static HPEN g_hpDarkerText = ::CreatePen(PS_SOLID, 1, GetDarkerTextColor());
		return g_hpDarkerText;
	}

	HPEN GetEdgePen()
	{
		static HPEN g_hpEdgePen = ::CreatePen(PS_SOLID, 1, GetEdgeColor());
		return g_hpEdgePen;
	}

	HBRUSH GetSofterBackgroundBrush()
	{
		static HBRUSH g_hbrSofterBackground = ::CreateSolidBrush(GetSofterBackgroundColor());
		return g_hbrSofterBackground;
	}

	// handle events

	bool OnSettingChange(HWND hwnd, LPARAM lParam) // true if dark mode toggled
	{
		bool toggled = false;
		if (IsColorSchemeChangeMessage(lParam))
		{
			bool darkModeWasEnabled = g_darkModeEnabled;
			g_darkModeEnabled = _ShouldAppsUseDarkMode() && !IsHighContrast();

			NppDarkMode::RefreshTitleBarThemeColor(hwnd);

			if (!!darkModeWasEnabled != !!g_darkModeEnabled) {
				toggled = true;
				RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
			}
		}

		return toggled;
	}

	// processes messages related to UAH / custom menubar drawing.
	// return true if handled, false to continue with normal processing in your wndproc
	bool UAHWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* lr)
	{
		if (!IsEnabled())
			return false;

		static HTHEME g_menuTheme = nullptr;

		UNREFERENCED_PARAMETER(wParam);
		switch (message)
		{
		case WM_UAHDRAWMENU:
		{
			UAHMENU* pUDM = (UAHMENU*)lParam;
			RECT rc = { 0 };

			// get the menubar rect
			{
				MENUBARINFO mbi = { sizeof(mbi) };
				GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi);

				RECT rcWindow;
				GetWindowRect(hWnd, &rcWindow);

				// the rcBar is offset by the window rect
				rc = mbi.rcBar;
				OffsetRect(&rc, -rcWindow.left, -rcWindow.top);

				rc.top -= 1;
			}

			FillRect(pUDM->hdc, &rc, GetBackgroundBrush());

			*lr = 0;

			return true;
		}
		case WM_UAHDRAWMENUITEM:
		{
			UAHDRAWMENUITEM* pUDMI = (UAHDRAWMENUITEM*)lParam;

			// get the menu item string
			wchar_t menuString[256] = { 0 };
			MENUITEMINFO mii = { sizeof(mii), MIIM_STRING };
			{
				mii.dwTypeData = menuString;
				mii.cch = (sizeof(menuString) / 2) - 1;

				GetMenuItemInfo(pUDMI->um.hmenu, pUDMI->umi.iPosition, TRUE, &mii);
			}

			// get the item state for drawing

			DWORD dwFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;

			int iTextStateID = MPI_NORMAL;
			int iBackgroundStateID = MPI_NORMAL;
			{
				if ((pUDMI->dis.itemState & ODS_INACTIVE) | (pUDMI->dis.itemState & ODS_DEFAULT)) {
					// normal display
					iTextStateID = MPI_NORMAL;
					iBackgroundStateID = MPI_NORMAL;
				}
				if (pUDMI->dis.itemState & ODS_HOTLIGHT) {
					// hot tracking
					iTextStateID = MPI_HOT;
					iBackgroundStateID = MPI_HOT;
				}
				if (pUDMI->dis.itemState & ODS_SELECTED) {
					// clicked -- MENU_POPUPITEM has no state for this, though MENU_BARITEM does
					iTextStateID = MPI_HOT;
					iBackgroundStateID = MPI_HOT;
				}
				if ((pUDMI->dis.itemState & ODS_GRAYED) || (pUDMI->dis.itemState & ODS_DISABLED)) {
					// disabled / grey text
					iTextStateID = MPI_DISABLED;
					iBackgroundStateID = MPI_DISABLED;
				}
				if (pUDMI->dis.itemState & ODS_NOACCEL) {
					dwFlags |= DT_HIDEPREFIX;
				}
			}

			if (!g_menuTheme) {
				g_menuTheme = OpenThemeData(hWnd, L"Menu");
			}

			if (iBackgroundStateID == MPI_NORMAL || iBackgroundStateID == MPI_DISABLED) {
				FillRect(pUDMI->um.hdc, &pUDMI->dis.rcItem, NppDarkMode::GetBackgroundBrush());
			}
			else if (iBackgroundStateID == MPI_HOT) {
                FillRect(pUDMI->um.hdc, &pUDMI->dis.rcItem, NppDarkMode::GetSofterBackgroundBrush());
            }
            else {
				DrawThemeBackground(g_menuTheme, pUDMI->um.hdc, MENU_POPUPITEM, iBackgroundStateID, &pUDMI->dis.rcItem, nullptr);
			}
			DTTOPTS dttopts = { sizeof(dttopts) };
			if (iTextStateID == MPI_NORMAL || iTextStateID == MPI_HOT) {
				dttopts.dwFlags |= DTT_TEXTCOLOR;
				dttopts.crText = NppDarkMode::GetTextColor();
			}
			DrawThemeTextEx(g_menuTheme, pUDMI->um.hdc, MENU_POPUPITEM, iTextStateID, menuString, mii.cch, dwFlags, &pUDMI->dis.rcItem, &dttopts);

			*lr = 0;

			return true;
		}
		case WM_THEMECHANGED:
		{
			if (g_menuTheme) {
				CloseThemeData(g_menuTheme);
				g_menuTheme = nullptr;
			}
			// continue processing in main wndproc
			return false;
		}
		default:
			return false;
		}
	}

    void DrawUAHMenuNCBottomLine(HWND hWnd)
    {
        MENUBARINFO mbi = { sizeof(mbi) };
        if (!GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
        {
            return;
        }

        RECT rcClient = { 0 };
        GetClientRect(hWnd, &rcClient);
        MapWindowPoints(hWnd, nullptr, (POINT*)&rcClient, 2);

        RECT rcWindow = { 0 };
        GetWindowRect(hWnd, &rcWindow);

        OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);

        // the rcBar is offset by the window rect
        RECT rcAnnoyingLine = rcClient;
        rcAnnoyingLine.bottom = rcAnnoyingLine.top;
        rcAnnoyingLine.top--;

        HDC hdc = GetWindowDC(hWnd);
        FillRect(hdc, &rcAnnoyingLine, GetBackgroundBrush());
        ReleaseDC(hWnd, hdc);
    }

	// from DarkMode.h

	void InitDarkMode(bool set_dark_mode, bool set_sys_menu)
	{
		::InitDarkMode();
        g_SystemMenuEnabled = set_sys_menu;
        SetDarkMode(set_dark_mode);
	}

    void SetDarkMode(bool dark_mode)
	{
        g_darkModeEnabled = dark_mode;

        if (!IsSupported())
            return;

        AllowDarkModeForApp(dark_mode);
        if (g_SystemMenuEnabled && _FlushMenuThemes)
            _FlushMenuThemes();
        FixDarkScrollBar();
    }

    void SetSystemMenuForApp(bool set_sys_menu)
	{
        if (IsSupported())
            g_SystemMenuEnabled = set_sys_menu;
    }

    void AllowDarkModeForApp(bool allow)
	{
		if (IsSupported())
            ::AllowDarkModeForApp(allow);
	}

	bool AllowDarkModeForWindow(HWND hWnd, bool allow)
	{
		if (IsSupported())
            return ::AllowDarkModeForWindow(hWnd, allow);
        return false;
	}

	void RefreshTitleBarThemeColor(HWND hWnd)
	{
		if (IsSupported())
            ::RefreshTitleBarThemeColor(hWnd);
	}

	void EnableDarkScrollBarForWindowAndChildren(HWND hwnd)
	{
		if (IsSupported())
            ::EnableDarkScrollBarForWindowAndChildren(hwnd);
	}

    void SetDarkTitleBar(HWND hwnd)
    {
        if (!IsSupported())
            return;

        ::AllowDarkModeForWindow(hwnd, IsEnabled());
        ::RefreshTitleBarThemeColor(hwnd);
        SetDarkExplorerTheme(hwnd);
    }

    void SetDarkExplorerTheme(HWND hwnd)
    {
        if (IsSupported())
            SetWindowTheme(hwnd, IsEnabled() ? L"DarkMode_Explorer" : nullptr, nullptr);
    }

    void SetDarkListView(HWND hwnd)
    {
        if (!IsSupported())
            return;
        bool useDark = IsEnabled();

        if (HWND hHeader = ListView_GetHeader(hwnd)) {
            _AllowDarkModeForWindow(hHeader, useDark);
            SetWindowTheme(hHeader, useDark ? L"ItemsView" : nullptr, nullptr);
        }

        _AllowDarkModeForWindow(hwnd, useDark);
        SetWindowTheme(hwnd, L"Explorer", nullptr);
    }

    void SetDarkListViewHeader(HWND hHeader)
    {
        if (!IsSupported())
            return;
        bool useDark = IsEnabled();

        _AllowDarkModeForWindow(hHeader, useDark);
        SetWindowTheme(hHeader, useDark ? L"ItemsView" : nullptr, nullptr);
    }

    int scaled(HWND hwnd, int val)
    {
        float scale = 1.0;
        // Both GetDpiForWindow and GetDpiForSystem shall be supported since Windows 10, version 1607
        if (_GetDpiForWindow && _GetDpiForSystem)
            scale = float(_GetDpiForWindow(hwnd)) / _GetDpiForSystem();
        return std::round(scale * val);
    }

    struct ButtonData
    {
        HTHEME hTheme = nullptr;
        int iStateID = 0;
        POINT mouse_pos{ -1,-1 };

        ~ButtonData()
        {
            closeTheme();
        }

        bool ensureTheme(HWND hwnd)
        {
            if (!hTheme)
            {
                hTheme = OpenThemeData(hwnd, L"Button");
            }
            return hTheme != nullptr;
        }

        void closeTheme()
        {
            if (hTheme)
            {
                CloseThemeData(hTheme);
                hTheme = nullptr;
            }
        }
    };

    void renderButton(HWND hwnd, HDC hdc, HTHEME hTheme, int iPartID, int iStateID)
    {
        RECT rcClient = { 0 };
        DWORD nStyle = GetWindowLong(hwnd, GWL_STYLE);
/*        WCHAR szText[256] = { 0 };
        DWORD nState = static_cast<DWORD>(SendMessage(hwnd, BM_GETSTATE, 0, 0));
        DWORD uiState = static_cast<DWORD>(SendMessage(hwnd, WM_QUERYUISTATE, 0, 0));

        HFONT hFont = nullptr;
        HFONT hOldFont = nullptr;
        HFONT hCreatedFont = nullptr;
        LOGFONT lf = { 0 };
        if (SUCCEEDED(GetThemeFont(hTheme, hdc, iPartID, iStateID, TMT_FONT, &lf)))
        {
            hCreatedFont = CreateFontIndirect(&lf);
            hFont = hCreatedFont;
        }

        if (!hFont) {
            hFont = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
        }

        hOldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

        DWORD dtFlags = DT_LEFT; // DT_LEFT is 0
        dtFlags |= (nStyle & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE;
        dtFlags |= ((nStyle & BS_CENTER) == BS_CENTER) ? DT_CENTER : (nStyle & BS_RIGHT) ? DT_RIGHT : 0;
        dtFlags |= ((nStyle & BS_VCENTER) == BS_VCENTER) ? DT_VCENTER : (nStyle & BS_BOTTOM) ? DT_BOTTOM : 0;
        dtFlags |= (uiState & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;

        if (!(nStyle & BS_MULTILINE) && !(nStyle & BS_BOTTOM) && !(nStyle & BS_TOP))
        {
            dtFlags |= DT_VCENTER;
        }

        GetClientRect(hwnd, &rcClient);
        GetWindowText(hwnd, szText, _countof(szText));

        SIZE szBox = { 13, 13 };
        GetThemePartSize(hTheme, hdc, iPartID, iStateID, NULL, TS_DRAW, &szBox);

        RECT rcText = rcClient;
        GetThemeBackgroundContentRect(hTheme, hdc, iPartID, iStateID, &rcClient, &rcText);

        RECT rcBackground = rcClient;
        if (dtFlags & DT_SINGLELINE)
        {
            rcBackground.top += (rcText.bottom - rcText.top - szBox.cy) / 2;
        }
        rcBackground.bottom = rcBackground.top + szBox.cy;
        rcBackground.right = rcBackground.left + szBox.cx;
        rcText.left = rcBackground.right + 3;

        DrawThemeParentBackground(hwnd, hdc, &rcClient);
        DrawThemeBackground(hTheme, hdc, iPartID, iStateID, &rcBackground, nullptr);

        DTTOPTS dtto = { sizeof(DTTOPTS), DTT_TEXTCOLOR };
        dtto.crText = iPartID == SBP_ARROWBTN ? GetSofterTextColor() : GetTextColor();

        if (nStyle & WS_DISABLED)
        {
            dtto.crText = GetSofterBackgroundColor();
        }
*/
        GetClientRect(hwnd, &rcClient);
        DrawThemeParentBackground(hwnd, hdc, &rcClient);

        if (iPartID == SBP_ARROWBTN)
        {
            {
                HBRUSH hbrush = ::CreateSolidBrush(RGB(0x64, 0x64, 0x64));
            	::FrameRect(hdc, &rcClient, hbrush);
            	::DeleteObject(hbrush);
            }

            COLORREF color = nStyle & WS_DISABLED ? GetSofterBackgroundColor() : GetSofterTextColor();

            HPEN hPen = CreatePen(PS_SOLID, 1, color);
            HPEN hOldPen = SelectPen(hdc, hPen);

            HBRUSH hBrush = CreateSolidBrush(color);
            HBRUSH hOldBrush = SelectBrush(hdc, hBrush);

            // Up arrow
            RECT rcFocus = rcClient;
            rcFocus.bottom *= 0.5;
            rcFocus.left += 1;
            InflateRect(&rcFocus, -1, -1);

            int triangle_edge = int(0.25 * (rcClient.right - rcClient.left));

            int left_pos = triangle_edge + 1;
            int shift_from_center = 0.5 * triangle_edge;
            int bottom_pos = rcFocus.bottom - shift_from_center;
            rcFocus.bottom += 1;
            POINT vertices_up[] = { {left_pos, bottom_pos }, {left_pos + triangle_edge, bottom_pos - triangle_edge}, {left_pos + 2*triangle_edge, bottom_pos} };
            Polygon(hdc, vertices_up, 3);

            if (iStateID == ARROWBTNSTATES::ABS_UPHOT)
                DrawFocusRect(hdc, &rcFocus);

            // Down arrow
            rcFocus = rcClient;
            rcFocus.top = 0.5 * rcFocus.bottom;
            rcFocus.left += 1;
            InflateRect(&rcFocus, -1, -1);

            int top_pos = rcFocus.top + shift_from_center;
            POINT vertices_down[] = { {left_pos, top_pos }, {left_pos + triangle_edge, top_pos + triangle_edge}, {left_pos + 2 * triangle_edge, top_pos} };
            Polygon(hdc, vertices_down, 3);

            if (iStateID == ARROWBTNSTATES::ABS_DOWNHOT)
                DrawFocusRect(hdc, &rcFocus);

            SelectBrush(hdc, hOldBrush);
            DeleteObject(hBrush);

            SelectPen(hdc, hOldPen);
            DeleteObject(hPen);
        }
/*        else
            DrawThemeTextEx(hTheme, hdc, iPartID, iStateID, szText, -1, dtFlags, &rcText, &dtto);

        if ((nState & BST_FOCUS) && !(uiState & UISF_HIDEFOCUS))
        {
            RECT rcTextOut = rcText;
            dtto.dwFlags |= DTT_CALCRECT;
            DrawThemeTextEx(hTheme, hdc, iPartID, iStateID, szText, -1, dtFlags | DT_CALCRECT, &rcTextOut, &dtto);
            RECT rcFocus = rcTextOut;
            rcFocus.bottom++;
            rcFocus.left--;
            rcFocus.right++;
            DrawFocusRect(hdc, &rcFocus);
        }

        if (hCreatedFont) DeleteObject(hCreatedFont);
        SelectObject(hdc, hOldFont);
*/    }

    void paintButton(HWND hwnd, HDC hdc, ButtonData& buttonData)
    {
        DWORD nState = static_cast<DWORD>(SendMessage(hwnd, BM_GETSTATE, 0, 0));
        DWORD nStyle = GetWindowLong(hwnd, GWL_STYLE);
        DWORD nButtonStyle = nStyle & 0xF;

        int iPartID = BP_CHECKBOX;
        if (nButtonStyle == BS_CHECKBOX || nButtonStyle == BS_AUTOCHECKBOX)
        {
            iPartID = BP_CHECKBOX;
        }
        else if (nButtonStyle == BS_RADIOBUTTON || nButtonStyle == BS_AUTORADIOBUTTON)
        {
            iPartID = BP_RADIOBUTTON;
        }
        else if (nButtonStyle == BS_AUTO3STATE)
        {
            iPartID = SBP_ARROWBTN;
        }
        else
        {
            assert(false);
        }

        // states of BP_CHECKBOX and BP_RADIOBUTTON are the same
        int iStateID = RBS_UNCHECKEDNORMAL;

        if (nStyle & WS_DISABLED)		iStateID = RBS_UNCHECKEDDISABLED;
        else if (nState & BST_PUSHED)	iStateID = RBS_UNCHECKEDPRESSED;
        else if (nState & BST_HOT)		iStateID = RBS_UNCHECKEDHOT;

        if (nState & BST_CHECKED)		iStateID += 4;

        if (BufferedPaintRenderAnimation(hwnd, hdc))
        {
            return;
        }

        BP_ANIMATIONPARAMS animParams = { sizeof(animParams) };
        animParams.style = BPAS_LINEAR;
        if (iStateID != buttonData.iStateID)
        {
            GetThemeTransitionDuration(buttonData.hTheme, iPartID, buttonData.iStateID, iStateID, TMT_TRANSITIONDURATIONS, &animParams.dwDuration);
        }

        RECT rcClient = { 0 };
        GetClientRect(hwnd, &rcClient);

        HDC hdcFrom = nullptr;
        HDC hdcTo = nullptr;
        HANIMATIONBUFFER hbpAnimation = BeginBufferedAnimation(hwnd, hdc, &rcClient, BPBF_COMPATIBLEBITMAP, nullptr, &animParams, &hdcFrom, &hdcTo);
        if (hbpAnimation)
        {
            if (hdcFrom)
            {
                renderButton(hwnd, hdcFrom, buttonData.hTheme, iPartID, buttonData.iStateID);
            }
            if (hdcTo)
            {
                if (iPartID == SBP_ARROWBTN && (buttonData.iStateID == ARROWBTNSTATES::ABS_DOWNHOT || buttonData.iStateID == ARROWBTNSTATES::ABS_UPHOT) )
                    iStateID = buttonData.iStateID;
                renderButton(hwnd, hdcTo, buttonData.hTheme, iPartID, iStateID);
            }

            buttonData.iStateID = iStateID;

            EndBufferedAnimation(hbpAnimation, TRUE);
        }
        else
        {
            renderButton(hwnd, hdc, buttonData.hTheme, iPartID, iStateID);

            buttonData.iStateID = iStateID;
        }
    }

    constexpr UINT_PTR g_buttonSubclassID = 42;

    LRESULT CALLBACK ButtonSubclass(
        HWND hWnd,
        UINT uMsg,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR uIdSubclass,
        DWORD_PTR dwRefData
    )
    {
        UNREFERENCED_PARAMETER(uIdSubclass);

        auto pButtonData = reinterpret_cast<ButtonData*>(dwRefData);

        auto paint = [pButtonData](HWND hWnd, WPARAM wParam)
        {
            PAINTSTRUCT ps = { 0 };
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (!hdc)
            {
                hdc = BeginPaint(hWnd, &ps);
            }

            paintButton(hWnd, hdc, *pButtonData);

            if (ps.hdc)
            {
                EndPaint(hWnd, &ps);
            }
        };

        switch (uMsg)
        {
        case WM_UPDATEUISTATE:
            if (HIWORD(wParam) & (UISF_HIDEACCEL | UISF_HIDEFOCUS))
            {
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, ButtonSubclass, g_buttonSubclassID);
            delete pButtonData;
            break;
        case WM_ERASEBKGND:
            if (IsEnabled() && pButtonData->ensureTheme(hWnd))
            {
                return TRUE;
            }
            else
            {
                break;
            }
        case WM_THEMECHANGED:
            pButtonData->closeTheme();
            break;
        case WM_PRINTCLIENT:
        case WM_PAINT:
            if (pButtonData->ensureTheme(hWnd))
            {
                paint(hWnd, wParam);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
        {
            DWORD nStyle = GetWindowLong(hWnd, GWL_STYLE);
            DWORD nButtonStyle = nStyle & 0xF;
            if (nButtonStyle == BS_AUTO3STATE)
            {
                int xPos = GET_X_LPARAM(lParam);
                int yPos = GET_Y_LPARAM(lParam);

                RECT rcClient = { 0 };
                GetClientRect(hWnd, &rcClient);

                int iStateID = 0;
                if (xPos <= rcClient.top || xPos >= rcClient.bottom ||
                    yPos <= rcClient.left || yPos >= rcClient.right)
                    iStateID = ARROWBTNSTATES::ABS_UPNORMAL;
                else
                    iStateID = yPos > 0.5 * rcClient.bottom ? ARROWBTNSTATES::ABS_DOWNHOT : ARROWBTNSTATES::ABS_UPHOT;

                if (pButtonData->iStateID != iStateID)
                {
                    pButtonData->iStateID = iStateID;
                    paint(hWnd, wParam);
                }

                return 0;
            }
            break;
        }
        case WM_KEYUP:
        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_VSCROLL:
        {
            DWORD nState = GET_KEYSTATE_WPARAM(wParam);
            if (nState == VK_UP || nState == VK_DOWN) {
                int iStateID = nState == VK_DOWN ? ARROWBTNSTATES::ABS_DOWNHOT : ARROWBTNSTATES::ABS_UPHOT;
                if (pButtonData->iStateID != iStateID)
                {
                    pButtonData->iStateID = iStateID;
                    paint(hWnd, wParam);
                }
                return 0;
            }
            break;
        }
        case WM_SIZE:
        case WM_DESTROY:
            BufferedPaintStopAllAnimations(hWnd);
            break;
        case WM_ENABLE:
            if (IsEnabled())
            {
                // skip the button's normal wndproc so it won't redraw out of wm_paint
                LRESULT lr = DefWindowProc(hWnd, uMsg, wParam, lParam);
                InvalidateRect(hWnd, nullptr, FALSE);
                return lr;
            }
            break;
        }
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    void subclassButtonControl(HWND hwnd)
    {
        DWORD_PTR pButtonData = reinterpret_cast<DWORD_PTR>(new ButtonData());
        SetWindowSubclass(hwnd, ButtonSubclass, g_buttonSubclassID, pButtonData);
    }

    void AutoSubclassAndThemeChildControls(HWND hwndParent, bool subclass, bool theme)
    {
        if (!IsSupported())
            return;

        struct Params
        {
            const wchar_t* themeClassName = nullptr;
            bool subclass = false;
            bool theme = false;
        };

        Params p{
            IsEnabled() ? L"DarkMode_Explorer" : nullptr
            , subclass
            , theme
        };

        EnumChildWindows(hwndParent, [](HWND hwnd, LPARAM lParam) WINAPI_LAMBDA{
            auto & p = *reinterpret_cast<Params*>(lParam);
            const size_t classNameLen = 16;
            TCHAR className[classNameLen] = { 0 };
            GetClassName(hwnd, className, classNameLen);

            if (wcscmp(className, UPDOWN_CLASS) == 0)
            {
                auto nButtonStyle = ::GetWindowLongPtr(hwnd, GWL_STYLE) & 0xF;
                if (nButtonStyle == BS_AUTO3STATE && p.subclass)
                {
                    subclassButtonControl(hwnd);
                }

                return TRUE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&p));
    }

}

