// Add TEMPORARY Registry rows for standard MSI write/remove actions. During
// uninstall, omit them if another application owns jms. Never edit persistent
// MSI rows or write/delete the user's registry from this immediate action.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
