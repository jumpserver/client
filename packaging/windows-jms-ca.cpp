// Add TEMPORARY Registry rows for standard MSI write/remove actions. During
// uninstall, omit them if another application owns jms. Never edit persistent
// MSI rows or write/delete the user's registry from this immediate action.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // QueryFullProcessImageNameW, CompareStringOrdinal
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <msi.h>
#include <msiquery.h>

// No C/C++ runtime dependency: Windows Installer extracts this DLL to a temporary
// directory before our payload or its app-local CRT can be assumed available.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

static bool equalText(const WCHAR *left, const WCHAR *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static bool ownsRegistration(MSIHANDLE session)
{
    MSIHANDLE format = MsiCreateRecord(0);
    if (!format) return false;
    MsiRecordSetStringW(format, 0, L"\"[#MainExecutable]\" \"%1\"");
    DWORD length = 0;
    WCHAR empty = 0;
    UINT status = MsiFormatRecordW(session, format, &empty, &length);
    if (status != ERROR_MORE_DATA) { MsiCloseHandle(format); return false; }
    if (length > 32768) { MsiCloseHandle(format); return false; }
    auto *expected = static_cast<WCHAR *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (length + 1) * sizeof(WCHAR)));
    if (!expected) { MsiCloseHandle(format); return false; }
    ++length;
    status = MsiFormatRecordW(session, format, expected, &length);
    MsiCloseHandle(format);
    if (status != ERROR_SUCCESS) { HeapFree(GetProcessHeap(), 0, expected); return false; }

    bool owns = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\jms\\shell\\open\\command", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        DWORD type = 0, bytes = 0;
        if (RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS
                && type == REG_SZ && bytes > 0 && bytes < 65536) {
            auto *current = static_cast<WCHAR *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + sizeof(WCHAR)));
            if (current) {
                if (RegQueryValueExW(key, nullptr, nullptr, &type,
                                    reinterpret_cast<BYTE *>(current), &bytes) == ERROR_SUCCESS)
                    owns = equalText(current, expected);
                HeapFree(GetProcessHeap(), 0, current);
            }
        }
        RegCloseKey(key);
    }
    HeapFree(GetProcessHeap(), 0, expected);
    return owns;
}

extern "C" __declspec(dllexport) UINT __stdcall PrepareJmsRegistration(MSIHANDLE session)
{
    INSTALLSTATE installed = INSTALLSTATE_UNKNOWN, action = INSTALLSTATE_UNKNOWN;
    if (MsiGetComponentStateW(session, L"JmsRegistration", &installed, &action) != ERROR_SUCCESS)
        return ERROR_INSTALL_FAILURE;
    const bool present = installed == INSTALLSTATE_LOCAL || installed == INSTALLSTATE_SOURCE;
    const bool removing = present && (action == INSTALLSTATE_ABSENT || action == INSTALLSTATE_REMOVED);
    const bool writing = action == INSTALLSTATE_LOCAL || action == INSTALLSTATE_SOURCE
                      || (action == INSTALLSTATE_DEFAULT && present);
    if (!removing && !writing) return ERROR_SUCCESS;
    if (removing && !ownsRegistration(session)) return ERROR_SUCCESS;

    MSIHANDLE database = MsiGetActiveDatabase(session);
    if (!database) return ERROR_INSTALL_FAILURE;
    MSIHANDLE view = 0;
    UINT status = MsiDatabaseOpenViewW(database,
        L"INSERT INTO `Registry` (`Registry`, `Root`, `Key`, `Name`, `Value`, `Component_`) "
        L"VALUES (?, ?, ?, ?, ?, ?) TEMPORARY", &view);
    if (status != ERROR_SUCCESS) { MsiCloseHandle(database); return ERROR_INSTALL_FAILURE; }
    struct RegistryRow { const WCHAR *id, *key, *name, *value; };
    static const RegistryRow rows[] = {
        {L"JmsDescription", L"Software\\Classes\\jms", nullptr, L"URL:JumpServer connection"},
        {L"JmsProtocolFlag", L"Software\\Classes\\jms", L"URL Protocol", L""},
        {L"JmsIcon", L"Software\\Classes\\jms\\DefaultIcon", nullptr, L"\"[#MainExecutable]\",0"},
        {L"JmsCommand", L"Software\\Classes\\jms\\shell\\open\\command", nullptr, L"\"[#MainExecutable]\" \"%1\""}
    };
    for (const auto &row : rows) {
        MSIHANDLE record = MsiCreateRecord(6);
        if (!record) { status = ERROR_OUTOFMEMORY; break; }
        MsiRecordSetStringW(record, 1, row.id);
        MsiRecordSetInteger(record, 2, 2); // HKLM
        MsiRecordSetStringW(record, 3, row.key);
        MsiRecordSetStringW(record, 4, row.name);
        MsiRecordSetStringW(record, 5, row.value);
        MsiRecordSetStringW(record, 6, L"JmsRegistration");
        status = MsiViewExecute(view, record);
        MsiCloseHandle(record);
        if (status != ERROR_SUCCESS) break;
        // Reusing a view requires closing the previous execution's result set.
        status = MsiViewClose(view);
        if (status != ERROR_SUCCESS) break;
    }
    MsiCloseHandle(view);
    MsiCloseHandle(database);
    return status == ERROR_SUCCESS ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

// ---------------------------------------------------------------------------
// Close clients running from an installation that is about to be replaced or
// removed. Immediate, scheduled after InstallValidate and before InstallInitialize
// (deferred actions may not run before InstallInitialize, where the major
// upgrade removes the old product). OSMCLOSEFOLDERS is "dir|dir|...". Only processes whose
// image lies inside one of those absolute, non-root folders are closed; the
// image name alone is never used. Failures never fail the installation: files
// still in use are then handled by Windows Installer as usual.
// No CRT: no large stack arrays and no aggregate zero-initialisation, which
// could emit __chkstk/memset calls.

static void logClient(MSIHANDLE session, const WCHAR *state, const WCHAR *path)
{
    MSIHANDLE record = MsiCreateRecord(2);
    if (!record) return;
    MsiRecordSetStringW(record, 0, L"OSMClient CloseInstalledClients: [1] [2]");
    MsiRecordSetStringW(record, 1, state);
    MsiRecordSetStringW(record, 2, path);
    MsiProcessMessage(session, INSTALLMESSAGE_INFO, record);
    MsiCloseHandle(record);
}

static void closeClientsUnder(MSIHANDLE session, const WCHAR *folder, WCHAR *image, DWORD imageCapacity)
{
    WCHAR dir[MAX_PATH + 2];
    int length = lstrlenW(folder);
    if (length < 4 || length > MAX_PATH - 1) return;
    lstrcpynW(dir, folder, MAX_PATH);
    while (length > 0 && dir[length - 1] == L'\\') --length;
    // Absolute drive path below the root only, e.g. C:\Program Files\OSMClient.
    if (length < 4 || dir[1] != L':' || dir[2] != L'\\') return;
    dir[length++] = L'\\';
    dir[length] = 0;
    const DWORD self = GetCurrentProcessId();
    for (int pass = 0; pass < 2; ++pass) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == self) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                         FALSE, entry.th32ProcessID);
            if (!process) continue;
            DWORD size = imageCapacity;
            if (QueryFullProcessImageNameW(process, 0, image, &size) && size > DWORD(length)
                    && CompareStringOrdinal(image, length, dir, length, TRUE) == CSTR_EQUAL) {
                if (pass == 0) {
                    logClient(session, L"closing", image);
                    if (TerminateProcess(process, 1)) WaitForSingleObject(process, 5000);
                } else {
                    logClient(session, L"still running", image);
                }
            }
            CloseHandle(process);
        }
        CloseHandle(snapshot);
    }
}

extern "C" __declspec(dllexport) UINT __stdcall CloseInstalledClients(MSIHANDLE session)
{
    DWORD length = 0;
    WCHAR empty = 0;
    if (MsiGetPropertyW(session, L"OSMCLOSEFOLDERS", &empty, &length) != ERROR_MORE_DATA
            || length == 0 || length > 8192)
        return ERROR_SUCCESS;
    ++length;
    const DWORD imageCapacity = 32768;
    auto *data = static_cast<WCHAR *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (length + 1) * sizeof(WCHAR)));
    auto *image = static_cast<WCHAR *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, imageCapacity * sizeof(WCHAR)));
    if (data && image && MsiGetPropertyW(session, L"OSMCLOSEFOLDERS", data, &length) == ERROR_SUCCESS) {
        WCHAR *segment = data;
        for (WCHAR *p = data;; ++p) {
            const bool last = *p == 0;
            if (last || *p == L'|') {
                *p = 0;
                closeClientsUnder(session, segment, image, imageCapacity);
                if (last) break;
                segment = p + 1;
            }
        }
    }
    if (image) HeapFree(GetProcessHeap(), 0, image);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    return ERROR_SUCCESS;
}
