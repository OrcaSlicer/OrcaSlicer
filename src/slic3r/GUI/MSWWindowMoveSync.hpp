#pragma once

#ifdef __WXMSW__
#include <windows.h>
#include <dwmapi.h>

namespace Slic3r { namespace GUI {

// A large child-window hierarchy can produce positions faster than the compositor
// consumes them. Wait between native moves, before Windows retrieves fresh input.
class MSWWindowMoveSync
{
public:
    bool on_message(HWND window, UINT message, LPARAM lparam)
    {
        if (m_sync_message != 0 && message == m_sync_message) {
            m_posted = false;
            synchronize_pending_move();
            return true;
        }

        switch (message) {
        case WM_ENTERSIZEMOVE:
            m_in_move_size = true;
            m_moving = false;
            m_pending = false;
            m_sync_available = m_sync_message != 0;
            break;
        case WM_MOVING:
            if (m_in_move_size)
                m_moving = true;
            break;
        case WM_WINDOWPOSCHANGED:
            if (m_in_move_size && m_moving && m_sync_available && lparam != 0 &&
                !(reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_NOMOVE)) {
                m_pending = true;
                // Waiting in WM_MOVING ages the already-calculated rectangle.
                // Waiting in WM_MOVE/WINDOWPOSCHANGED blocks an unfinished native
                // hierarchy update. A posted message runs after that update returns
                // to the modal loop, ahead of the next coalesced mouse move.
                if (!m_posted) {
                    m_posted = ::PostMessageW(window, m_sync_message, 0, 0) != FALSE;
                    if (!m_posted)
                        m_sync_available = false;
                }
            }
            break;
        case WM_EXITSIZEMOVE:
            m_in_move_size = false;
            m_moving = false;
            synchronize_pending_move();
            break;
        }
        return false;
    }

private:
    void synchronize_pending_move()
    {
        // If composition is unavailable, keep normal native movement for the rest
        // of this drag.
        const bool pending = m_pending;
        m_pending = false;
        if (pending && m_sync_available && FAILED(::DwmFlush()))
            m_sync_available = false;
    }

    const UINT m_sync_message = ::RegisterWindowMessageW(L"OrcaSlicer.NativeMoveSync");
    bool m_in_move_size = false;
    bool m_moving = false;
    bool m_sync_available = false;
    bool m_pending = false;
    bool m_posted = false;
};

}} // namespace Slic3r::GUI
#endif
