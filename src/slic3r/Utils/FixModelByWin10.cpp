#ifdef HAS_WIN10SDK

#ifndef NOMINMAX
# define NOMINMAX
#endif

// Windows Runtime
#include <roapi.h>
// for ComPtr
#include <wrl/client.h>

// from C:/Program Files (x86)/Windows Kits/10/Include/10.0.17134.0/
#include <winrt/robuffer.h>
#include <winrt/windows.storage.provider.h>
#include <winrt/windows.graphics.printing3d.h>

#include "FixModelByWin10.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <string>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/thread.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "../GUI/GUI.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/MsgDialog.hpp"

#include <wx/msgdlg.h>
#include <wx/progdlg.h>

extern "C"{
	// from rapi.h
	typedef HRESULT (__stdcall* FunctionRoInitialize)(int);
	typedef HRESULT (__stdcall* FunctionRoUninitialize)();
	typedef HRESULT	(__stdcall* FunctionRoActivateInstance)(HSTRING activatableClassId, IInspectable **instance);
	typedef HRESULT (__stdcall* FunctionRoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void **factory);
	// from winstring.h
	typedef HRESULT	(__stdcall* FunctionWindowsCreateString)(LPCWSTR sourceString, UINT32  length, HSTRING *string);
	typedef HRESULT	(__stdcall* FunctionWindowsDelteString)(HSTRING string);
}

namespace Slic3r {

static std::string saving_failed_str = L("Saving objects into the 3MF failed.");

HMODULE							s_hRuntimeObjectLibrary  = nullptr;
FunctionRoInitialize			s_RoInitialize			 = nullptr;
FunctionRoUninitialize			s_RoUninitialize		 = nullptr;
FunctionRoActivateInstance		s_RoActivateInstance     = nullptr;
FunctionRoGetActivationFactory	s_RoGetActivationFactory = nullptr;
FunctionWindowsCreateString		s_WindowsCreateString    = nullptr;
FunctionWindowsDelteString		s_WindowsDeleteString    = nullptr;

bool winrt_load_runtime_object_library()
{
	if (s_hRuntimeObjectLibrary == nullptr)
		s_hRuntimeObjectLibrary = LoadLibrary(L"ComBase.dll");
	if (s_hRuntimeObjectLibrary != nullptr) {
		s_RoInitialize			 = (FunctionRoInitialize)			GetProcAddress(s_hRuntimeObjectLibrary, "RoInitialize");
		s_RoUninitialize		 = (FunctionRoUninitialize)			GetProcAddress(s_hRuntimeObjectLibrary, "RoUninitialize");
		s_RoActivateInstance	 = (FunctionRoActivateInstance)		GetProcAddress(s_hRuntimeObjectLibrary, "RoActivateInstance");
		s_RoGetActivationFactory = (FunctionRoGetActivationFactory)	GetProcAddress(s_hRuntimeObjectLibrary, "RoGetActivationFactory");
		s_WindowsCreateString	 = (FunctionWindowsCreateString)	GetProcAddress(s_hRuntimeObjectLibrary, "WindowsCreateString");
		s_WindowsDeleteString	 = (FunctionWindowsDelteString)		GetProcAddress(s_hRuntimeObjectLibrary, "WindowsDeleteString");
	}
	return s_RoInitialize && s_RoUninitialize && s_RoActivateInstance && s_WindowsCreateString && s_WindowsDeleteString;
}

static HRESULT winrt_activate_instance(const std::wstring &class_name, IInspectable **pinst)
{
	HSTRING hClassName;
	HRESULT hr = (*s_WindowsCreateString)(class_name.c_str(), class_name.size(), &hClassName);
	if (S_OK != hr)
		return hr;
	hr = (*s_RoActivateInstance)(hClassName, pinst);
	(*s_WindowsDeleteString)(hClassName);
	return hr;
}

template<typename TYPE>
static HRESULT winrt_activate_instance(const std::wstring &class_name, TYPE **pinst)
{
	IInspectable *pinspectable = nullptr;
	HRESULT hr = winrt_activate_instance(class_name, &pinspectable);
	if (S_OK != hr)
		return hr;
	hr = pinspectable->QueryInterface(__uuidof(TYPE), (void**)pinst);
	pinspectable->Release();
	return hr;
}

static HRESULT winrt_get_activation_factory(const std::wstring &class_name, REFIID iid, void **pinst)
{
	HSTRING hClassName;
	HRESULT hr = (*s_WindowsCreateString)(class_name.c_str(), class_name.size(), &hClassName);
	if (S_OK != hr)
		return hr;
	hr = (*s_RoGetActivationFactory)(hClassName, iid, pinst);
	(*s_WindowsDeleteString)(hClassName);
	return hr;
}

template<typename TYPE>
static HRESULT winrt_get_activation_factory(const std::wstring &class_name, TYPE **pinst)
{
	return winrt_get_activation_factory(class_name, __uuidof(TYPE), reinterpret_cast<void**>(pinst));
}

// To be called often to test whether to cancel the operation.
typedef std::function<void ()> ThrowOnCancelFn;

template<typename T>
static AsyncStatus winrt_async_await(const Microsoft::WRL::ComPtr<T> &asyncAction, ThrowOnCancelFn throw_on_cancel, int blocking_tick_ms = 100)
{
	Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncInfo> asyncInfo;
	asyncAction.As(&asyncInfo);
	AsyncStatus status;
	// Ugly blocking loop until the RepairAsync call finishes.
//FIXME replace with a callback.
// https://social.msdn.microsoft.com/Forums/en-US/a5038fb4-b7b7-4504-969d-c102faa389fb/trying-to-block-an-async-operation-and-wait-for-a-particular-time?forum=vclanguage
	for (;;) {
		asyncInfo->get_Status(&status);
		if (status != AsyncStatus::Started)
			return status;
		throw_on_cancel();
		::Sleep(blocking_tick_ms);
	}
}

static HRESULT winrt_open_file_stream(
	const std::wstring									 &path,
	ABI::Windows::Storage::FileAccessMode				  mode,
	ABI::Windows::Storage::Streams::IRandomAccessStream **fileStream,
	ThrowOnCancelFn										  throw_on_cancel)
{
	// Get the file factory.
	Microsoft::WRL::ComPtr<ABI::Windows::Storage::IStorageFileStatics> fileFactory;
	HRESULT hr = winrt_get_activation_factory(L"Windows.Storage.StorageFile", fileFactory.GetAddressOf());
	if (FAILED(hr)) return hr;

	// Open the file asynchronously.
	HSTRING hstr_path;
	hr = (*s_WindowsCreateString)(path.c_str(), path.size(), &hstr_path);
	if (FAILED(hr)) return hr;
	Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::StorageFile*>> fileOpenAsync;
	hr = fileFactory->GetFileFromPathAsync(hstr_path, fileOpenAsync.GetAddressOf());
	if (FAILED(hr)) return hr;
	(*s_WindowsDeleteString)(hstr_path);

	// Wait until the file gets open, get the actual file.
	AsyncStatus status = winrt_async_await(fileOpenAsync, throw_on_cancel);
	Microsoft::WRL::ComPtr<ABI::Windows::Storage::IStorageFile> storageFile;
	if (status == AsyncStatus::Completed) {
		hr = fileOpenAsync->GetResults(storageFile.GetAddressOf());
	} else {
		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncInfo> asyncInfo;
		hr = fileOpenAsync.As(&asyncInfo);
		if (FAILED(hr)) return hr;
		HRESULT err;
		hr = asyncInfo->get_ErrorCode(&err);
		return FAILED(hr) ? hr : err;
	}

	Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::Streams::IRandomAccessStream*>> fileStreamAsync;
	hr = storageFile->OpenAsync(mode, fileStreamAsync.GetAddressOf());
	if (FAILED(hr)) return hr;

	status = winrt_async_await(fileStreamAsync, throw_on_cancel);
	if (status == AsyncStatus::Completed) {
		hr = fileStreamAsync->GetResults(fileStream);
	} else {
		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncInfo> asyncInfo;
		hr = fileStreamAsync.As(&asyncInfo);
		if (FAILED(hr)) return hr;
		HRESULT err;
		hr = asyncInfo->get_ErrorCode(&err);
		if (!FAILED(hr))
			hr = err;
	}
	return hr;
}

bool is_windows10()
{
	HKEY hKey;
	LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey);
	if (lRes == ERROR_SUCCESS) {
		WCHAR szBuffer[512];
		DWORD dwBufferSize = sizeof(szBuffer);
		lRes = RegQueryValueExW(hKey, L"ProductName", 0, nullptr, (LPBYTE)szBuffer, &dwBufferSize);
		if (lRes == ERROR_SUCCESS)
			return wcsncmp(szBuffer, L"Windows 10", 10) == 0;
		RegCloseKey(hKey);
	}
	return false;
}

// Progress function, to be called regularly to update the progress.
typedef std::function<void (const char * /* message */, unsigned /* progress */)> ProgressFn;

void fix_model_by_win10_sdk(const std::string &path_src, const std::string &path_dst, ProgressFn on_progress, ThrowOnCancelFn throw_on_cancel)
{
	if (! is_windows10())
		throw Slic3r::RuntimeError(L("Only Windows 10 is supported."));

	if (! winrt_load_runtime_object_library())
		throw Slic3r::RuntimeError(L("Failed to initialize the WinRT library."));

	HRESULT hr = (*s_RoInitialize)(RO_INIT_MULTITHREADED);
	{
		on_progress(L("Exporting objects"), 20);

		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IRandomAccessStream>       fileStream;
		hr = winrt_open_file_stream(boost::nowide::widen(path_src), ABI::Windows::Storage::FileAccessMode::FileAccessMode_Read, fileStream.GetAddressOf(), throw_on_cancel);

		Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Printing3D::IPrinting3D3MFPackage> printing3d3mfpackage;
		hr = winrt_activate_instance(L"Windows.Graphics.Printing3D.Printing3D3MFPackage", printing3d3mfpackage.GetAddressOf());

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Graphics::Printing3D::Printing3DModel*>> modelAsync;
		hr = printing3d3mfpackage->LoadModelFromPackageAsync(fileStream.Get(), modelAsync.GetAddressOf());

		AsyncStatus status = winrt_async_await(modelAsync, throw_on_cancel);
		Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Printing3D::IPrinting3DModel>	  model;
		if (status == AsyncStatus::Completed)
			hr = modelAsync->GetResults(model.GetAddressOf());
		else
			throw Slic3r::RuntimeError(L("Failed loading objects."));

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::Collections::IVector<ABI::Windows::Graphics::Printing3D::Printing3DMesh*>> meshes;
		hr = model->get_Meshes(meshes.GetAddressOf());
		unsigned num_meshes = 0;
		hr = meshes->get_Size(&num_meshes);
		on_progress(L("Repairing object by Windows service"), 40);
		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncAction>					  repairAsync;
		hr = model->RepairAsync(repairAsync.GetAddressOf());
		status = winrt_async_await(repairAsync, throw_on_cancel);
		if (status != AsyncStatus::Completed)
			throw Slic3r::RuntimeError(L("Repair failed."));
		repairAsync->GetResults();

		on_progress(L("Loading repaired objects"), 60);

		// Verify the number of meshes returned after the repair action.
		meshes.Reset();
		hr = model->get_Meshes(meshes.GetAddressOf());
		hr = meshes->get_Size(&num_meshes);

		// Save model to this class' Printing3D3MFPackage.
		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncAction>					  saveToPackageAsync;
		hr = printing3d3mfpackage->SaveModelToPackageAsync(model.Get(), saveToPackageAsync.GetAddressOf());
		status = winrt_async_await(saveToPackageAsync, throw_on_cancel);
		if (status != AsyncStatus::Completed)
			throw Slic3r::RuntimeError(saving_failed_str);
		hr = saveToPackageAsync->GetResults();

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperation<ABI::Windows::Storage::Streams::IRandomAccessStream*>> generatorStreamAsync;
		hr = printing3d3mfpackage->SaveAsync(generatorStreamAsync.GetAddressOf());
		status = winrt_async_await(generatorStreamAsync, throw_on_cancel);
		if (status != AsyncStatus::Completed)
			throw Slic3r::RuntimeError(saving_failed_str);
		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IRandomAccessStream> generatorStream;
		hr = generatorStreamAsync->GetResults(generatorStream.GetAddressOf());

		// Go to the beginning of the stream.
		generatorStream->Seek(0);
		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IInputStream> inputStream;
		hr = generatorStream.As(&inputStream);

		// Get the buffer factory.
		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IBufferFactory> bufferFactory;
		hr = winrt_get_activation_factory(L"Windows.Storage.Streams.Buffer", bufferFactory.GetAddressOf());

		// Open the destination file.
		FILE *fout = boost::nowide::fopen(path_dst.c_str(), "wb");

		Microsoft::WRL::ComPtr<ABI::Windows::Storage::Streams::IBuffer> buffer;
		byte														   *buffer_ptr;
		bufferFactory->Create(65536 * 2048, buffer.GetAddressOf());
		{
			Microsoft::WRL::ComPtr<Windows::Storage::Streams::IBufferByteAccess> bufferByteAccess;
			buffer.As(&bufferByteAccess);
			hr = bufferByteAccess->Buffer(&buffer_ptr);
		}
		uint32_t length;
		hr = buffer->get_Length(&length);

		Microsoft::WRL::ComPtr<ABI::Windows::Foundation::IAsyncOperationWithProgress<ABI::Windows::Storage::Streams::IBuffer*, UINT32>> asyncRead;
		for (;;) {
			hr = inputStream->ReadAsync(buffer.Get(), 65536 * 2048, ABI::Windows::Storage::Streams::InputStreamOptions_ReadAhead, asyncRead.GetAddressOf());
			status = winrt_async_await(asyncRead, throw_on_cancel);
			if (status != AsyncStatus::Completed)
				throw Slic3r::RuntimeError(saving_failed_str);
			hr = buffer->get_Length(&length);
			if (length == 0)
				break;
			fwrite(buffer_ptr, length, 1, fout);
		}
		fclose(fout);
		// Here all the COM objects will be released through the ComPtr destructors.
	}
	(*s_RoUninitialize)();
}

class RepairCanceledException : public std::exception {
public:
   const char* what() const throw() { return "Model repair has been canceled"; }
};

// returt FALSE, if fixing was canceled
// fix_result is empty, if fixing finished successfully
// fix_result containes a message if fixing failed
bool fix_model_by_win10_sdk_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog& progress_dialog, const wxString& msg_header, std::string& fix_result)
{
    std::mutex mtx;
    std::condition_variable condition;
	struct Progress {
		std::string 				message;
		int 						percent  = 0;
		bool						updated = false;
	} progress;
	std::atomic<bool>				canceled = false;
	std::atomic<bool>				finished = false;

	std::vector<ModelVolume*> volumes;
	if (volume_idx == -1)
		volumes = model_object.volumes;
	else
		volumes.emplace_back(model_object.volumes[volume_idx]);

	// Executing the calculation in a background thread, so that the COM context could be created with its own threading model.
	// (It seems like wxWidgets initialize the COM contex as single threaded and we need a multi-threaded context).
	bool   success = false;
	size_t ivolume = 0;
	auto on_progress = [&mtx, &condition, &ivolume, &volumes, &progress](const char *msg, unsigned prcnt) {
	    std::unique_lock<std::mutex> lock(mtx);
		progress.message = msg;
		progress.percent = (int)floor((float(prcnt) + float(ivolume) * 100.f) / float(volumes.size()));
		progress.updated = true;
	    condition.notify_all();
	};
	auto worker_thread = boost::thread([&model_object, &volumes, &ivolume, on_progress, &success, &canceled, &finished]() {
		try {
			std::vector<TriangleMesh> meshes_repaired;
			meshes_repaired.reserve(volumes.size());
			for (; ivolume < volumes.size(); ++ ivolume) {
				on_progress(L("Exporting objects"), 0);
				boost::filesystem::path path_src = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
				path_src += ".3mf";
				Model model;
                ModelObject *mo = model.add_object();
                mo->add_volume(*volumes[ivolume]);

                // We are about to save a 3mf, fix it by netfabb and load the fixed 3mf back.
                // store_3mf currently bakes the volume transformation into the mesh itself.
                // If we then loaded the repaired 3mf and pushed the mesh into the original ModelVolume
                // (which remembers the matrix the whole time), the transformation would be used twice.
                // We will therefore set the volume transform on the dummy ModelVolume to identity.
                mo->volumes.back()->set_transformation(Geometry::Transformation());

                mo->add_instance();
				if (!Slic3r::store_3mf(path_src.string().c_str(), &model, nullptr, false, nullptr, false)) {
					boost::filesystem::remove(path_src);
					throw Slic3r::RuntimeError(L("Exporting 3MF file failed"));
				}
				model.clear_objects();
				model.clear_materials();
				boost::filesystem::path path_dst = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
				path_dst += ".3mf";
				fix_model_by_win10_sdk(path_src.string().c_str(), path_dst.string(), on_progress,
					[&canceled]() { if (canceled) throw RepairCanceledException(); });
				boost::filesystem::remove(path_src);
	            // PresetBundle bundle;
				on_progress(L("Loading repaired objects"), 80);
				DynamicPrintConfig config;
				ConfigSubstitutionContext config_substitutions{ ForwardCompatibilitySubstitutionRule::EnableSilent };
				bool loaded = Slic3r::load_3mf(path_dst.string().c_str(), config, config_substitutions, &model, false);
			    boost::filesystem::remove(path_dst);
				if (! loaded)
					throw Slic3r::RuntimeError(L("Import 3MF file failed"));
	 			if (model.objects.size() == 0)
					throw Slic3r::RuntimeError(L("Repaired 3MF file does not contain any object"));
	 			if (model.objects.size() > 1)
					throw Slic3r::RuntimeError(L("Repaired 3MF file contains more than one object"));
	 			if (model.objects.front()->volumes.size() == 0)
					throw Slic3r::RuntimeError(L("Repaired 3MF file does not contain any volume"));
				if (model.objects.front()->volumes.size() > 1)
					throw Slic3r::RuntimeError(L("Repaired 3MF file contains more than one volume"));
	 			meshes_repaired.emplace_back(std::move(model.objects.front()->volumes.front()->mesh()));
			}
			for (size_t i = 0; i < volumes.size(); ++ i) {
				volumes[i]->set_mesh(std::move(meshes_repaired[i]));
				volumes[i]->calculate_convex_hull();
				volumes[i]->invalidate_convex_hull_2d();
				volumes[i]->set_new_unique_id();
			}
			model_object.invalidate_bounding_box();
			-- ivolume;
			on_progress(L("Repair finished"), 100);
			success  = true;
			finished = true;
		} catch (RepairCanceledException & /* ex */) {
			canceled = true;
			finished = true;
			on_progress(L("Repair canceled"), 100);
		} catch (std::exception &ex) {
			success = false;
			finished = true;
			on_progress(ex.what(), 100);
		}
	});
    while (! finished) {
		std::unique_lock<std::mutex> lock(mtx);
		condition.wait_for(lock, std::chrono::milliseconds(250), [&progress]{ return progress.updated; });
		// decrease progress.percent value to avoid closing of the progress dialog
		if (!progress_dialog.Update(progress.percent-1, msg_header + _(progress.message)))
			canceled = true;
		else
			progress_dialog.Fit();
		progress.updated = false;
    }

	if (canceled) {
		// Nothing to show.
	} else if (success) {
		fix_result = "";
	} else {
		fix_result = progress.message;
	}
	worker_thread.join();
	return !canceled;
}

} // namespace Slic3r

#endif /* HAS_WIN10SDK */
