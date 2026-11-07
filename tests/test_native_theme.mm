#include "theme.h"
#include <QApplication>
#include <QDialog>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#import <AppKit/AppKit.h>
class TestNativeTheme : public QObject {
  Q_OBJECT
private slots:
  void windowAppearance() {
    if (QApplication::platformName() != "cocoa") QSKIP("Requires native Cocoa windows");
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings().setValue("appearance/theme", "system");
    ClientTheme::apply();
    QWidget main;
    main.show();
    QTest::qWait(30);
    NSWindow *native = [reinterpret_cast<NSView *>(main.winId()) window];
    QVERIFY(native);
    QSettings().setValue("appearance/theme", "dark");
    ClientTheme::apply();
    QVERIFY([native.appearance.name isEqualToString:NSAppearanceNameDarkAqua]);
    QDialog dialog(&main);
    dialog.show();
    QTest::qWait(30);
    NSWindow *nativeDialog = [reinterpret_cast<NSView *>(dialog.winId()) window];
    QVERIFY(nativeDialog);
    QVERIFY([nativeDialog.appearance.name isEqualToString:NSAppearanceNameDarkAqua]);
    QSettings().setValue("appearance/theme", "light");
    ClientTheme::apply();
    QVERIFY([native.appearance.name isEqualToString:NSAppearanceNameAqua]);
    QVERIFY([nativeDialog.appearance.name isEqualToString:NSAppearanceNameAqua]);
    QSettings().setValue("appearance/theme", "system");
    ClientTheme::apply();
    QVERIFY(native.appearance == nil);
    QVERIFY(nativeDialog.appearance == nil);
  }
};
QTEST_MAIN(TestNativeTheme)
#include "test_native_theme.moc"
