#include "BackgroundSlicingProcess.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "format.hpp"

#include <wx/app.h>
#include <wx/panel.h>
#include <wx/stdpaths.h>

// For zipped archive creation
#include <wx/stdstream.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

#include <miniz.h>

// Print now includes tbb, and tbb includes Windows. This breaks compilation of wxWidgets if included before wx.
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/Format/SL1.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/libslic3r.h"

#include <cassert>
#include <stdexcept>
#include <cctype>

#include <boost/format/format_fwd.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include "I18N.hpp"
//#include "RemovableDriveManager.hpp"

#include "slic3r/GUI/Plater.hpp"

namespace Slic3r {

bool SlicingProcessCompletedEvent::critical_error() const
{
	try {
		this->rethrow_exception();
	} catch (const Slic3r::SlicingError &) {
		// Exception derived from SlicingError is non-critical.
		return false;
    } catch (const Slic3r::SlicingErrors &) {
        return false;
    } catch (...) {}
    return true;
}

bool SlicingProcessCompletedEvent::invalidate_plater() const
{
	if (critical_error())
	{
		try {
			this->rethrow_exception();
		}
		catch (const Slic3r::ExportError&) {
			// Exception thrown by copying file does not ivalidate plater
			return false;
		}
		catch (...) {
		}
		return true;
	}
	return false;
}

std::pair<std::string, std::vector<size_t>> SlicingProcessCompletedEvent::format_error_message() const
{
	std::string error;
    size_t      monospace = 0;
	try {
		this->rethrow_exception();
    } catch (const std::bad_alloc &ex) {
        wxString errmsg = GUI::from_u8(boost::format(_utf8(L("A error occurred. Maybe memory of system is not enough or it's a bug "
			                  "of the program"))).str());
        error = std::string(errmsg.ToUTF8()) + "\n" + std::string(ex.what());
    } catch (const HardCrash &ex) {
        error = GUI::format(_u8L("A fatal error occurred: \"%1%\""), ex.what()) + "\n" +
                            _u8L("Please save project and restart the program.");
    } catch (PlaceholderParserError &ex) {
		error = ex.what();
		monospace = 1;
    } catch (SlicingError &ex) {
		error = ex.what();
		monospace = ex.objectId();
    } catch (SlicingErrors &exs) {
        std::vector<size_t> ids;
        for (auto &ex : exs.errors_) {
            error     = ex.what();
            monospace = ex.objectId();
            ids.push_back(monospace);
        }
        return std::make_pair(std::move(error), ids);
    } catch (std::exception &ex) {
        error = ex.what();
    } catch (...) {
        error = "Unknown C++ exception.";
    }
    return std::make_pair(std::move(error), std::vector<size_t>{monospace});
}

BackgroundSlicingProcess::BackgroundSlicingProcess()
{
	//BBS: move this logic to part plate
#if 0
    boost::filesystem::path temp_path(wxStandardPaths::Get().GetTempDir().utf8_str().data());
    temp_path /= (boost::format(".%1%.gcode") % get_current_pid()).str();
	m_temp_output_path = temp_path.string();
#endif
}

BackgroundSlicingProcess::~BackgroundSlicingProcess()
{
	this->stop();
	this->join_background_thread();
	//BBS: move this logic to part plate
	//boost::nowide::remove(m_temp_output_path.c_str());
}

//BBS: switch the print in background slicing process
bool BackgroundSlicingProcess::switch_print_preprocess()
{
	bool result = true;

	/*switch (m_printer_tech) {
	case ptFFF: m_print = m_fff_print; break;
	case ptSLA: m_print = m_sla_print; break;
	default: assert(false); break;
	}*/
	return result;
}

//BBS: judge whether can switch the print
bool BackgroundSlicingProcess::can_switch_print()
{
	bool result = true;

	if (m_state == STATE_RUNNING)
	{
		//currently it is on slicing, judge whether the slice result is valid or not
		//if (m_current_plate->is_slice_result_valid())
		{
			result = false;
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": slicing plate's plate_id %1%, on slicing, can not switch print") % m_current_plate->get_index();
		}
	}

	return result;
}

//BBS: select the printer technology
bool BackgroundSlicingProcess::select_technology(PrinterTechnology tech)
{
	bool changed = false;
	if (m_printer_tech != tech) {
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": change the printer technology from %1% to %2%") % m_printer_tech % tech;
		m_printer_tech = tech;
		if (m_print != nullptr)
			this->reset();
		changed = true;
	}

	switch (tech) {
	case ptFFF: m_print = m_fff_print; break;
	case ptSLA: m_print = m_sla_print; break;
	default: assert(false); break;
	}
	assert(m_print != nullptr);
	return changed;
}

PrinterTechnology BackgroundSlicingProcess::current_printer_technology() const
{
	//BBS: as the m_printer is changed frequently when switch plates, use m_printer_tech directly
	return m_printer_tech;
	//return m_print->technology();
}

std::string BackgroundSlicingProcess::output_filepath_for_project(const boost::filesystem::path &project_path)
{
	assert(m_print != nullptr);
    if (project_path.empty())
        return m_print->output_filepath("");
    return m_print->output_filepath(project_path.parent_path().string(), project_path.stem().string());
}

// This function may one day be merged into the Print, but historically the print was separated
// from the G-code generator.
void BackgroundSlicingProcess::process_fff()
{
    assert(m_print == m_fff_print);
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    m_fff_print->is_BBL_printer() = preset_bundle.is_bbl_vendor();
	//BBS: add the logic to process from an existed gcode file
	if (m_print->finished()) {
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: skip slicing, to process previous gcode file")%__LINE__;
		m_fff_print->set_status(80, _utf8(L("Processing G-code from Previous file...")));
		wxCommandEvent evt(m_event_slicing_completed_id);
		// Post the Slicing Finished message for the G-code viewer to update.
		// Passing the timestamp 
		evt.SetInt((int)(m_fff_print->step_state_with_timestamp(PrintStep::psSlicingFinished).timestamp));
		wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt.Clone());

		m_temp_output_path = this->get_current_plate()->get_tmp_gcode_path();
		if (! m_export_path.empty()) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: export gcode from %2% directly to %3%")%__LINE__%m_temp_output_path %m_export_path;
		}
		else {
            if (m_upload_job.empty()) {
                m_fff_print->export_gcode_from_previous_file(m_temp_output_path, m_gcode_result, [this](const ThumbnailsParams &params) {
                    return this->render_thumbnails(params);
                });
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: export_gcode_from_previous_file from %2% finished")%__LINE__ % m_temp_output_path;
		}
	}
	else {
		//BBS: reset the gcode before reload_print in slicing_completed event processing
		//FIX the gcode rename failed issue
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: will start slicing, reset gcode_result %2% firstly")%__LINE__%m_gcode_result;
		m_gcode_result->reset();

		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: gcode_result reseted, will start print::process")%__LINE__;
		m_print->process();
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: after print::process, send slicing complete event to gui...")%__LINE__;
        if (m_current_plate->get_real_filament_map_mode(preset_bundle.project_config) < FilamentMapMode::fmmManual) {
            std::vector<int> f_maps = m_fff_print->get_filament_maps();
            m_current_plate->set_filament_maps(f_maps);
		}
		wxCommandEvent evt(m_event_slicing_completed_id);
		// Post the Slicing Finished message for the G-code viewer to update.
		// Passing the timestamp
		evt.SetInt((int)(m_fff_print->step_state_with_timestamp(PrintStep::psSlicingFinished).timestamp));
		wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt.Clone());

		//BBS: add plate index into render params
		m_temp_output_path = this->get_current_plate()->get_tmp_gcode_path();
		m_fff_print->export_gcode(m_temp_output_path, m_gcode_result, [this](const ThumbnailsParams& params) { return this->render_thumbnails(params); });
		if(m_fff_print->is_BBL_printer())
			run_post_process_scripts(m_temp_output_path, false, "File", m_temp_output_path, m_fff_print->full_print_config());

		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": export gcode finished");
	}
	if (this->set_step_started(bspsGCodeFinalize)) {
	    if (! m_export_path.empty()) {
			wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, new wxCommandEvent(m_event_export_began_id));
			if(!m_fff_print->is_BBL_printer())
				finalize_gcode();
			else
				export_gcode();
	    } else if (! m_upload_job.empty()) {
			wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, new wxCommandEvent(m_event_export_began_id));
			prepare_upload();
	    } else {
			m_print->set_status(100, _utf8(L("Slicing complete")));
	    }
		this->set_step_done(bspsGCodeFinalize);
	}
}

static void write_thumbnail(Zipper& zipper, const ThumbnailData& data)
{
    size_t png_size = 0;
    void* png_data = tdefl_write_image_to_png_file_in_memory_ex((const void*)data.pixels.data(), data.width, data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
    if (png_data != nullptr)
    {
        zipper.add_entry("thumbnail/thumbnail" + std::to_string(data.width) + "x" + std::to_string(data.height) + ".png", (const std::uint8_t*)png_data, png_size);
        mz_free(png_data);
    }
}

void BackgroundSlicingProcess::process_sla()
{
    assert(m_print == m_sla_print);
    m_print->process();
    if (this->set_step_started(bspsGCodeFinalize)) {
        if (! m_export_path.empty()) {
			wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, new wxCommandEvent(m_event_export_began_id));

            const std::string export_path = m_sla_print->print_statistics().finalize_output_path(m_export_path);

			//BBS: add plate id for thumbnail generation
            ThumbnailsList thumbnails = this->render_thumbnails(
				ThumbnailsParams{ current_print()->full_print_config().option<ConfigOptionPoints>("thumbnails")->values, true, true, true, true, 0 });

            Zipper zipper(export_path);
            m_sla_archive.export_print(zipper, *m_sla_print);																											         // true, false, true, true); // renders also supports and pad
			for (const ThumbnailData& data : thumbnails)
                if (data.is_valid())
                    write_thumbnail(zipper, data);
            zipper.finalize();

            //m_print->set_status(100, (boost::format(_utf8(L("Masked SLA file exported to %1%"))) % export_path).str());
			m_print->set_status(100, (boost::format(_utf8("Masked SLA file exported to %1%")) % export_path).str());
        } else {
			//m_print->set_status(100, _utf8(L("Slicing complete")));
			m_print->set_status(100, _utf8("Slicing complete"));
        }
        this->set_step_done(bspsGCodeFinalize);
    }
}

void BackgroundSlicingProcess::thread_proc()
{
	//BBS: thread name
	set_current_thread_name("bbl_BgSlcPcs");
    name_tbb_thread_pool_threads_set_locale();

	assert(m_print != nullptr);
	assert(m_print == m_fff_print || m_print == m_sla_print);
	std::unique_lock<std::mutex> lck(m_mutex);
	// Let the caller know we are ready to run the background processing task.
	m_state = STATE_IDLE;
	lck.unlock();
	m_condition.notify_one();
	for (;;) {
		//BBS: sometimes the state has already been set in the start function
		//assert(m_state == STATE_IDLE || m_state == STATE_CANCELED || m_state == STATE_FINISHED || m_state == STATE_STARTED);
		// Wait until a new task is ready to be executed, or this thread should be finished.
		lck.lock();
		m_condition.wait(lck, [this](){ return m_state == STATE_STARTED || m_state == STATE_EXIT; });
		if (m_state == STATE_EXIT)
			// Exiting this thread.
			break;
		// Process the background slicing task.
		m_state = STATE_RUNNING;
		//BBS: internal cancel
		m_internal_cancelled = false;
		lck.unlock();
		std::exception_ptr exception;
#ifdef _WIN32
		this->call_process_seh_throw(exception);
#else
		this->call_process(exception);
#endif
		m_print->finalize();
		lck.lock();
		m_state = m_print->canceled() ? STATE_CANCELED : STATE_FINISHED;
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": process finished, state %1%, print cancel_status %2%")%m_state %m_print->cancel_status();
		if (m_print->cancel_status() != Print::CANCELED_INTERNAL) {
			// Only post the canceled event, if canceled by user.
			// Don't post the canceled event, if canceled from Print::apply().
			SlicingProcessCompletedEvent evt(m_event_finished_id, 0,
				(m_state == STATE_CANCELED) ? SlicingProcessCompletedEvent::Cancelled :
				exception ? SlicingProcessCompletedEvent::Error : SlicingProcessCompletedEvent::Finished, exception);
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": send SlicingProcessCompletedEvent to main, status %1%")%evt.status();
			wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt.Clone());
		}
		else {
			//BBS: internal cancel
			m_internal_cancelled = true;
		}
		m_print->restart();
		lck.unlock();
		// Let the UI thread wake up if it is waiting for the background task to finish.
		m_condition.notify_one();
		// Let the UI thread see the result.
	}
	m_state = STATE_EXITED;
	lck.unlock();
	// End of the background processing thread. The UI thread should join m_thread now.
}

#ifdef _WIN32
// Only these SEH exceptions will be catched and turned into Slic3r::HardCrash C++ exceptions.
static bool is_win32_seh_harware_exception(unsigned long ex) throw() {
	return
		ex == STATUS_ACCESS_VIOLATION ||
		ex == STATUS_DATATYPE_MISALIGNMENT ||
		ex == STATUS_FLOAT_DIVIDE_BY_ZERO ||
		ex == STATUS_FLOAT_OVERFLOW ||
		ex == STATUS_FLOAT_UNDERFLOW ||
#ifdef STATUS_FLOATING_RESEVERED_OPERAND
		ex == STATUS_FLOATING_RESEVERED_OPERAND ||
#endif // STATUS_FLOATING_RESEVERED_OPERAND
		ex == STATUS_ILLEGAL_INSTRUCTION ||
		ex == STATUS_PRIVILEGED_INSTRUCTION ||
		ex == STATUS_INTEGER_DIVIDE_BY_ZERO ||
		ex == STATUS_INTEGER_OVERFLOW ||
		ex == STATUS_STACK_OVERFLOW;
}

// Rethrow some SEH exceptions as Slic3r::HardCrash C++ exceptions.
static void rethrow_seh_exception(unsigned long win32_seh_catched)
{
	if (win32_seh_catched) {
		// Rethrow SEH exception as Slicer::HardCrash.
		if (win32_seh_catched == STATUS_ACCESS_VIOLATION || win32_seh_catched == STATUS_DATATYPE_MISALIGNMENT)
			throw Slic3r::HardCrash(_u8L("Access violation"));
		if (win32_seh_catched == STATUS_ILLEGAL_INSTRUCTION || win32_seh_catched == STATUS_PRIVILEGED_INSTRUCTION)
			throw Slic3r::HardCrash(_u8L("Illegal instruction"));
		if (win32_seh_catched == STATUS_FLOAT_DIVIDE_BY_ZERO || win32_seh_catched == STATUS_INTEGER_DIVIDE_BY_ZERO)
			throw Slic3r::HardCrash(_u8L("Divide by zero"));
		if (win32_seh_catched == STATUS_FLOAT_OVERFLOW || win32_seh_catched == STATUS_INTEGER_OVERFLOW)
			throw Slic3r::HardCrash(_u8L("Overflow"));
		if (win32_seh_catched == STATUS_FLOAT_UNDERFLOW)
			throw Slic3r::HardCrash(_u8L("Underflow"));
#ifdef STATUS_FLOATING_RESEVERED_OPERAND
		if (win32_seh_catched == STATUS_FLOATING_RESEVERED_OPERAND)
			throw Slic3r::HardCrash(_u8L("Floating reserved operand"));
#endif // STATUS_FLOATING_RESEVERED_OPERAND
		if (win32_seh_catched == STATUS_STACK_OVERFLOW)
			throw Slic3r::HardCrash(_u8L("Stack overflow"));
	}
}

// Wrapper for Win32 structured exceptions. Win32 structured exception blocks and C++ exception blocks cannot be mixed in the same function.
unsigned long BackgroundSlicingProcess::call_process_seh(std::exception_ptr &ex) throw()
{
	unsigned long win32_seh_catched = 0;
	__try {
		this->call_process(ex);
	} __except (is_win32_seh_harware_exception(GetExceptionCode())) {
		win32_seh_catched = GetExceptionCode();
	}
	return win32_seh_catched;
}
void BackgroundSlicingProcess::call_process_seh_throw(std::exception_ptr &ex) throw()
{
	unsigned long win32_seh_catched = this->call_process_seh(ex);
	if (win32_seh_catched) {
		// Rethrow SEH exception as Slicer::HardCrash.
		try {
			rethrow_seh_exception(win32_seh_catched);
		} catch (...) {
			ex = std::current_exception();
		}
	}
}
#endif // _WIN32

void BackgroundSlicingProcess::call_process(std::exception_ptr &ex) throw()
{
	try {
		assert(m_print != nullptr);
		switch (m_print->technology()) {
		case ptFFF: this->process_fff(); break;
		case ptSLA: this->process_sla(); break;
		default: m_print->process(); break;
		}
	} catch (CanceledException& /* ex */) {
		// Canceled, this is all right.
		assert(m_print->canceled());
		ex = std::current_exception();
		BOOST_LOG_TRIVIAL(error) <<__FUNCTION__ << ":got cancelled exception" << std::endl;
	} catch (...) {
		ex = std::current_exception();
		BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ":got other exception" << std::endl;
	}
}

#ifdef _WIN32
unsigned long BackgroundSlicingProcess::thread_proc_safe_seh() throw()
{
	unsigned long win32_seh_catched = 0;
	__try {
		this->thread_proc_safe();
	} __except (is_win32_seh_harware_exception(GetExceptionCode())) {
		win32_seh_catched = GetExceptionCode();
	}
	return win32_seh_catched;
}
void BackgroundSlicingProcess::thread_proc_safe_seh_throw() throw()
{
	unsigned long win32_seh_catched = this->thread_proc_safe_seh();
	if (win32_seh_catched) {
		// Rethrow SEH exception as Slicer::HardCrash.
		try {
			rethrow_seh_exception(win32_seh_catched);
		} catch (...) {
			wxTheApp->OnUnhandledException();
		}
	}
}
#endif // _WIN32

void BackgroundSlicingProcess::thread_proc_safe() throw()
{
	try {
		this->thread_proc();
	} catch (...) {
		wxTheApp->OnUnhandledException();
   	}
}

void BackgroundSlicingProcess::join_background_thread()
{
	std::unique_lock<std::mutex> lck(m_mutex);
	if (m_state == STATE_INITIAL) {
		// Worker thread has not been started yet.
		assert(! m_thread.joinable());
	} else {
		assert(m_state == STATE_IDLE);
		assert(m_thread.joinable());
		// Notify the worker thread to exit.
		m_state = STATE_EXIT;
		lck.unlock();
		m_condition.notify_one();
		// Wait until the worker thread exits.
		m_thread.join();
	}
}

bool BackgroundSlicingProcess::start()
{
	if (m_print->empty()) {
		if (!m_current_plate  || !m_current_plate->is_slice_result_valid())
			// The print is empty (no object in Model, or all objects are out of the print bed).
		    return false;
	}

	std::unique_lock<std::mutex> lck(m_mutex);
	if (m_state == STATE_INITIAL) {
		// The worker thread is not running yet. Start it.
		assert(! m_thread.joinable());
		m_thread = create_thread([this]{
#ifdef _WIN32
			this->thread_proc_safe_seh_throw();
#else // _WIN32
			this->thread_proc_safe();
#endif // _WIN32
		});
		// Wait until the worker thread is ready to execute the background processing task.
		m_condition.wait(lck, [this](){ return m_state == STATE_IDLE; });
	}
	assert(m_state == STATE_IDLE || this->running());
	if (this->running())
		// The background processing thread is already running.
		return false;
	if (! this->idle())
		throw Slic3r::RuntimeError("Cannot start a background task, the worker thread is not idle.");
	m_state = STATE_STARTED;
	m_print->set_cancel_callback([this](){ this->stop_internal(); });
	lck.unlock();
	m_condition.notify_one();
	return true;
}

// To be called on the UI thread.
bool BackgroundSlicingProcess::stop()
{
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ", enter"<<std::endl;
	// m_print->state_mutex() shall NOT be held. Unfortunately there is no interface to test for it.
	std::unique_lock<std::mutex> lck(m_mutex);
	if (m_state == STATE_INITIAL) {
//		m_export_path.clear();
		return false;
	}
//	assert(this->running());
	if (m_state == STATE_STARTED || m_state == STATE_RUNNING) {
		// Cancel any task planned by the background thread on UI thread.
		cancel_ui_task(m_ui_task);
		m_print->cancel();
		// Wait until the background processing stops by being canceled.
		m_condition.wait(lck, [this](){ return m_state == STATE_CANCELED; });
		// In the "Canceled" state. Reset the state to "Idle".
		m_state = STATE_IDLE;
		m_print->set_cancel_callback([](){});
	} else if (m_state == STATE_FINISHED || m_state == STATE_CANCELED) {
		// In the "Finished" or "Canceled" state. Reset the state to "Idle".
		m_state = STATE_IDLE;
		m_print->set_cancel_callback([](){});
	}
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ", exit"<<std::endl;
//	m_export_path.clear();
	return true;
}

bool BackgroundSlicingProcess::reset()
{
	bool stopped = this->stop();
	this->reset_export();
	//BBS: don't clear print for print is not owned by background slicing process anymore
	//do it in the part_plate
	//m_print->clear();
	this->invalidate_all_steps();
	return stopped;
}

// To be called by Print::apply() on the UI thread through the Print::m_cancel_callback to stop the background
// processing before changing any data of running or finalized milestones.
// This function shall not trigger any UI update through the wxWidgets event.
void BackgroundSlicingProcess::stop_internal()
{
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ", enter"<<std::endl;
	// m_print->state_mutex() shall be held. Unfortunately there is no interface to test for it.
	if (m_state == STATE_IDLE)
		// The worker thread is waiting on m_mutex/m_condition for wake up. The following lock of the mutex would block.
		return;
	std::unique_lock<std::mutex> lck(m_mutex);
	assert(m_state == STATE_STARTED || m_state == STATE_RUNNING || m_state == STATE_FINISHED || m_state == STATE_CANCELED);
	if (m_state == STATE_STARTED || m_state == STATE_RUNNING) {
		// Cancel any task planned by the background thread on UI thread.
		cancel_ui_task(m_ui_task);
		// At this point of time the worker thread may be blocking on m_print->state_mutex().
		// Set the print state to canceled before unlocking the state_mutex(), so when the worker thread wakes up,
		// it throws the CanceledException().
		m_print->cancel_internal();
		// Allow the worker thread to wake up if blocking on a milestone.
		m_print->state_mutex().unlock();
		// Wait until the background processing stops by being canceled.
		m_condition.wait(lck, [this](){ return m_state == STATE_CANCELED; });
		// Lock it back to be in a consistent state.
		m_print->state_mutex().lock();
	}
	// In the "Canceled" state. Reset the state to "Idle".
	m_state = STATE_IDLE;
	m_print->set_cancel_callback([](){});
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ", exit"<<std::endl;
}

// Execute task from background thread on the UI thread. Returns true if processed, false if cancelled.
bool BackgroundSlicingProcess::execute_ui_task(std::function<void()> task)
{
	bool running = false;
	if (m_mutex.try_lock()) {
		// Cancellation is either not in process, or already canceled and waiting for us to finish.
		// There must be no UI task planned.
		assert(! m_ui_task);
		if (! m_print->canceled()) {
			running = true;
			m_ui_task = std::make_shared<UITask>();
		}
		m_mutex.unlock();
	} else {
		// Cancellation is in process.
	}

	bool result = false;
	if (running) {
		std::shared_ptr<UITask> ctx = m_ui_task;
		GUI::wxGetApp().mainframe->m_plater->CallAfter([task, ctx]() {
			// Running on the UI thread, thus ctx->state does not need to be guarded with mutex against ::cancel_ui_task().
			assert(ctx->state == UITask::Planned || ctx->state == UITask::Canceled);
			if (ctx->state == UITask::Planned) {
				task();
				std::unique_lock<std::mutex> lck(ctx->mutex);
	    		ctx->state = UITask::Finished;
	    	}
	    	// Wake up the worker thread from the UI thread.
    		ctx->condition.notify_all();
	    });

	    {
			std::unique_lock<std::mutex> lock(ctx->mutex);
	    	ctx->condition.wait(lock, [&ctx]{ return ctx->state == UITask::Finished || ctx->state == UITask::Canceled; });
	    }
	    result = ctx->state == UITask::Finished;
		m_ui_task.reset();
	}

	return result;
}

// To be called on the UI thread from ::stop() and ::stop_internal().
void BackgroundSlicingProcess::cancel_ui_task(std::shared_ptr<UITask> task)
{
	if (task) {
		std::unique_lock<std::mutex> lck(task->mutex);
		task->state = UITask::Canceled;
		lck.unlock();
		task->condition.notify_all();
	}
}

bool BackgroundSlicingProcess::empty() const
{
	assert(m_print != nullptr);
	return m_print->empty();
}

StringObjectException BackgroundSlicingProcess::validate(StringObjectException *warning, Polygons* collison_polygons, std::vector<std::pair<Polygon, float>>* height_polygons)
{
	assert(m_print != nullptr);
    assert(m_print == m_fff_print);

    m_fff_print->is_BBL_printer() = wxGetApp().preset_bundle->is_bbl_vendor();
    return m_print->validate(warning, collison_polygons, height_polygons);
}

// Apply config over the print. Returns false, if the new config values caused any of the already
// processed steps to be invalidated, therefore the task will need to be restarted.
Print::ApplyStatus BackgroundSlicingProcess::apply(const Model &model, const DynamicPrintConfig &config)
{
	assert(m_print != nullptr);
	assert(config.opt_enum<PrinterTechnology>("printer_technology") == m_print->technology());
	// TODO: add partplate config
	DynamicPrintConfig new_config = config;
	new_config.apply(*m_current_plate->config());
	Print::ApplyStatus invalidated = m_print->apply(model, new_config);

	// Orca: prevent resetting under gcode viewer mode
    if (invalidated != PrintBase::APPLY_STATUS_UNCHANGED) {
        const auto plater = GUI::wxGetApp().mainframe->m_plater;
        if (plater && plater->only_gcode_mode()) {
            invalidated = PrintBase::APPLY_STATUS_UNCHANGED;
        }
    }

	if ((invalidated & PrintBase::APPLY_STATUS_INVALIDATED) != 0 && m_print->technology() == ptFFF &&
		!m_fff_print->is_step_done(psGCodeExport)) {
		// Some FFF status was invalidated, and the G-code was not exported yet.
		// Let the G-code preview UI know that the final G-code preview is not valid.
		// In addition, this early memory deallocation reduces memory footprint.
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": invalide gcode result %1%, will reset soon")%m_gcode_result;
		if (m_gcode_result != nullptr)
			m_gcode_result->reset();
	}
	return invalidated;
}

void BackgroundSlicingProcess::set_task(const PrintBase::TaskParams &params)
{
	assert(m_print != nullptr);
	m_print->set_task(params);
}

// Set the output path of the G-code.
void BackgroundSlicingProcess::schedule_export(const std::string &path, bool export_path_on_removable_media)
{
	assert(m_export_path.empty());
	if (! m_export_path.empty())
		return;

	// Guard against entering the export step before changing the export path.
	std::scoped_lock<std::mutex> lock(m_print->state_mutex());
	this->invalidate_step(bspsGCodeFinalize);
	m_export_path = path;
	m_export_path_on_removable_media = export_path_on_removable_media;
}

void BackgroundSlicingProcess::schedule_upload(Slic3r::PrintHostJob upload_job)
{
	assert(m_export_path.empty());
	if (! m_export_path.empty())
		return;

	// Guard against entering the export step before changing the export path.
	std::scoped_lock<std::mutex> lock(m_print->state_mutex());
	this->invalidate_step(bspsGCodeFinalize);
	m_export_path.clear();
	m_upload_job = std::move(upload_job);
}

void BackgroundSlicingProcess::reset_export()
{
	assert(! this->running());
	if (! this->running()) {
		m_export_path.clear();
		m_export_path_on_removable_media = false;
		// invalidate_step expects the mutex to be locked.
		std::scoped_lock<std::mutex> lock(m_print->state_mutex());
		this->invalidate_step(bspsGCodeFinalize);
	}
}

bool BackgroundSlicingProcess::set_step_started(BackgroundSlicingProcessStep step)
{
	return m_step_state.set_started(step, m_print->state_mutex(), [this](){ this->throw_if_canceled(); });
}

void BackgroundSlicingProcess::set_step_done(BackgroundSlicingProcessStep step)
{
	m_step_state.set_done(step, m_print->state_mutex(), [this](){ this->throw_if_canceled(); });
}

bool BackgroundSlicingProcess::is_step_done(BackgroundSlicingProcessStep step) const
{
	return m_step_state.is_done(step, m_print->state_mutex());
}

bool BackgroundSlicingProcess::invalidate_step(BackgroundSlicingProcessStep step)
{
    bool invalidated = m_step_state.invalidate(step, [this](){ this->stop_internal(); });
    return invalidated;
}

bool BackgroundSlicingProcess::invalidate_all_steps()
{
	return m_step_state.invalidate_all([this](){ this->stop_internal(); });
}

// G-code is generated in m_temp_output_path.
// Optionally run a post-processing script on a copy of m_temp_output_path.
// Copy the final G-code to target location (possibly a SD card, if it is a removable media, then verify that the file was written without an error).
void BackgroundSlicingProcess::finalize_gcode()
{
	m_print->set_status(95, _u8L("Running post-processing scripts"));

	// Perform the final post-processing of the export path by applying the print statistics over the file name.
	std::string export_path = m_fff_print->print_statistics().finalize_output_path(m_export_path);
	std::string output_path = m_temp_output_path;
	// Both output_path and export_path ar in-out parameters.
	// If post processed, output_path will differ from m_temp_output_path as run_post_process_scripts() will make a copy of the G-code to not
	// collide with the G-code viewer memory mapping of the unprocessed G-code. G-code viewer maps unprocessed G-code, because m_gcode_result 
	// is calculated for the unprocessed G-code and it references lines in the memory mapped G-code file by line numbers.
	// export_path may be changed by the post-processing script as well if the post processing script decides so, see GH #6042.
	bool post_processed = run_post_process_scripts(output_path, true, "File", export_path, m_fff_print->full_print_config());
	auto remove_post_processed_temp_file = [post_processed, &output_path]() {
		if (post_processed)
			try {
				boost::filesystem::remove(output_path);
			} catch (const std::exception &ex) {
				BOOST_LOG_TRIVIAL(error) << "Failed to remove temp file " << output_path << ": " << ex.what();
			}
	};
    m_print->set_status(99, _utf8(L("Successfully executed post-processing script")));

	//FIXME localize the messages
	std::string error_message;
	int copy_ret_val = CopyFileResult::SUCCESS;
	try
	{
		copy_ret_val = copy_file(output_path, export_path, error_message, m_export_path_on_removable_media);
		remove_post_processed_temp_file();
	}
	catch (...)
	{
		remove_post_processed_temp_file();
		throw Slic3r::ExportError(_u8L("Unknown error occurred during exporting G-code."));
	}
	switch (copy_ret_val) {
	case CopyFileResult::SUCCESS: break; // no error
	case CopyFileResult::FAIL_COPY_FILE:
		throw Slic3r::ExportError(GUI::format(_L("Copying of the temporary G-code to the output G-code failed. Maybe the SD card is write locked?\nError message: %1%"), error_message));
		break;
	case CopyFileResult::FAIL_FILES_DIFFERENT:
		throw Slic3r::ExportError(GUI::format(_L("Copying of the temporary G-code to the output G-code failed. There might be problem with target device, please try exporting again or using different device. The corrupted output G-code is at %1%.tmp."), export_path));
		break;
	case CopyFileResult::FAIL_RENAMING:
		throw Slic3r::ExportError(GUI::format(_L("Renaming of the G-code after copying to the selected destination folder has failed. Current path is %1%.tmp. Please try exporting again."), export_path));
		break;
	case CopyFileResult::FAIL_CHECK_ORIGIN_NOT_OPENED:
		throw Slic3r::ExportError(GUI::format(_L("Copying of the temporary G-code has finished but the original code at %1% couldn't be opened during copy check. The output G-code is at %2%.tmp."), output_path, export_path));
		break;
	case CopyFileResult::FAIL_CHECK_TARGET_NOT_OPENED:
		throw Slic3r::ExportError(GUI::format(_L("Copying of the temporary G-code has finished but the exported code couldn't be opened during copy check. The output G-code is at %1%.tmp."), export_path));
		break;
	default:
		throw Slic3r::ExportError(_u8L("Unknown error occurred during exporting G-code."));
		BOOST_LOG_TRIVIAL(error) << "Unexpected fail code(" << (int)copy_ret_val << ") durring copy_file() to " << export_path << ".";
		break;
	}

	m_print->set_status(100, GUI::format(_L("G-code file exported to %1%"), export_path));
}

// G-code is generated in m_temp_output_path.
// Optionally run a post-processing script on a copy of m_temp_output_path.
// Copy the final G-code to target location (possibly a SD card, if it is a removable media, then verify that the file was written without an error).
void BackgroundSlicingProcess::export_gcode()
{
	// Perform the final post-processing of the export path by applying the print statistics over the file name.
	std::string export_path = m_fff_print->print_statistics().finalize_output_path(m_export_path);
	std::string output_path = m_temp_output_path;

	//FIXME localize the messages
	std::string error_message;
	int copy_ret_val = CopyFileResult::SUCCESS;
	try
	{
		copy_ret_val = copy_file(output_path, export_path, error_message, m_export_path_on_removable_media);
	}
	catch (...)
	{
		throw Slic3r::ExportError(_utf8(L("Unknown error when exporting G-code.")));
	}
	switch (copy_ret_val) {
	case CopyFileResult::SUCCESS: break; // no error
	case CopyFileResult::FAIL_COPY_FILE:
		//throw Slic3r::ExportError((boost::format(_utf8(L("Copying of the temporary G-code to the output G-code failed. Maybe the SD card is write locked?\nError message: %1%"))) % error_message).str());
		//break;
	case CopyFileResult::FAIL_FILES_DIFFERENT:
		//throw Slic3r::ExportError((boost::format(_utf8(L("Copying of the temporary G-code to the output G-code failed. There might be problem with target device, please try exporting again or using different device. The corrupted output G-code is at %1%.tmp."))) % export_path).str());
		//break;
	case CopyFileResult::FAIL_RENAMING:
		//throw Slic3r::ExportError((boost::format(_utf8(L("Renaming of the G-code after copying to the selected destination folder has failed. Current path is %1%.tmp. Please try exporting again."))) % export_path).str());
		//break;
	case CopyFileResult::FAIL_CHECK_ORIGIN_NOT_OPENED:
		//throw Slic3r::ExportError((boost::format(_utf8(L("Copying of the temporary G-code has finished but the original code at %1% couldn't be opened during copy check. The output G-code is at %2%.tmp."))) % output_path % export_path).str());
		//break;
	case CopyFileResult::FAIL_CHECK_TARGET_NOT_OPENED:
		//throw Slic3r::ExportError((boost::format(_utf8(L("Copying of the temporary G-code has finished but the exported code couldn't be opened during copy check. The output G-code is at %1%.tmp."))) % export_path).str());
		//break;
	default:
		BOOST_LOG_TRIVIAL(error) << "Fail code(" << (int)copy_ret_val << ") when copy "<<output_path<<" to " << export_path << ".";
		throw Slic3r::ExportError((boost::format(_utf8(L("Failed to save G-code file.\nError message: %1%.\nSource file %2%."))) % error_message % output_path).str());
		//throw Slic3r::ExportError(_utf8(L("Unknown error when exporting G-code.")));
		break;
	}

	// BBS
	auto evt = new wxCommandEvent(m_event_export_finished_id, GUI::wxGetApp().mainframe->m_plater->GetId());
	wxString output_gcode_str = wxString::FromUTF8(export_path.c_str(), export_path.length());
	evt->SetString(output_gcode_str);
	wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt);

	// BBS: to be checked. Whether use export_path or output_path.
	gcode_add_line_number(export_path, m_fff_print->full_print_config());

}

// A print host upload job has been scheduled, enqueue it to the printhost job queue
void BackgroundSlicingProcess::prepare_upload()
{
	// Generate a unique temp path to which the gcode/zip file is copied/exported
	boost::filesystem::path source_path = boost::filesystem::temp_directory_path()
		/ boost::filesystem::unique_path("." SLIC3R_APP_KEY ".upload.%%%%-%%%%-%%%%-%%%%");

	if (m_print == m_fff_print) {
        if (m_upload_job.upload_data.use_3mf) {
            source_path = m_upload_job.upload_data.source_path;
        } else {
		    m_print->set_status(95, _utf8(L("Running post-processing scripts")));
		    std::string error_message;
		    if (copy_file(m_temp_output_path, source_path.string(), error_message) != SUCCESS)
		    	throw Slic3r::RuntimeError(_utf8(L("Copying of the temporary G-code to the output G-code failed")));
            m_upload_job.upload_data.upload_path = m_fff_print->print_statistics().finalize_output_path(m_upload_job.upload_data.upload_path.string());
		    // Orca: skip post-processing scripts for BBL printers as we have run them already in finalize_gcode()
		    // todo: do we need to copy the file?
		
            // Make a copy of the source path, as run_post_process_scripts() is allowed to change it when making a copy of the source file
            // (not here, but when the final target is a file).
            if (!m_fff_print->is_BBL_printer()) {
                std::string source_path_str = source_path.string();
                std::string output_name_str = m_upload_job.upload_data.upload_path.string();
                if (run_post_process_scripts(source_path_str, false, m_upload_job.printhost->get_name(), output_name_str,
                                             m_fff_print->full_print_config()))
			    m_upload_job.upload_data.upload_path = output_name_str;
			}
		}
    } else {
        m_upload_job.upload_data.upload_path = m_sla_print->print_statistics().finalize_output_path(m_upload_job.upload_data.upload_path.string());
        
        ThumbnailsList thumbnails = this->render_thumbnails(
        	ThumbnailsParams{current_print()->full_print_config().option<ConfigOptionPoints>("thumbnails")->values, true, true, true, true});
																												 // true, false, true, true); // renders also supports and pad
        Zipper zipper{source_path.string()};
        m_sla_archive.export_print(zipper, *m_sla_print, m_upload_job.upload_data.upload_path.string());
        for (const ThumbnailData& data : thumbnails)
	        if (data.is_valid())
	            write_thumbnail(zipper, data);
        zipper.finalize();
    }

    m_print->set_status(100, (boost::format(_utf8(L("Scheduling upload to `%1%`. See Window -> Print Host Upload Queue"))) % m_upload_job.printhost->get_host()).str());

	m_upload_job.upload_data.source_path = std::move(source_path);

	GUI::wxGetApp().printhost_job_queue().enqueue(std::move(m_upload_job));
}
// Executed by the background thread, to start a task on the UI thread.
ThumbnailsList BackgroundSlicingProcess::render_thumbnails(const ThumbnailsParams &params)
{
	ThumbnailsList thumbnails;
	if (m_thumbnail_cb)
		this->execute_ui_task([this, &params, &thumbnails](){ thumbnails = m_thumbnail_cb(params); });
	return thumbnails;
}

}; // namespace Slic3r
