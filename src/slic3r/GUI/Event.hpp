#ifndef slic3r_Events_hpp_
#define slic3r_Events_hpp_

#include <array>
#include <wx/debug.h>
#include <wx/event.h>


namespace Slic3r {

namespace GUI {


struct SimpleEvent : public wxEvent
{
    SimpleEvent(wxEventType type, wxObject* origin = nullptr) : wxEvent(0, type)
    {
        m_propagationLevel = wxEVENT_PROPAGATE_MAX;
        SetEventObject(origin);
    }

    virtual wxEvent* Clone() const
    {
        return new SimpleEvent(GetEventType(), GetEventObject());
    }
};

struct IntEvent : public wxEvent
{
public:
    IntEvent(wxEventType type, int data, wxObject* origin = nullptr) : wxEvent(0, type)
    {
        m_propagationLevel = wxEVENT_PROPAGATE_MAX;
        SetEventObject(origin);
        m_data = data;
    }

    virtual wxEvent* Clone() const
    {
        return new IntEvent(GetEventType(), m_data, GetEventObject());
    }
    int get_data() { return m_data; }

private:
    int m_data;
    
};

template<class T, size_t N> struct ArrayEvent : public wxEvent
{
    std::array<T, N> data;

    ArrayEvent(wxEventType type, std::array<T, N> data, wxObject* origin = nullptr)
        : wxEvent(0, type), data(std::move(data))
    {
        m_propagationLevel = wxEVENT_PROPAGATE_MAX;
        SetEventObject(origin);
    }

    virtual wxEvent* Clone() const
    {
        return new ArrayEvent<T, N>(GetEventType(), data, GetEventObject());
    }
};

template<class T> struct Event : public wxEvent
{
    T data;

    Event(wxEventType type, const T &data, wxObject* origin = nullptr)
        : wxEvent(0, type), data(std::move(data))
    {
        m_propagationLevel = wxEVENT_PROPAGATE_MAX;
        SetEventObject(origin);
    }

    Event(wxEventType type, T&& data, wxObject* origin = nullptr)
        : wxEvent(0, type), data(std::move(data))
    {
        m_propagationLevel = wxEVENT_PROPAGATE_MAX;
        SetEventObject(origin);
    }

    virtual wxEvent* Clone() const
    {
        return new Event<T>(GetEventType(), data, GetEventObject());
    }
};


class LoadPrinterViewEvent  : public wxCommandEvent
{
public:
    LoadPrinterViewEvent(wxEventType commandType = wxEVT_NULL, int winid = 0)
        : wxCommandEvent(commandType, winid)
        {  }

    LoadPrinterViewEvent(const LoadPrinterViewEvent& event)
        : wxCommandEvent(event)
        { m_APIkey = event.m_APIkey; }

    const wxString& GetAPIkey() const { return m_APIkey; }
    void SetAPIkey(const wxString& apikey) { m_APIkey = apikey; }

    virtual wxEvent *Clone() const wxOVERRIDE { return new LoadPrinterViewEvent(*this); }

private:
    wxString m_APIkey;

};
}
}

#endif // slic3r_Events_hpp_
