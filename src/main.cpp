#include "appservice.h"
#include "mainwindow.h"
#include "platform.h"
#include "theme.h"
#ifdef Q_OS_MACOS
#include "protocol_events.h"
#endif
#include <QApplication>
#include <QFileOpenEvent>
#include <QIcon>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTemporaryDir>
#include <functional>

class ClientApplication : public QApplication {
public:
    using QApplication::QApplication;
    std::function<void(QString)> openUrl;
    std::function<void()> onActivate;
    QStringList queued;
    void receiveUrl(const QString &url) {
        if (openUrl) openUrl(url); else queued.append(url);
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::ApplicationActivate && onActivate)
            onActivate();
        if (event->type() == QEvent::FileOpen) {
            const QUrl url = static_cast<QFileOpenEvent *>(event)->url();
            // Opaque JMS payloads must never be round-tripped through QUrl.
            // On macOS protocol_events.mm receives the original AppleEvent text.
            if (url.host() == "auth") receiveUrl(url.toString(QUrl::FullyEncoded));
            return true;
        }
        return QApplication::event(event);
    }
};

int main(int argc, char **argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    ClientApplication app(argc, argv);
#ifdef Q_OS_MACOS
    installProtocolEventHandler([&app](const QString &url) { app.receiveUrl(url); });
#endif
    QCoreApplication::setOrganizationName("FIT2CLOUD");
    QCoreApplication::setOrganizationDomain("fit2cloud.com");
    // Preserve the existing settings namespace while updating the visible brand.
    QCoreApplication::setApplicationName("client-qt");
    QApplication::setApplicationDisplayName("OSMClient");
    QCoreApplication::setApplicationVersion(CLIENT_QT_VERSION);
    QApplication::setWindowIcon(ClientTheme::applicationIcon());
    const bool smoke = app.arguments().contains("--smoke-test");
    QTemporaryDir smokeDirectory;
    if (smoke) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, smokeDirectory.path());
    }
    SingleInstance instance(nullptr, smoke ? "client-qt-smoke" : QString());
    if (!instance.start(app.arguments())) {
        if (!instance.error().isEmpty()) {
            qCritical("Cannot start client instance");
            return 1;
        }
        return 0;
    }
    ConfigStore config(nullptr, smoke ? smokeDirectory.filePath("config.json") : QString());
    AppService service;
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    auto raiseWindow = [&window] {
        if (window.windowState().testFlag(Qt::WindowMinimized))
            window.showNormal();
        else
            window.show();
        window.raise();
        window.activateWindow();
        QTimer::singleShot(0, &window, [&window] {
            window.raise();
            window.activateWindow();
        });
    };
#ifdef Q_OS_MACOS
    app.onActivate = [&window, raiseWindow] {
        if (!QApplication::activeWindow() && !window.isVisible()) raiseWindow();
        else if (window.windowState().testFlag(Qt::WindowMinimized)) raiseWindow();
    };
#endif
    auto route = [&window](const QString &url) {
        if (url.startsWith("jms://", Qt::CaseInsensitive)) window.handleUrl(url);
    };
    app.openUrl = route;
    QObject::connect(&instance, &SingleInstance::message, &window, [&](const QStringList &args) {
        bool routed = false;
        for (const auto &arg : args) if (arg.startsWith("jms://", Qt::CaseInsensitive)) { route(arg); routed = true; }
        if (!routed) raiseWindow();
    });
    using ClientTheme::text;
    auto *appMenu = window.menuBar()->addMenu("OSMClient");
#ifndef Q_OS_MACOS
    window.menuBar()->hide();
#endif
    auto *settings = appMenu->addAction(text("设置", "Settings"), &window, &MainWindow::showSettings);
    settings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    settings->setMenuRole(QAction::PreferencesRole);
    auto *about = appMenu->addAction(text("关于", "About"), &window, [&] {
        QMessageBox::about(&window, text("关于 OSMClient", "About OSMClient"),
            QStringLiteral("OSMClient %1\nQt %2\nFIT2CLOUD").arg(CLIENT_QT_VERSION, qVersion()));
    });
    about->setMenuRole(QAction::AboutRole);
    appMenu->addSeparator();
    auto *close = appMenu->addAction(text("关闭窗口", "Close window"), &window, &QWidget::close);
    close->setShortcut(QKeySequence::Close);
    auto *minimize = appMenu->addAction(text("最小化", "Minimize"), &window, &QWidget::showMinimized);
    auto *quit = appMenu->addAction(text("退出", "Quit"), &app, &QCoreApplication::quit);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    QSystemTrayIcon tray(QIcon(":/icons/icon.png"));
    QMenu trayMenu;
    auto *trayShow = trayMenu.addAction(text("显示窗口", "Show window"), &window, raiseWindow);
    auto *traySettings = trayMenu.addAction(text("设置", "Settings"), &window, &MainWindow::showSettings);
    trayMenu.addSeparator();
    auto *trayQuit = trayMenu.addAction(text("退出", "Quit"), &app, &QCoreApplication::quit);
    QObject::connect(&window, &MainWindow::preferencesChanged, &window, [&] {
        settings->setText(text("设置", "Settings"));
        about->setText(text("关于", "About"));
        close->setText(text("关闭窗口", "Close window"));
        minimize->setText(text("最小化", "Minimize"));
        quit->setText(text("退出", "Quit"));
        trayShow->setText(text("显示窗口", "Show window"));
        traySettings->setText(text("设置", "Settings"));
        trayQuit->setText(text("退出", "Quit"));
    });
    if (!smoke && QSystemTrayIcon::isSystemTrayAvailable()) {
        tray.setToolTip("OSMClient"); tray.setContextMenu(&trayMenu); tray.show();
        QObject::connect(&tray, &QSystemTrayIcon::activated, &window, [&](QSystemTrayIcon::ActivationReason why) {
            if (why == QSystemTrayIcon::Trigger || why == QSystemTrayIcon::DoubleClick) raiseWindow();
        });
    }
    window.show();
    QTimer::singleShot(0, &window, [&] {
        for (const auto &url : app.queued) route(url);
        app.queued.clear();
        for (const auto &arg : app.arguments()) if (arg.startsWith("jms://", Qt::CaseInsensitive)) route(arg);
    });
    if (smoke) QTimer::singleShot(500, &app, &QCoreApplication::quit);
    const int result = app.exec();
#ifdef Q_OS_MACOS
    installProtocolEventHandler({});
#endif
    return result;
}
