#include "native_theme.h"
#include <QApplication>
#include <QWidget>
#import <AppKit/AppKit.h>
void applyNativeWindowTheme(QWidget *window, const QString &mode, bool dark) {
  if (QApplication::platformName() != QStringLiteral("cocoa") ||
      !window->isWindow() || !window->internalWinId()) return;
  NSView *view = reinterpret_cast<NSView *>(window->internalWinId());
  NSWindow *nativeWindow = [view window];
  if (!nativeWindow) return;
  // nil restores inheritance from the system, including later appearance changes.
  nativeWindow.appearance = mode == QStringLiteral("system") ? nil :
      [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}
