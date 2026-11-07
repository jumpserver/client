#include "native_theme.h"
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
void applyNativeWindowTheme(QWidget *window, const QString &mode, bool dark) {
#ifdef Q_OS_WIN
  if (!window->isWindow() || !window->internalWinId()) return;
  // Resolve dynamically so Windows 7/8 can start without newer DWM exports.
  HMODULE module = LoadLibraryW(L"dwmapi.dll");
  if (!module) return;
  using SetAttribute = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
  auto setAttribute = reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute"));
  if (setAttribute) {
    BOOL value = dark ? TRUE : FALSE;
    HWND handle = reinterpret_cast<HWND>(window->internalWinId());
    if (FAILED(setAttribute(handle, 20, &value, sizeof(value))))
      setAttribute(handle, 19, &value, sizeof(value));
    // Windows 11 supports explicit caption/text colors, including when the
    // application theme differs from the system theme. Older DWM versions
    // ignore these attributes and retain their native title-bar behavior.
    COLORREF caption = mode == "system" ? 0xffffffff : dark ? RGB(36,36,38) : RGB(250,250,250);
    COLORREF text = mode == "system" ? 0xffffffff : dark ? RGB(237,237,240) : RGB(48,52,59);
    setAttribute(handle, 35, &caption, sizeof(caption));
    setAttribute(handle, 36, &text, sizeof(text));
    RedrawWindow(handle, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
  }
  FreeLibrary(module);
#else
  Q_UNUSED(window); Q_UNUSED(mode); Q_UNUSED(dark);
#endif
}
