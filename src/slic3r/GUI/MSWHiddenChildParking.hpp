#pragma once

#ifdef __WXMSW__
#include <windows.h>
#include <vector>

namespace Slic3r { namespace GUI {

// Each move of a top-level window costs the compositor time for every descendant
// window, hidden ones included. With many hidden pages, fast mice produce moves
// faster than they are composed and the window trails the cursor. While the
// frame is dragged, hidden subtrees are moved under an invisible top-level
// window and put back afterwards. wxWidgets' own parent links are not changed.
class MSWHiddenChildParking
{
public:
    void on_message(HWND frame, UINT message)
    {
        switch (message) {
        case WM_ENTERSIZEMOVE:
            m_in_move_size = true;
            m_attempted = false;
            break;
        case WM_MOVING:
            // Resizing never sends WM_MOVING, so hidden pages stay in place for layout.
            if (m_in_move_size && !m_attempted && !m_dpi_changed)
                park(frame);
            break;
        case WM_DPICHANGED:
            // Windows defers the DPI update of hidden windows until they are shown or
            // reparented. Reparenting would do that work, up to hundreds of ms for a
            // hidden page, at the start of a drag, so stop parking after a DPI change.
            restore();
            m_dpi_changed = true;
            break;
        case WM_EXITSIZEMOVE:
            m_in_move_size = false;
            restore();
            break;
        case WM_DESTROY:
            restore();
            break;
        }
    }

private:
    struct Parked
    {
        HWND window;
        HWND parent;
        HWND above; // previous sibling, to restore the z-order
    };

    void park(HWND frame)
    {
        m_attempted = true;
        std::vector<Parked> found;
        collect(frame, ::GetFocus(), found);
        if (found.empty())
            return;
        RECT rect;
        ::GetWindowRect(frame, &rect);
        // Same rectangle as the frame, so it gets the frame's monitor and DPI. Parking
        // under a window of another DPI would rescale the hidden windows both ways.
        m_park = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Static", L"", WS_POPUP, rect.left, rect.top,
                                   rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, nullptr, nullptr);
        if (m_park == nullptr)
            return;
        for (const Parked &entry : found)
            if (::SetParent(entry.window, m_park) != nullptr)
                m_parked.push_back(entry);
    }

    void restore()
    {
        // Same order as parked, so each window's previous sibling is already back.
        for (const Parked &entry : m_parked) {
            if (!::IsWindow(entry.window) || ::GetParent(entry.window) != m_park || !::IsWindow(entry.parent))
                continue;
            ::SetParent(entry.window, entry.parent);
            const bool above_valid = entry.above != nullptr && ::IsWindow(entry.above) && ::GetParent(entry.above) == entry.parent;
            ::SetWindowPos(entry.window, above_valid ? entry.above : HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        m_parked.clear();
        // A window that could not be put back must not be destroyed with the park window.
        if (m_park != nullptr && ::GetWindow(m_park, GW_CHILD) == nullptr)
            ::DestroyWindow(m_park);
        m_park = nullptr;
    }

    // Each SetParent costs about 0.3 ms whatever the subtree size, so only subtrees
    // that save at least this many windows per move are worth it (whole hidden pages).
    static constexpr int min_descendants = 32;

    // Topmost hidden windows of this thread with large subtrees, not holding the focus.
    static void collect(HWND parent, HWND focus, std::vector<Parked> &found)
    {
        for (HWND child = ::GetWindow(parent, GW_CHILD); child != nullptr; child = ::GetWindow(child, GW_HWNDNEXT)) {
            if (::GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)
                collect(child, focus, found);
            else if (::GetWindowThreadProcessId(child, nullptr) == ::GetCurrentThreadId() && child != focus &&
                     !::IsChild(child, focus) && descendants(child) >= min_descendants)
                found.push_back({child, parent, ::GetWindow(child, GW_HWNDPREV)});
        }
    }

    static int descendants(HWND root)
    {
        int count = 0;
        ::EnumChildWindows(root, [](HWND, LPARAM counter) -> BOOL { ++*reinterpret_cast<int *>(counter); return TRUE; },
                           reinterpret_cast<LPARAM>(&count));
        return count;
    }

    HWND               m_park = nullptr;
    std::vector<Parked> m_parked;
    bool               m_in_move_size = false;
    bool               m_attempted = false;
    bool               m_dpi_changed = false;
};

}} // namespace Slic3r::GUI
#endif
