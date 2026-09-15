// ResetBootDevice - Dell Command Configure BIOS storage-mode helper.
// The build places the required x64 CCTK payload in resources next to this EXE.

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <wbemidl.h>
#include <comdef.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <regex>
#include <sstream>
#include <fstream>

// Link required libraries
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fs = std::filesystem;

// Names of embedded resources (match resources.rc)
static const wchar_t* EMBED_RESOURCES[] = {
	L"R_CCTK_EXE",
	L"R_ABI_DLL",
	L"R_BIOSINTF_DLL",
	L"R_DCHAPI64_DLL",
	L"R_DCHBAS64_DLL",
	L"R_LIBCRYPTO_DLL",
	L"R_LIBSSL_DLL",
	L"R_CCTK_BAT10",
	L"R_CCTK_BAT11",
	L"R_FILE_TXT",
	NULL
};

static bool ExtractResourceToFile(const wchar_t* resName, const fs::path& outPath)
{
	HMODULE hm = GetModuleHandleW(NULL);
	HRSRC hRes = FindResourceW(hm, resName, RT_RCDATA);
	if (!hRes) return false;
	HGLOBAL hData = LoadResource(hm, hRes);
	if (!hData) return false;
	void* pdata = LockResource(hData);
	DWORD sz = SizeofResource(hm, hRes);
	if (!pdata || sz == 0) return false;
	std::ofstream out(outPath, std::ios::binary);
	if (!out) return false;
	out.write(reinterpret_cast<const char*>(pdata), sz);
	out.close();
	return true;
}

// Extract all embedded resources into a temp directory. Returns true on success and sets tempDir.
static bool ExtractAllEmbeddedResources(fs::path& tempDir)
{
	tempDir = fs::temp_directory_path() / fs::path(L"ResetBootDevice_emb");
	try { fs::remove_all(tempDir); fs::create_directories(tempDir); } catch (...) { return false; }
	for (const wchar_t** p = EMBED_RESOURCES; *p; ++p) {
		std::wstring name(*p);
		// derive filename from resource identifier by removing prefix R_ and converting underscores to dots for known types
		std::wstring fname = name.substr(2); // drop R_
		// map common ones to original filenames
		if (name == std::wstring(L"R_CCTK_EXE")) fname = L"cctk.exe";
		else if (name == std::wstring(L"R_ABI_DLL")) fname = L"ABI.dll";
		else if (name == std::wstring(L"R_BIOSINTF_DLL")) fname = L"BIOSIntf.dll";
		else if (name == std::wstring(L"R_DCHAPI64_DLL")) fname = L"dchapi64.dll";
		else if (name == std::wstring(L"R_DCHBAS64_DLL")) fname = L"dchbas64.dll";
		else if (name == std::wstring(L"R_LIBCRYPTO_DLL")) fname = L"libcrypto.dll";
		else if (name == std::wstring(L"R_LIBSSL_DLL")) fname = L"libssl.dll";
		else if (name == std::wstring(L"R_CCTK_BAT10")) fname = L"cctk_x86_64_winpe_10.bat";
		else if (name == std::wstring(L"R_CCTK_BAT11")) fname = L"cctk_x86_64_winpe_11.bat";
		else if (name == std::wstring(L"R_FILE_TXT")) fname = L"file";
		fs::path out = tempDir / fname;
		if (!ExtractResourceToFile(*p, out)) return false;
		// ensure executable bit for .exe/.bat if needed (on Windows not necessary)
	}
	return true;
}

static fs::path GetModuleDirectory()
{
	std::vector<wchar_t> buffer(32768);
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size()) return {};
	return fs::path(std::wstring(buffer.data(), length)).parent_path();
}

// Quote one argument using the CommandLineToArgvW/CreateProcess escaping rules.
static std::wstring QuoteArgument(const std::wstring& value)
{
	if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;

	std::wstring quoted = L"\"";
	size_t backslashes = 0;
	for (const wchar_t ch : value) {
		if (ch == L'\\') {
			++backslashes;
			continue;
		}
		if (ch == L'\"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(ch);
			backslashes = 0;
			continue;
		}
		quoted.append(backslashes, L'\\');
		backslashes = 0;
		quoted.push_back(ch);
	}
	quoted.append(backslashes * 2, L'\\');
	quoted.push_back(L'\"');
	return quoted;
}

static void SecureClear(std::wstring& value)
{
	if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
	value.clear();
}

static std::wstring Trim(std::wstring value)
{
	const auto first = value.find_first_not_of(L" \t\r\n");
	if (first == std::wstring::npos) return {};
	const auto last = value.find_last_not_of(L" \t\r\n");
	return value.substr(first, last - first + 1);
}

static bool RunProcessAndWait(const fs::path& exe, const std::wstring& args, DWORD* exitCodeOut = nullptr, bool sensitiveArgs = false)
{
	std::wstring cmd = QuoteArgument(exe.wstring()) + L" " + args;
	STARTUPINFOW si{};
	PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);
	// CreateProcess may modify the command buffer, so ensure it's writable
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(0);
	const std::wstring workingDirectory = exe.parent_path().wstring();
	if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi)) {
		// Do not print args here: CCTK args can contain the BIOS password.
		std::wcerr << L"CreateProcess failed for " << exe.wstring() << L" (Win32 error " << GetLastError() << L")\n";
		if (exitCodeOut) *exitCodeOut = GetLastError();
		if (sensitiveArgs) { SecureZeroMemory(cmdBuf.data(), cmdBuf.size() * sizeof(wchar_t)); SecureClear(cmd); }
		return false;
	}
	if (WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
		if (sensitiveArgs) { SecureZeroMemory(cmdBuf.data(), cmdBuf.size() * sizeof(wchar_t)); SecureClear(cmd); }
		return false;
	}
	DWORD exitCode = 0; GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
	if (sensitiveArgs) { SecureZeroMemory(cmdBuf.data(), cmdBuf.size() * sizeof(wchar_t)); SecureClear(cmd); }
	if (exitCodeOut) *exitCodeOut = exitCode;
	return exitCode == 0;
}

static bool RunProcessCaptureOutput(const fs::path& exe, const std::wstring& args, std::string& outStdout, DWORD* exitCodeOut = nullptr)
{
	SECURITY_ATTRIBUTES saAttr{};
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
	if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &saAttr, 0)) return false;
	if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(hStdOutRead); CloseHandle(hStdOutWrite); return false; }

	STARTUPINFOW si{}; PROCESS_INFORMATION pi{}; si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESTDHANDLES; si.hStdOutput = hStdOutWrite; si.hStdError = hStdOutWrite;

	std::wstring cmd = QuoteArgument(exe.wstring()) + L" " + args;
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
	const std::wstring workingDirectory = exe.parent_path().wstring();
	if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi)) {
		CloseHandle(hStdOutRead); CloseHandle(hStdOutWrite); return false;
	}

	CloseHandle(hStdOutWrite);
	const DWORD bufSize = 4096; char buffer[bufSize]; DWORD read = 0;
	outStdout.clear();
	for (;;) {
		BOOL ok = ReadFile(hStdOutRead, buffer, bufSize, &read, nullptr);
		if (!ok || read == 0) break;
		outStdout.append(buffer, buffer + read);
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 0;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hStdOutRead);
	if (exitCodeOut) *exitCodeOut = exitCode;
	return exitCode == 0;
}

static std::wstring StripGuidSuffix(const std::wstring& name)
{
	try {
		std::wregex r(LR"(\.[0-9]+\.[0-9A-Fa-f]{8}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{12}$)");
		if (std::regex_search(name, r)) return std::regex_replace(name, r, L"");
		std::wregex r2(LR"(_[0-9A-Fa-f]{8}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{4}_[0-9A-Fa-f]{12}$)");
		if (std::regex_search(name, r2)) return std::regex_replace(name, r2, L"");
	} catch (...) {}
	return name;
}

// WinRE normally exposes the firmware service tag through this hardware registry key,
// even when the optional WinPE-WMI component or the WMI service is unavailable.
static std::wstring GetSystemSerialNumberFromRegistry()
{
	DWORD bytes = 0;
	const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
	const wchar_t* valueName = L"SystemSerialNumber";
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
		return {};
	std::vector<wchar_t> value(bytes / sizeof(wchar_t));
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, valueName, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
		return {};
	return Trim(value.data());
}

// WMI fallback for environments where the firmware registry value is unavailable.
static std::wstring GetSystemSerialNumber()
{
	std::wstring serial = GetSystemSerialNumberFromRegistry();
	if (!serial.empty()) return serial;

	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	bool comInited = SUCCEEDED(hr);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return {};
	hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
	if (FAILED(hr) && hr != RPC_E_TOO_LATE) { if (comInited) CoUninitialize(); return {}; }

	IWbemLocator* pLoc = NULL;
	if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)) || !pLoc) { if (comInited) CoUninitialize(); return {}; }
	IWbemServices* pSvc = NULL;
	if (FAILED(pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc)) || !pSvc) { pLoc->Release(); if (comInited) CoUninitialize(); return {}; }
	if (FAILED(CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE))) {
		pSvc->Release(); pLoc->Release(); if (comInited) CoUninitialize(); return {};
	}

	IEnumWbemClassObject* pEnumerator = NULL;
	if (FAILED(pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT SerialNumber FROM Win32_BIOS"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator)) || !pEnumerator) { pSvc->Release(); pLoc->Release(); if (comInited) CoUninitialize(); return {}; }

	IWbemClassObject* pClsObj = NULL; ULONG uReturn = 0;
	if (pEnumerator->Next(WBEM_INFINITE, 1, &pClsObj, &uReturn) == S_OK && uReturn == 1) {
		VARIANT vtProp; VariantInit(&vtProp); if (SUCCEEDED(pClsObj->Get(L"SerialNumber", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR && vtProp.bstrVal) serial = std::wstring(vtProp.bstrVal);
		VariantClear(&vtProp); pClsObj->Release();
	}

	pEnumerator->Release(); pSvc->Release(); pLoc->Release(); if (comInited) CoUninitialize();
	return Trim(serial);
}

static std::wstring GetPasswordServiceUrl()
{
	const wchar_t* variableName = L"RESETBOOTDEVICE_PASSWORD_URL";
	const DWORD required = GetEnvironmentVariableW(variableName, nullptr, 0);
	if (required > 1) {
		std::vector<wchar_t> value(required);
		if (GetEnvironmentVariableW(variableName, value.data(), required) > 0)
			return Trim(value.data());
	}

	// Existing deployment default. WinRE callers can override it with the environment
	// variable above without rebuilding the utility.
	return L"https://defaulteb14b04624c445198f26b89c2159828.c.environment.api.gov.powerplatform.microsoft.us:443/powerautomate/automations/direct/workflows/29b29c13aead496a912617ef6429141a/triggers/manual/paths/invoke?api-version=1&sp=%2Ftriggers%2Fmanual%2Frun&sv=1.0&sig=5_0PssCMtAjTfBtJDYIXPsawUfFAwXJTp_nP4V-5p50";
}

// POST JSON and return response body
static bool PostReservationAndGetBase64Password(const std::wstring& url, const std::wstring& serial, std::string& outBase64)
{
	URL_COMPONENTS uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
	uc.dwSchemeLength = -1; uc.dwHostNameLength = -1; uc.dwUrlPathLength = -1; uc.dwExtraInfoLength = -1;
	std::wstring urlCopy = url;
	if (!WinHttpCrackUrl(urlCopy.c_str(), (DWORD)urlCopy.length(), 0, &uc)) return false;
	std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
	if (uc.dwExtraInfoLength > 0) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
	INTERNET_PORT port = uc.nPort;

	HINTERNET hSession = WinHttpOpen(L"ResetBootDevice/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return false; HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
	if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
	const DWORD requestFlags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
	if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

	std::string body; { std::wstring json = L"{\"Reservation\":\"" + serial + L"\"}"; int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), -1, NULL, 0, NULL, NULL); if (len > 0) { std::vector<char> buf(len); WideCharToMultiByte(CP_UTF8, 0, json.c_str(), -1, buf.data(), len, NULL, NULL); body.assign(buf.data()); } }

	static constexpr wchar_t requestHeaders[] = L"Content-Type: application/json\r\n";
	BOOL ok = WinHttpSendRequest(hRequest, requestHeaders, static_cast<DWORD>(std::size(requestHeaders) - 1), (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
	if (!ok) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
	ok = WinHttpReceiveResponse(hRequest, NULL);
	if (!ok) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
	DWORD statusCode = 0; DWORD statusCodeSize = sizeof(statusCode);
	if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX) ||
		statusCode < 200 || statusCode >= 300) {
		WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
	}

	outBase64.clear(); DWORD dwSize = 0; do { dwSize = 0; WinHttpQueryDataAvailable(hRequest, &dwSize); if (dwSize == 0) break; std::vector<char> buffer(dwSize + 1); DWORD dwDownloaded = 0; if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) break; outBase64.append(buffer.data(), dwDownloaded); } while (dwSize > 0);
	WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return !outBase64.empty();
}

// Decode base64
static std::wstring DecodeBase64ToWString(const std::string& base64)
{
	std::string payload = base64;
	const auto first = payload.find_first_not_of(" \t\r\n");
	const auto last = payload.find_last_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	payload = payload.substr(first, last - first + 1);
	// Power Automate commonly returns a JSON string rather than bare text.
	if (payload.size() >= 2 && payload.front() == '\"' && payload.back() == '\"')
		payload = payload.substr(1, payload.size() - 2);

	DWORD outLen = 0; if (!CryptStringToBinaryA(payload.c_str(), 0, CRYPT_STRING_BASE64, NULL, &outLen, NULL, NULL)) return {};
	std::vector<BYTE> buf(outLen); if (!CryptStringToBinaryA(payload.c_str(), 0, CRYPT_STRING_BASE64, buf.data(), &outLen, NULL, NULL)) return {};
	int wlen = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)buf.data(), (int)outLen, NULL, 0); if (wlen == 0) return {}; std::vector<wchar_t> wbuf(wlen + 1); MultiByteToWideChar(CP_UTF8, 0, (LPCCH)buf.data(), (int)outLen, wbuf.data(), wlen); return std::wstring(wbuf.data(), wlen);
}

// Find cctk.exe
static fs::path FindCctkExecutable(const fs::path& resourcesDir, const fs::path& adminExtractDir)
{
	for (const fs::path& directory : { resourcesDir, adminExtractDir }) {
		if (directory.empty() || !fs::exists(directory) || !fs::is_directory(directory)) continue;
		for (auto& p : fs::recursive_directory_iterator(directory)) {
			if (!p.is_regular_file()) continue;
			std::wstring name = p.path().filename().wstring();
			std::transform(name.begin(), name.end(), name.begin(), ::towlower);
			if (name == L"cctk.exe") return p.path();
		}
	}
	return {};
}

static bool ValidateWinReCctkPayload(const fs::path& cctkPath)
{
	static constexpr const wchar_t* requiredFiles[] = {
		L"cctk.exe", L"ABI.dll", L"BIOSIntf.dll", L"dchapi64.dll", L"dchbas64.dll", L"libcrypto.dll", L"libssl.dll"
	};
	const fs::path directory = cctkPath.parent_path();
	bool complete = true;
	for (const wchar_t* file : requiredFiles) {
		if (!fs::is_regular_file(directory / file)) {
			std::wcerr << L"Required WinRE CCTK file is missing: " << (directory / file).wstring() << L"\n";
			complete = false;
		}
	}
	return complete;
}

int wmain(int argc, wchar_t** argv)
{
	const fs::path moduleDir = GetModuleDirectory();
	if (moduleDir.empty()) { std::wcerr << L"Unable to determine application directory.\n"; return 1; }

	std::wstring outDir = L".";
	if (argc >= 2) outDir = argv[1];

	fs::path outPath = fs::absolute(outDir); try { fs::create_directories(outPath); } catch (...) { std::wcerr << L"Failed to create output directory\n"; return 1; }

	// Try extracting embedded resources first (will succeed if we built with resources.rc)
	fs::path embeddedDir;
	bool haveEmbedded = ExtractAllEmbeddedResources(embeddedDir);
	fs::path resourcesDir;
	if (haveEmbedded) {
		resourcesDir = embeddedDir;
		std::wcout << L"Using embedded resources extracted to: " << resourcesDir.wstring() << L"\n";
	} else {
		// Look for binaries under resources\Bins first, then fallback to resources on disk
		resourcesDir = moduleDir / L"resources" / L"Bins";
		if (!fs::exists(resourcesDir) || !fs::is_directory(resourcesDir)) {
			resourcesDir = moduleDir / L"resources"; // fallback
		}
		if (!fs::exists(resourcesDir) || !fs::is_directory(resourcesDir)) { std::wcerr << L"resources or resources\\Bins directory not found next to executable: " << (moduleDir / L"resources").wstring() << L"\n"; return 2; }
	}

	fs::path adminExtractDir; // declared early so it's in scope for direct cctk path handling
	fs::path msiToUse;
	// If cctk.exe is present directly in resources, skip MSI extraction flow.
	fs::path directCctk = FindCctkExecutable(resourcesDir, fs::path());
	bool skipMsiFlow = false;
	if (!directCctk.empty()) {
		std::wcout << L"Found cctk.exe directly in resources: " << directCctk.wstring() << L"\n";
		if (!ValidateWinReCctkPayload(directCctk)) return 5;
		// Skip MSI expansion and copying if cctk is present
		skipMsiFlow = true;
	}
	fs::path tempRoot;
	fs::path exeExtract;
	if (!skipMsiFlow) {
		tempRoot = fs::temp_directory_path() / fs::path(L"dell_tmp"); fs::remove_all(tempRoot); fs::create_directories(tempRoot);
		exeExtract = tempRoot / L"exe_expanded"; fs::create_directories(exeExtract);
		// Prefer an existing MSI in resources.
		for (auto& p : fs::directory_iterator(resourcesDir)) { if (!p.is_regular_file()) continue; if (p.path().extension()==L".msi") { msiToUse = p.path(); break; } }
		if (msiToUse.empty()) {
			// find exe
		fs::path exeInResources;
		for (auto& p : fs::directory_iterator(resourcesDir)) { if (!p.is_regular_file()) continue; auto ext = p.path().extension().wstring(); std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower); if (ext==L".exe") { std::wstring name = p.path().filename().wstring(); std::wstring lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower); if (lower.find(L"dell")!=std::wstring::npos || lower.find(L"cctk")!=std::wstring::npos || lower.find(L"configure")!=std::wstring::npos) { exeInResources = p.path(); break; } if (exeInResources.empty()) exeInResources = p.path(); } }
			if (exeInResources.empty()) { std::wcerr << L"No suitable EXE or MSI found in resources. Place the Dell installer or MSI in resources.\n"; return 3; }
		fs::path exeTemp = tempRoot / exeInResources.filename(); fs::copy_file(exeInResources, exeTemp, fs::copy_options::overwrite_existing);
		std::vector<std::wstring> attempts = { L"/x \"" + exeExtract.wstring() + L"\"", L"/extract \"" + exeExtract.wstring() + L"\"", L"/extractall \"" + exeExtract.wstring() + L"\"", L"-extract \"" + exeExtract.wstring() + L"\"", L"/s /x \"" + exeExtract.wstring() + L"\"", L"/silent /extract \"" + exeExtract.wstring() + L"\"" };
		bool msiFound = false;
		for (auto& at : attempts) {
			DWORD ec = 0; RunProcessAndWait(exeTemp, at, &ec);
			for (auto& p : fs::recursive_directory_iterator(exeExtract)) { if (!p.is_regular_file()) continue; if (p.path().extension()==L".msi") { msiToUse = p.path(); msiFound = true; break; } }
			if (msiFound) break;
		}
			if (!msiFound) { std::wcerr << L"Failed to locate MSI inside the installer EXE.\n"; return 4; }
		}
	}

	if (!skipMsiFlow) {
		std::wcout << L"MSI to use: " << msiToUse.wstring() << L"\n";

		adminExtractDir = tempRoot / L"msi_admin_expand"; fs::create_directories(adminExtractDir);
		{
			std::wstringstream ss; ss << L"/a \"" << msiToUse.wstring() << L"\" /qn TARGETDIR=\"" << adminExtractDir.wstring() << L"\"";
			std::wcout << L"Running msiexec " << ss.str() << L"\n";
				DWORD exitCode = 0; if (!RunProcessAndWait(fs::path(L"msiexec.exe"), ss.str(), &exitCode)) { std::wcerr << L"msiexec administrative install failed (exit=" << exitCode << L")\n"; return 6; }
		}

		for (auto& p : fs::recursive_directory_iterator(adminExtractDir)) {
			if (!p.is_regular_file()) continue;
			std::wstring fname = p.path().filename().wstring(); std::wstring clean = StripGuidSuffix(fname);
			fs::path dest = outPath / clean; int idx = 1; fs::path base = dest.stem(); fs::path ext = dest.extension();
			while (fs::exists(dest)) { std::wstringstream ss; ss << base.wstring() << L"_" << idx << ext.wstring(); dest = outPath / ss.str(); idx++; }
			try { fs::copy_file(p.path(), dest); std::wcout << L"Copied " << p.path().wstring() << L" -> " << dest.wstring() << L"\n"; }
			catch (const fs::filesystem_error& e) { std::wcerr << L"Failed to copy " << p.path().wstring() << L": " << e.what() << L"\n"; }
		}
	}

	// Network to get BIOS password and toggle storage
	std::wcout << L"Attempting to retrieve BIOS password and toggle storage mode...\n";
	std::wstring serial = GetSystemSerialNumber();
	if (serial.empty()) { std::wcerr << L"Unable to determine the firmware service tag from the registry or WMI.\n"; return 7; }
	else {
			std::string base64Resp; const std::wstring serviceUrl = GetPasswordServiceUrl();
			if (serviceUrl.empty()) { std::wcerr << L"Password service URL is not configured.\n"; return 8; }
			if (PostReservationAndGetBase64Password(serviceUrl, serial, base64Resp)) {
				std::wstring pwd = DecodeBase64ToWString(base64Resp);
				if (pwd.empty()) { std::wcerr << L"Failed to decode password from service response.\n"; return 8; }
				else {
				while (!pwd.empty() && (pwd.back()==L'\n' || pwd.back()==L'\r' || pwd.back()==L' ')) pwd.pop_back();
				fs::path cctkPath;
				if (!directCctk.empty()) cctkPath = directCctk;
				else cctkPath = FindCctkExecutable(resourcesDir, adminExtractDir);
					if (cctkPath.empty()) { SecureZeroMemory(pwd.data(), pwd.size() * sizeof(wchar_t)); std::wcerr << L"cctk.exe not found in resources or extracted MSI.\n"; return 3; }
				else {
					std::wcout << L"Found cctk: " << cctkPath.wstring() << L"\n";
					std::string probeOut; DWORD probeExit = 0;
					if (!RunProcessCaptureOutput(cctkPath, L"--embsataraid", probeOut, &probeExit)) {
						std::wcerr << L"cctk storage-mode query failed (exit=" << probeExit << L").\n";
						SecureZeroMemory(pwd.data(), pwd.size() * sizeof(wchar_t));
						return 9;
					}
					std::smatch modeMatch;
					const std::regex modePattern(R"(embsataraid\s*=\s*(ahci|raid(?:on)?))", std::regex_constants::icase);
					if (!std::regex_search(probeOut, modeMatch, modePattern)) {
						SecureZeroMemory(pwd.data(), pwd.size() * sizeof(wchar_t));
						std::wcerr << L"Unable to determine current storage mode from cctk output.\n";
						return 11;
					}
					else {
						std::string currentMode = modeMatch[1].str();
						std::transform(currentMode.begin(), currentMode.end(), currentMode.begin(), ::tolower);
						const bool isAhci = currentMode == "ahci";
						const std::wstring targetMode = isAhci ? L"Raid" : L"Ahci";
						std::wcout << L"Current mode " << (isAhci ? L"AHCI" : L"RAID") << L", switching to " << targetMode << L"\n";
					std::wstring passwordArgument = L"--ValSetupPwd=" + pwd;
					std::wstring setArgs = QuoteArgument(passwordArgument) + L" " + QuoteArgument(L"--EmbSataRaid=" + targetMode);
					DWORD ec = 0;
					const bool setSucceeded = RunProcessAndWait(cctkPath, setArgs, &ec, true);
					SecureClear(setArgs);
					SecureClear(passwordArgument);
					SecureClear(pwd);
						if (!setSucceeded) { std::wcerr << L"Failed to run cctk to set storage mode (exit=" << ec << L")\n"; return 10; }
						std::wcout << L"cctk set command executed successfully.\n";
					}
				}
			}
			} else { std::wcerr << L"Failed to contact password service or the service returned a non-success status.\n"; return 8; }
	}

	std::wcout << L"Operation complete. Output: " << outPath.wstring() << L"\n";
	try { if (!tempRoot.empty()) fs::remove_all(tempRoot); } catch (...) {}
	return 0;
}
