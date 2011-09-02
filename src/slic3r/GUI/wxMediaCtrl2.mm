//
//  wxMediaCtrl2.m
//  OrcaSlicer
//
//  Created by cmguo on 2021/12/7.
//

#import "wxMediaCtrl2.h"
#import "wx/mediactrl.h"
#include <boost/log/trivial.hpp>

#import <Foundation/Foundation.h>
#import "BambuPlayer/BambuPlayer.h"
#import "../Utils/NetworkAgent.hpp"

#include <stdlib.h>
#include <dlfcn.h>

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

#define BAMBU_DYNAMIC

void wxMediaCtrl2::bambu_log(void const * ctx, int level, char const * msg)
{
    if (level == 1) {
        wxString msg2(msg);
        if (msg2.EndsWith("]")) {
            int n = msg2.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
                if (msg2.SubString(n + 1, msg2.Length() - 2).ToLong(&val))
                    ctrl->m_error = (int) val;
            }
        } else if (strstr(msg, "stat_log")) {
            wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            evt.SetEventObject(ctrl);
            evt.SetString(strchr(msg, ' ') + 1);
            wxPostEvent(ctrl, evt);
        }
    } else if (level < 0) {
        wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
        ctrl->NotifyStopped();
    }
    BOOST_LOG_TRIVIAL(info) << msg;
}

wxMediaCtrl2::wxMediaCtrl2(wxWindow * parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    NSView * imageView = (NSView *) GetHandle();
    imageView.layer = [[CALayer alloc] init];
    CGColorRef color = CGColorCreateGenericRGB(0, 0, 0, 1.0f);
    imageView.layer.backgroundColor = color;
    CGColorRelease(color);
    imageView.wantsLayer = YES;
    create_player();
}

wxMediaCtrl2::~wxMediaCtrl2()
{
    BambuPlayer * player = (BambuPlayer *) m_player;
    [player dealloc];
}

void wxMediaCtrl2::create_player()
{
	auto module = Slic3r::NetworkAgent::get_bambu_source_entry();
	if (!module) {
		//not ready yet
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Network plugin not ready currently!";
		return;
	}
    Class cls = (__bridge Class) dlsym(module, "OBJC_CLASS_$_BambuPlayer");
    if (cls == nullptr) {
        m_error = -2;
        return;
    }
    NSView * imageView = (NSView *) GetHandle();
    BambuPlayer * player = [cls alloc];
    [player initWithImageView: imageView];
    [player setLogger: bambu_log withContext: this];
    m_player = player;
}

void wxMediaCtrl2::Load(wxURI url)
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}

    BambuPlayer * player = (BambuPlayer *) m_player;
    if (player) {
        [player close];
        m_error = 0;
        m_error = [player open: url.BuildURI().ToUTF8()];
    }
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

void wxMediaCtrl2::Play()
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    [player2 play];
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl2::Stop()
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    [player2 close];
    NotifyStopped();
}

void wxMediaCtrl2::NotifyStopped()
{
    if (m_state != wxMEDIASTATE_STOPPED) {
        m_state = wxMEDIASTATE_STOPPED;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

wxMediaState wxMediaCtrl2::GetState() const
{
    return m_state;
}

wxSize wxMediaCtrl2::GetVideoSize() const
{
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    if (player2) {
        NSSize size = [player2 videoSize];
        if (size.width > 0)
            const_cast<wxSize&>(m_video_size) = {(int) size.width, (int) size.height};
        return {(int) size.width, (int) size.height};
    } else {
        return {0, 0};
    }
}
