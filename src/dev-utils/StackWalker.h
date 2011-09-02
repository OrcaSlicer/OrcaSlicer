#pragma once
#include <Windows.h>
#include <tchar.h>
#include <vector>

namespace textconv_helper
{
	// Forward declarations of our classes. They are defined later.
	class CA2A_;
	class CA2W_;
	class CW2A_;
	class CW2W_;
	class CA2BSTR_;
	class CW2BSTR_;

	// typedefs for the well known text conversions
	typedef CA2W_ A2W_;
	typedef CW2A_ W2A_;
	//typedef CW2BSTR_ W2BSTR_;
	//typedef CA2BSTR_ A2BSTR_;
	typedef CW2A_ BSTR2A_;
	typedef CW2W_ BSTR2W_;

#ifdef _UNICODE
	typedef CA2W_ A2T_;
	typedef CW2A_ T2A_;
	typedef CW2W_ T2W_;
	typedef CW2W_ W2T_;
	//typedef CW2BSTR_ T2BSTR_;
	//typedef BSTR2W_ BSTR2T_;
#else
	typedef CA2A_ A2T_;
	typedef CA2A_ T2A_;
	typedef CA2W_ T2W_;
	typedef CW2A_ W2T_;
	typedef CA2BSTR_ T2BSTR_;
	typedef BSTR2A_ BSTR2T_;
#endif

	typedef A2W_  A2OLE_;
	typedef T2W_  T2OLE_;
	typedef CW2W_ W2OLE_;
	typedef W2A_  OLE2A_;
	typedef W2T_  OLE2T_;
	typedef CW2W_ OLE2W_;

	class CA2W_
	{
	public:
		CA2W_(LPCSTR pStr, UINT codePage = CP_ACP) : m_pStr(pStr)
		{
			if (pStr)
			{
				// Resize the vector and assign null WCHAR to each element
				int length = MultiByteToWideChar(codePage, 0, pStr, -1, NULL, 0) + 1;
				m_vWideArray.assign(length, L'\0');

				// Fill our vector with the converted WCHAR array
				MultiByteToWideChar(codePage, 0, pStr, -1, &m_vWideArray[0], length);
			}
		}
		~CA2W_() {}
		operator LPCWSTR() { return m_pStr ? &m_vWideArray[0] : NULL; }
		//operator LPOLESTR() { return m_pStr ? (LPOLESTR)&m_vWideArray[0] : (LPOLESTR)NULL; }

	private:
		CA2W_(const CA2W_&);
		CA2W_& operator= (const CA2W_&);
		std::vector<wchar_t> m_vWideArray;
		LPCSTR m_pStr;
	};

	class CW2A_
	{
	public:
		CW2A_(LPCWSTR pWStr, UINT codePage = CP_ACP) : m_pWStr(pWStr)
			// Usage:
			//   CW2A_ ansiString(L"Some Text");
			//   CW2A_ utf8String(L"Some Text", CP_UTF8);
			//
			// or
			//   SetWindowTextA( W2A(L"Some Text") ); The ANSI version of SetWindowText
		{
			// Resize the vector and assign null char to each element
			int length = WideCharToMultiByte(codePage, 0, pWStr, -1, NULL, 0, NULL, NULL) + 1;
			m_vAnsiArray.assign(length, '\0');

			// Fill our vector with the converted char array
			WideCharToMultiByte(codePage, 0, pWStr, -1, &m_vAnsiArray[0], length, NULL, NULL);
		}

		~CW2A_()
		{
			m_pWStr = 0;
		}
		operator LPCSTR() { return m_pWStr ? &m_vAnsiArray[0] : NULL; }

	private:
		CW2A_(const CW2A_&);
		CW2A_& operator= (const CW2A_&);
		std::vector<char> m_vAnsiArray;
		LPCWSTR m_pWStr;
	};

	class CW2W_
	{
	public:
		CW2W_(LPCWSTR pWStr) : m_pWStr(pWStr) {}
		operator LPCWSTR() { return const_cast<LPWSTR>(m_pWStr); }
		//operator LPOLESTR() { return const_cast<LPOLESTR>(m_pWStr); }

	private:
		CW2W_(const CW2W_&);
		CW2W_& operator= (const CW2W_&);

		LPCWSTR m_pWStr;
	};

	class CA2A_
	{
	public:
		CA2A_(LPCSTR pStr) : m_pStr(pStr) {}
		operator LPCSTR() { return (LPSTR)m_pStr; }

	private:
		CA2A_(const CA2A_&);
		CA2A_& operator= (const CA2A_&);

		LPCSTR m_pStr;
	};

	/*class CW2BSTR_
	{
	public:
		CW2BSTR_(LPCWSTR pWStr) { m_bstrString = ::SysAllocString(pWStr); }
		~CW2BSTR_() { ::SysFreeString(m_bstrString); }
		operator BSTR() { return m_bstrString; }

	private:
		CW2BSTR_(const CW2BSTR_&);
		CW2BSTR_& operator= (const CW2BSTR_&);
		BSTR m_bstrString;
	};

	class CA2BSTR_
	{
	public:
		CA2BSTR_(LPCSTR pStr) { m_bstrString = ::SysAllocString(textconv_helper::CA2W_(pStr)); }
		~CA2BSTR_() { ::SysFreeString(m_bstrString); }
		operator BSTR() { return m_bstrString; }

	private:
		CA2BSTR_(const CA2BSTR_&);
		CA2BSTR_& operator= (const CA2BSTR_&);
		BSTR m_bstrString;
	};*/
}

#define MAX_SYMBOL_PATH 1024
#define MAX_MODULE_NAME32 255
#define TH32CS_SNAPMODULE 0x00000008
#define MAX_VERSION_LENGTH 512
#define STACKWALK_MAX_NAMELEN 1024

#define ASSERT(judge)\
	{\
		if(!(judge))\
		{\
			DebugBreak();\
		}\
	}

typedef struct tagMODULEENTRY32
{
	DWORD   dwSize;
	DWORD   th32ModuleID;  // This module
	DWORD   th32ProcessID; // owning process
	DWORD   GlblcntUsage;  // Global usage count on the module
	DWORD   ProccntUsage;  // Module usage count in th32ProcessID's context
	BYTE*   modBaseAddr;   // Base address of module in th32ProcessID's context
	DWORD   modBaseSize;   // Size in bytes of module starting at modBaseAddr
	HMODULE hModule;       // The hModule of this module in th32ProcessID's context
	TCHAR   szModule[MAX_MODULE_NAME32 + 1];
	TCHAR   szExePath[MAX_PATH];
} MODULEENTRY32;

typedef struct _MODULEINFO
{
	LPVOID lpBaseOfDll;
	DWORD  SizeOfImage;
	LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;

typedef MODULEENTRY32* PMODULEENTRY32;
typedef MODULEENTRY32* LPMODULEENTRY32;

typedef struct _tag_MODULE_INFO 
{
	DWORD64 ModuleAddress;
	DWORD dwModSize;
	TCHAR szModuleName[MAX_MODULE_NAME32 + 1];
	TCHAR szModulePath[MAX_PATH];
	TCHAR szSymbolPath[MAX_PATH];
	TCHAR szVersion[MAX_VERSION_LENGTH];
	struct _tag_MODULE_INFO* pNext;
}MODULE_INFO, *LPMODULE_INFO;

typedef struct tagSTACKINFO
{
	DWORD64 szFncAddr;
	TCHAR szFileName[MAX_PATH];
	TCHAR szFncName[MAX_PATH];
	unsigned long uFileNum;
	TCHAR    undName[STACKWALK_MAX_NAMELEN];
	TCHAR    undFullName[STACKWALK_MAX_NAMELEN];
	tagSTACKINFO *pNext;
}STACKINFO, *LPSTACKINFO;

class CStackWalker
{
public:
	CStackWalker(HANDLE hProcess = GetCurrentProcess(), WORD wPID = GetCurrentProcessId(), LPCTSTR lpSymbolPath = NULL);
	~CStackWalker(void);
	BOOL LoadSymbol();
	LPMODULE_INFO GetLoadModules();
	void GetModuleInformation(LPMODULE_INFO pmi);

	void FreeModuleInformations(LPMODULE_INFO pmi);
	virtual void OutputString(LPCTSTR lpszFormat, ...);

	LPSTACKINFO StackWalker(HANDLE hThread = GetCurrentThread(), const CONTEXT* context = NULL);
	void FreeStackInformations(LPSTACKINFO psi);

protected:
	LPMODULE_INFO GetModulesTH32();
	LPMODULE_INFO GetModulesPSAPI();
protected:
	HANDLE m_hProcess;
	WORD m_wPID;
	LPTSTR m_lpszSymbolPath;
	BOOL m_bSymbolLoaded;
};

#if defined(_M_IX86)
#ifdef CURRENT_THREAD_VIA_EXCEPTION
#define GET_CURRENT_THREAD_CONTEXT(c, contextFlags)\
do\
{\
	memset(&c, 0, sizeof(CONTEXT));\
	EXCEPTION_POINTERS* pExp = NULL;\
	__try\
	{\
		throw 0;\
	}\
	__except (((pExp = GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER:EXCEPTION_EXECUTE_HANDLER))\
	{\
	}\
	if (pExp != NULL)\
		memcpy(&c, pExp->ContextRecord, sizeof(CONTEXT));\
	c.ContextFlags = contextFlags;\
} while (0);
#else
#define GET_CURRENT_THREAD_CONTEXT(c, contextFlags) \
	do\
	{\
		memset(&c, 0, sizeof(CONTEXT));\
		c.ContextFlags = contextFlags;\
		__asm    call $+5\
		__asm    pop eax\
		__asm    mov c.Eip, eax\
		__asm    mov c.Ebp, ebp\
		__asm    mov c.Esp, esp\
} while (0)
#endif

#else
#define GET_CURRENT_THREAD_CONTEXT(c, contextFlags) \
	do\
	{ \
		memset(&c, 0, sizeof(CONTEXT));\
		c.ContextFlags = contextFlags;\
		RtlCaptureContext(&c);\
} while (0);
#endif