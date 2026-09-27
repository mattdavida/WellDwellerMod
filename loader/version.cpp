// WellDweller.exe imports version.dll, so this copy in the game folder loads
// at startup and starts Aurie. The exe does not need the Aurie patch.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

HMODULE g_Real = nullptr;

bool IsWellDweller()
{
	wchar_t path[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return false;
	const wchar_t* slash = wcsrchr(path, L'\\');
	const wchar_t* name = slash != nullptr ? slash + 1 : path;
	return _wcsicmp(name, L"WellDweller.exe") == 0;
}

void LoadRealVersion()
{
	if (g_Real != nullptr)
		return;
	wchar_t path[MAX_PATH]{};
	const UINT length = GetSystemDirectoryW(path, MAX_PATH);
	if (length == 0 || length + 13 >= MAX_PATH)
		return;
	wcscat_s(path, L"\\version.dll");
	g_Real = LoadLibraryW(path);
}

DWORD g_MainThread = 0;

DWORD WINAPI LoadAurieThread(LPVOID)
{
	if (GetModuleHandleW(L"AurieCore.dll") != nullptr)
		return 0;

	wchar_t path[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return 0;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash == nullptr)
		return 0;
	slash[1] = L'\0';
	if (wcslen(path) + 28 >= MAX_PATH)
		return 0;
	wcscat_s(path, L"mods\\Native\\AurieCore.dll");

	const HANDLE main = OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_MainThread);
	if (main == nullptr)
		return 0;
	SuspendThread(main);
	if (LoadLibraryW(path) == nullptr)
		ResumeThread(main);
	CloseHandle(main);
	return 0;
}

FARPROC RealProc(const char* name)
{
	if (g_Real == nullptr)
		LoadRealVersion();
	if (g_Real == nullptr)
		return nullptr;
	return GetProcAddress(g_Real, name);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		LoadRealVersion();
		if (IsWellDweller())
		{
			g_MainThread = GetCurrentThreadId();
			const HANDLE thread = CreateThread(nullptr, 0, LoadAurieThread, nullptr, 0, nullptr);
			if (thread != nullptr)
				CloseHandle(thread);
		}
	}
	return TRUE;
}

#define PROXY_BOOL(name, params, args) \
	extern "C" BOOL WINAPI name params \
	{ \
		using Fn = BOOL(WINAPI*) params; \
		const auto fn = reinterpret_cast<Fn>(RealProc(#name)); \
		return fn != nullptr ? fn args : FALSE; \
	}

#define PROXY_DWORD(name, params, args) \
	extern "C" DWORD WINAPI name params \
	{ \
		using Fn = DWORD(WINAPI*) params; \
		const auto fn = reinterpret_cast<Fn>(RealProc(#name)); \
		return fn != nullptr ? fn args : 0; \
	}

PROXY_BOOL(GetFileVersionInfoA, (LPCSTR file, DWORD handle, DWORD len, LPVOID data), (file, handle, len, data))
PROXY_BOOL(GetFileVersionInfoByHandle, (DWORD flags, HANDLE file, DWORD len, LPVOID data), (flags, file, len, data))
PROXY_BOOL(GetFileVersionInfoExA, (DWORD flags, LPCSTR file, DWORD handle, DWORD len, LPVOID data), (flags, file, handle, len, data))
PROXY_BOOL(GetFileVersionInfoExW, (DWORD flags, LPCWSTR file, DWORD handle, DWORD len, LPVOID data), (flags, file, handle, len, data))
PROXY_DWORD(GetFileVersionInfoSizeA, (LPCSTR file, LPDWORD handle), (file, handle))
PROXY_DWORD(GetFileVersionInfoSizeExA, (DWORD flags, LPCSTR file, LPDWORD handle), (flags, file, handle))
PROXY_DWORD(GetFileVersionInfoSizeExW, (DWORD flags, LPCWSTR file, LPDWORD handle), (flags, file, handle))
PROXY_DWORD(GetFileVersionInfoSizeW, (LPCWSTR file, LPDWORD handle), (file, handle))
PROXY_BOOL(GetFileVersionInfoW, (LPCWSTR file, DWORD handle, DWORD len, LPVOID data), (file, handle, len, data))
PROXY_DWORD(VerFindFileA, (DWORD flags, LPCSTR file, LPCSTR win, LPCSTR app, LPSTR cur, PUINT curLen, LPSTR dest, PUINT destLen), (flags, file, win, app, cur, curLen, dest, destLen))
PROXY_DWORD(VerFindFileW, (DWORD flags, LPCWSTR file, LPCWSTR win, LPCWSTR app, LPWSTR cur, PUINT curLen, LPWSTR dest, PUINT destLen), (flags, file, win, app, cur, curLen, dest, destLen))
PROXY_DWORD(VerInstallFileA, (DWORD flags, LPCSTR src, LPCSTR dest, LPCSTR srcDir, LPCSTR destDir, LPCSTR cur, LPSTR tmp, PUINT tmpLen), (flags, src, dest, srcDir, destDir, cur, tmp, tmpLen))
PROXY_DWORD(VerInstallFileW, (DWORD flags, LPCWSTR src, LPCWSTR dest, LPCWSTR srcDir, LPCWSTR destDir, LPCWSTR cur, LPWSTR tmp, PUINT tmpLen), (flags, src, dest, srcDir, destDir, cur, tmp, tmpLen))
PROXY_DWORD(VerLanguageNameA, (DWORD lang, LPSTR buffer, DWORD size), (lang, buffer, size))
PROXY_DWORD(VerLanguageNameW, (DWORD lang, LPWSTR buffer, DWORD size), (lang, buffer, size))
PROXY_BOOL(VerQueryValueA, (LPCVOID block, LPCSTR sub, LPVOID* buffer, PUINT len), (block, sub, buffer, len))
PROXY_BOOL(VerQueryValueW, (LPCVOID block, LPCWSTR sub, LPVOID* buffer, PUINT len), (block, sub, buffer, len))
