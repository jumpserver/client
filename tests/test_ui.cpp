#include "appservice.h"
#include "connectiondialog.h"
#include "mainwindow.h"
#include "platform.h"
#include "settingsdialog.h"
#include "theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QStatusBar>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSignalBlocker>
#include <QSettings>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTextDocument>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QtTest>
class TestUi : public QObject {
  Q_OBJECT
private:
  void chooseMethod(ConnectionDialog &dialog, const QString &value) {
    auto *tabs = dialog.findChild<QTabWidget *>("connectionMethodTabs");
    for (int i = 0; i < tabs->count(); ++i) {
      auto *choices = tabs->widget(i)->findChild<QComboBox *>("method");
      const int index = choices->findData(value);
      if (index >= 0) {
        tabs->setCurrentIndex(i);
        choices->setCurrentIndex(index);
        return;
      }
    }
    QFAIL("Requested method is missing");
  }
  // Rebuilt cards stay alive until deleteLater() runs, so only unhidden cards
  // belong to the protocol currently selected.
  QList<QFrame *> applicationCards(QWidget *root) {
    QList<QFrame *> result;
    for (auto *card : root->findChildren<QFrame *>("appCard"))
      if (!card->isHidden())
        result.append(card);
    return result;
  }
  QPushButton *applicationProtocolButton(QWidget *root, const QString &protocol) {
    for (auto *button : root->findChildren<QPushButton *>("applicationProtocol"))
      if (button->property("protocol").toString() == protocol)
        return button;
    return nullptr;
  }
  QStringList supportedApplicationNames(ConfigStore *config,
                                        const QString &category,
                                        const QString &protocol) {
    QStringList result;
    for (const auto &value : config->applications().value(category).toArray()) {
      const QJsonObject item = value.toObject();
      for (const auto &supported : item.value("protocol").toArray())
        if (supported.toString() == protocol) {
          result << item.value("display_name").toString();
          break;
        }
    }
    return result;
  }
  int defaultApplicationCount(ConfigStore *config, const QString &category,
                              const QString &protocol) {
    int count = 0;
    for (const auto &value : config->applications().value(category).toArray())
      for (const auto &matched : value.toObject().value("match_first").toArray())
        if (matched.toString() == protocol)
          ++count;
    return count;
  }
  QLabel *applicationNotice(QWidget *root) {
    QList<QLabel *> found;
    for (auto *label : root->findChildren<QLabel *>())
      if (label->property("role").toString() == "validation")
        found.append(label);
    return found.isEmpty() ? nullptr : found.first();
  }
  QTemporaryDir directory;
  QJsonObject asset{{"id", "asset"}, {"name", "Test server"}};
  QJsonObject details{
      {"permed_protocols",
       QJsonArray{QJsonObject{{"name", "ssh"}}, QJsonObject{{"name", "rdp"}},
                  QJsonObject{{"name", "http"}},
                  QJsonObject{{"name", "winrm"}}}},
      {"permed_accounts",
       QJsonArray{
           QJsonObject{{"id", "managed"}, {"name", "root"}, {"alias", "root"}},
           QJsonObject{
               {"id", "manual"}, {"name", "Manual"}, {"alias", "@INPUT"}},
           QJsonObject{
               {"id", "dynamic"}, {"name", "Dynamic"}, {"alias", "@USER"}}}}};
  QJsonObject methods{
      {"ssh",
       QJsonArray{
           QJsonObject{
               {"value", "ssh_client"}, {"label", "SSH"}, {"type", "native"}},
           QJsonObject{{"value", "ssh_guide"}, {"type", "native"}},
           QJsonObject{{"value", "web_cli"}, {"type", "web"}},
           QJsonObject{
               {"value", "disabled"}, {"type", "native"}, {"disabled", true}}}},
      {"rdp", QJsonArray{QJsonObject{{"value", "mstsc"}, {"type", "native"}}}},
      {"http", QJsonArray{QJsonObject{{"value", "chrome"}, {"type", "applet"}},
                          QJsonObject{{"value", "browser"}, {"type", "web"}}}}};
  QJsonArray visualAssets() const {
    const QStringList names{
        "production-web-01-with-a-deliberately-long-display-name",
        "测试数据库", "北京跳板机", "文件服务器", "DB2 数据库", "灾备应用（已禁用）"};
    QJsonArray assets;
    for (int row = 0; row < names.size(); ++row) {
      const QString type = row == 1 ? "mysql" : (row == 3 ? "windows" : (row == 4 ? "db2" : "linux"));
      assets.append(QJsonObject{
          {"id", QString::number(row)}, {"name", names[row]},
          {"address", QString("192.0.2.%1").arg(10 + row)},
          {"type", QJsonObject{{"value", type}}},
          {"platform", QJsonObject{{"name", type}}}, {"is_active", row != 5},
          {"permed_accounts", row == 2 ? QJsonArray{} : QJsonArray{QJsonObject{{"name", "Synthetic operator"}}}},
          {"permed_protocols", row == 2 ? QJsonArray{} : QJsonArray{QJsonObject{{"name", row == 1 ? "mysql" : (row == 4 ? "db2" : "ssh")}}}}});
    }
    return assets;
  }
  QString layoutProblem(QWidget *container) const {
    const auto children = container->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    QList<QWidget *> visible;
    for (auto *child : children) {
      if (!child->isVisible() || child->isWindow())
        continue;
      if (!container->rect().contains(child->geometry()))
        return QString("%1 extends outside %2").arg(child->objectName(), container->objectName());
      for (auto *other : visible)
        if (child->geometry().intersects(other->geometry()))
          return QString("%1 overlaps %2 in %3").arg(child->objectName(), other->objectName(), container->objectName());
      visible.append(child);
      const QString nested = layoutProblem(child);
      if (!nested.isEmpty())
        return nested;
    }
    return {};
  }
  QRect paintedConnectBounds(QListWidget *grid) const {
    // Measure the actual painted button on an unselected active card, instead
    // of duplicating the delegate's private button-width calculation.
    const QSize size = grid->visualItemRect(grid->item(1)).size();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QStyleOptionViewItem option;
    option.initFrom(grid);
    option.rect = image.rect();
    option.font = grid->font();
    option.fontMetrics = QFontMetrics(option.font);
    option.state &= ~QStyle::State_Selected;
    {
      QPainter painter(&image);
      grid->itemDelegate()->paint(&painter, option, grid->model()->index(1, 0));
    }
    QRect bounds;
    const QColor fill = ClientTheme::colors().softAccent;
    for (int y = 0; y < image.height(); ++y)
      for (int x = image.width() / 2; x < image.width(); ++x)
        if (image.pixelColor(x, y).rgba() == fill.rgba())
          bounds = bounds.united(QRect(x, y, 1, 1));
    return bounds;
  }
  QRect logoColorBounds(const QImage &image) const {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const QColor color = image.pixelColor(x, y);
        // The logo is green/blue; exclude its transparent background and the
        // neutral theme surface when measuring the rendered shield.
        if (color.alpha() > 128 && color.saturation() > 100 && color.value() > 64)
          bounds = bounds.united(QRect(x, y, 1, 1));
      }
    return bounds;
  }
  void capture(QWidget &widget, const QString &name) {
    const QString output = qEnvironmentVariable("CLIENT_QT_SCREENSHOT_DIR");
    if (output.isEmpty())
      return;
    QVERIFY(QDir().mkpath(output));
    const QPixmap screenshot = widget.grab();
    QVERIFY(screenshot.save(output + "/" + name + ".png"));
    QFile metadata(output + "/geometry.jsonl");
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Append));
    const QJsonObject record{{"name", name}, {"logicalWidth", widget.width()},
                             {"logicalHeight", widget.height()},
                             {"devicePixelRatio", screenshot.devicePixelRatioF()},
                             {"pixelWidth", screenshot.width()}, {"pixelHeight", screenshot.height()}};
    metadata.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
  }
public slots:
  void browser(QUrl) {
  } // Never open a real browser from the local OAuth fixture.
private slots:
  void protocolMenuUsesCurrentAuthorization() {
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/language", "en");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int detailRequests = 0, methodRequests = 0;
    bool revoke = false, failDetail = false;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
          const QByteArray request = socket->readAll();
          if (!request.contains("\r\n\r\n")) return;
          QJsonObject response;
          if (request.contains("/connect-methods/")) {
            ++methodRequests;
            response = {{"ssh", QJsonArray{QJsonObject{{"type", "native"}, {"value", "ssh_client"}, {"endpoint_protocol", "ssh"}}}},
                        {"db2", QJsonArray{QJsonObject{{"type", "applet"}, {"value", "db2_app"}, {"listen", "rdp"}}}},
                        {"invalid", QJsonArray{QJsonObject{{"type", "native"}, {"value", "db_client"}, {"endpoint_protocol", "db2"}}}},
                        {"webonly", QJsonArray{QJsonObject{{"type", "web"}, {"value", "web_gui"}}}}};
          } else {
            ++detailRequests;
            if (failDetail) {
              socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
              socket->disconnectFromHost();
              return;
            }
            response = {{"permed_protocols", revoke ? QJsonArray{} : QJsonArray{QJsonObject{{"name", "ssh"}}, QJsonObject{{"name", "db2"}}, QJsonObject{{"name", "invalid"}}, QJsonObject{{"name", "webonly"}}}},
                        {"permed_accounts", QJsonArray{QJsonObject{{"id", "acct"}, {"name", "root"}}}}};
          }
          const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    AppService service;
    service.m_site = QString("http://127.0.0.1:%1").arg(server.serverPort());
    service.m_access = "synthetic";
    service.m_org = "fixture";
    ConfigStore config(nullptr, directory.filePath("protocol-menu-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    emit service.assetsReady(visualAssets(), 6);
    window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    QTest::qWait(30);
    const QPoint position = grid->visualItemRect(grid->item(0)).center();
    QStringList seen;
    bool dialogOpened = false;
    for (int phase = 0; phase < 3; ++phase) {
      QTimer poll;
      poll.setInterval(10);
      connect(&poll, &QTimer::timeout, &window, [&] {
        auto *menu = window.findChild<QMenu *>("protocolMenu");
        if (!menu || !menu->isEnabled()) return;
        poll.stop();
        for (auto *action : menu->actions()) seen.append(action->text());
        if (phase == 0) {
          auto *action = menu->actions().last();
          QTimer::singleShot(40, &window, [&] {
            auto *dialog = window.findChild<ConnectionDialog *>();
            if (dialog) {
              dialogOpened = true;
              QCOMPARE(dialog->requestBody()["protocol"].toString(), QString("ssh"));
              dialog->reject();
            }
          });
          action->trigger();
        } else if (phase == 2) {
          failDetail = false;
          revoke = false;
          menu->actions().last()->trigger();
        }
        if (auto *parent = qobject_cast<QMenu *>(menu->parentWidget())) parent->close();
      });
      QTimer::singleShot(3000, &window, [&window] {
        if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) popup->close();
      });
      poll.start();
      QMetaObject::invokeMethod(grid, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, position));
      poll.stop();
      if (phase == 0) {
        QCOMPARE(seen, QStringList({"DB2", "SSH"}));
        QVERIFY(dialogOpened);
        QCOMPARE(detailRequests, 1); // Reuse the menu's authorization in the dialog.
        QCOMPARE(methodRequests, 1);
        seen.clear();
        revoke = true;
      } else if (phase == 1) {
        QCOMPARE(seen, QStringList({"No authorized protocols"}));
        QCOMPARE(detailRequests, 2); // A fresh authorization is always requested.
        QCOMPARE(methodRequests, 1); // Short-lived methods cache is reused.
        seen.clear();
        failDetail = true;
      } else {
        QCOMPARE(seen, QStringList({"Failed to load asset permissions", "Retry"}));
        QCOMPARE(detailRequests, 3);
        bool retried = false;
        QTimer retryPoll;
        retryPoll.setInterval(10);
        connect(&retryPoll, &QTimer::timeout, &window, [&] {
          auto *retryMenu = window.findChild<QMenu *>("protocolMenu");
          if (!retryMenu || !retryMenu->isEnabled()) return;
          QStringList choices;
          for (auto *action : retryMenu->actions()) choices.append(action->text());
          if (choices != QStringList({"DB2", "SSH"})) return;
          retried = true;
          retryPoll.stop();
          if (auto *parent = qobject_cast<QMenu *>(retryMenu->parentWidget())) parent->close();
        });
        retryPoll.start();
        QTRY_VERIFY_WITH_TIMEOUT(retried, 3000);
        QCOMPARE(detailRequests, 4);
      }
    }
    // Switching away and back closes the open menu; the old actions cannot
    // become valid again simply because the textual scope matches.
    QPointer<QMenu> oldMenu;
    QTimer::singleShot(30, &window, [&] {
      oldMenu = window.findChild<QMenu *>("protocolMenu");
      service.setOrg("different");
      service.setOrg("fixture");
    });
    QMetaObject::invokeMethod(grid, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, position));
    QVERIFY(oldMenu.isNull());
    // A disabled asset never issues a protocol lookup.
    const int previousDetailRequests = detailRequests;
    grid->scrollToItem(grid->item(5));
    QTest::qWait(20);
    const QPoint disabledPos = grid->visualItemRect(grid->item(5)).center();
    QTimer::singleShot(30, &window, [&] {
      auto *disabled = window.findChild<QMenu *>("protocolMenu");
      QVERIFY(disabled);
      QVERIFY(!disabled->isEnabled());
      if (auto *parent = qobject_cast<QMenu *>(disabled->parentWidget())) parent->close();
    });
    QMetaObject::invokeMethod(grid, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, disabledPos));
    QCOMPARE(detailRequests, previousDetailRequests);
  }
  void normalConnectionDoesNotDependOnClosedMenuLookup() {
    QSettings().setValue("general/restoreLogin", false);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QPointer<QTcpSocket> oldMethods;
    int methodsRequests = 0;
    const auto reply = [](QTcpSocket *socket, const QJsonObject &data) {
      const QByteArray body = QJsonDocument(data).toJson(QJsonDocument::Compact);
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
      socket->disconnectFromHost();
    };
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
          const QByteArray request = socket->readAll();
          if (!request.contains("\r\n\r\n")) return;
          if (request.contains("/connect-methods/")) {
            if (++methodsRequests == 1) { oldMethods = socket; return; }
            reply(socket, QJsonObject{{"ssh", QJsonArray{QJsonObject{{"type", "native"}, {"value", "ssh_client"}, {"endpoint_protocol", "ssh"}}}}});
          } else {
            reply(socket, QJsonObject{{"permed_protocols", QJsonArray{QJsonObject{{"name", "ssh"}}}},
                                     {"permed_accounts", QJsonArray{QJsonObject{{"id", "acct"}, {"name", "root"}}}}});
          }
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    AppService service;
    service.m_site = QString("http://127.0.0.1:%1").arg(server.serverPort());
    service.m_access = "synthetic";
    service.m_org = "fixture";
    ConfigStore config(nullptr, directory.filePath("protocol-menu-race.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    emit service.assetsReady(visualAssets(), 6);
    window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    QTest::qWait(30);
    const QPoint position = grid->visualItemRect(grid->item(0)).center();
    bool opened = false;
    QTimer poll;
    poll.setInterval(10);
    connect(&poll, &QTimer::timeout, &window, [&] {
      if (auto *dialog = window.findChild<ConnectionDialog *>()) {
        opened = true;
        QCOMPARE(dialog->requestBody()["connect_method"].toString(), QString("ssh_client"));
        dialog->reject();
        poll.stop();
      } else if (oldMethods) {
        auto *submenu = window.findChild<QMenu *>("protocolMenu");
        auto *parent = submenu ? qobject_cast<QMenu *>(submenu->parentWidget()) : nullptr;
        if (parent && parent->isVisible()) parent->actions().first()->trigger();
      }
    });
    QTimer::singleShot(3000, &window, [&] {
      if (auto *dialog = window.findChild<ConnectionDialog *>()) dialog->reject();
      if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) popup->close();
    });
    poll.start();
    QMetaObject::invokeMethod(grid, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, position));
    QTRY_VERIFY_WITH_TIMEOUT(opened, 3000);
    QCOMPARE(methodsRequests, 2);
    if (oldMethods) reply(oldMethods, QJsonObject{{"stale", QJsonArray{}}});
  }
  void systemThemeUpdatesImmediately() {
    QSettings().setValue("appearance/theme", "light");
    ClientTheme::apply();
    QPalette systemDark = qApp->palette();
    systemDark.setColor(QPalette::Window, QColor("#151515"));
    qApp->setPalette(systemDark);
    QTRY_COMPARE(ClientTheme::colors().background, QColor("#fafafa"));
    QSettings().setValue("appearance/theme", "system");
    ClientTheme::apply();
    QCOMPARE(ClientTheme::colors().background, QColor("#242426"));
    QPalette systemLight = qApp->palette();
    systemLight.setColor(QPalette::Window, QColor("#f7f7f7"));
    qApp->setPalette(systemLight);
    QTRY_COMPARE(ClientTheme::colors().background, QColor("#fafafa"));
    QSettings().setValue("appearance/theme", "dark");
    ClientTheme::apply();
    QCOMPARE(ClientTheme::colors().background, QColor("#242426"));
    QSettings().setValue("appearance/theme", "system");
    ClientTheme::apply();
    QCOMPARE(ClientTheme::colors().background, QColor("#fafafa"));
  }
  void initTestCase() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Match main.cpp so screenshot fixtures request native-resolution icons.
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QCoreApplication::setOrganizationName("ClientQtUiTests");
    QCoreApplication::setApplicationName("ui");
    QCoreApplication::setApplicationVersion("4.1.2");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       directory.path());
    QDesktopServices::setUrlHandler("http", this, "browser");
  }
  void cleanupTestCase() { QDesktopServices::unsetUrlHandler("http"); }
  void init() { QSettings().clear(); }
  void startupRestoreTransition_data() {
    QTest::addColumn<QString>("outcome");
    QTest::newRow("cancel-before-restore") << QString("cancel");
    QTest::newRow("no-saved-session") << QString("empty");
    QTest::newRow("restore-error") << QString("error");
  }
  void startupRestoreTransition() {
    QFETCH(QString, outcome);
    QSettings().setValue("general/restoreLogin", true);
    QSettings().setValue("lastSite", "https://restore-fixture.invalid");
    AppService service;
    ConfigStore config(nullptr, directory.filePath("restore-ui-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    auto *pages = window.findChild<QStackedWidget *>("contentPages");
    auto *restore = window.findChild<QWidget *>("restoreSessionPage");
    auto *login = window.findChild<QPushButton *>("loginButton");
    auto *cancel = window.findChild<QPushButton *>("cancelRestoreButton");
    // No event loop yet: the first frame must already be the transition.
    QCOMPARE(pages->currentWidget(), restore);
    window.show();
    QVERIFY(!login->isVisible());
    QVERIFY(cancel->isVisible());
    QVERIFY(!window.findChild<QPushButton *>("openSettings")->isEnabled());
    if (outcome == "cancel" && !qEnvironmentVariableIsEmpty("CLIENT_QT_SCREENSHOT_DIR")) {
      for (const QString &theme : {QString("light"), QString("dark")}) {
        QSettings().setValue("appearance/theme", theme);
        ClientTheme::apply();
        capture(window, "restore-session-" + theme);
      }
    }
    // Simulate presentation signals before the queued restore; never access
    // the OS credential store or the network in this UI fixture.
    if (outcome == "cancel") cancel->click();
    else if (outcome == "empty") emit service.loginStateChanged(false);
    else emit service.error("Synthetic restore failure", true);
    QCOMPARE(pages->currentIndex(), 0);
    QVERIFY(login->isVisible());
    QVERIFY(login->isEnabled());
    QVERIFY(restore->isHidden());
    QSignalSpy pending(&service, &AppService::loginStateChanged);
    QTest::qWait(30);
    QCOMPARE(pending.count(), 0); // Canceled queued restore cannot restart.
    QVERIFY(!service.loginPending());
  }
  void versionFeedbackOnlyUsesStatusBar() {
    QSettings().setValue("general/restoreLogin", false);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("version-status-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    auto *message = window.findChild<QLabel *>("loginMessage");
    QVERIFY(message);
    QVERIFY(window.findChild<QPushButton *>("refreshAssets")->isHidden());
    QVERIFY(window.findChild<QPushButton *>("sortAssets")->isHidden());
    QVERIFY(window.findChild<QPushButton *>("layoutToggle")->isHidden());
    emit service.status("Earlier login message");
    QVERIFY(!message->isVisible());
    emit service.status("正在检查站点版本…", true);
    QCOMPARE(window.statusBar()->currentMessage(), QString("正在检查站点版本…"));
    QVERIFY(!message->isVisible());
    bool modalShown = false;
    QTimer::singleShot(0, &window, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
          modalShown = true; dialog->accept();
        }
    });
    emit service.error("无法检查站点客户端版本：站点请求失败（HTTP 400）", true);
    QCoreApplication::processEvents();
    QVERIFY(!modalShown);
    QVERIFY(!message->isVisible());
    QVERIFY(window.statusBar()->currentMessage().contains("HTTP 400"));
    capture(window, "version-status-only");
    emit service.status("请在浏览器完成登录");
    QVERIFY(!message->isVisible());
    modalShown = false;
    QTimer::singleShot(0, &window, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QMessageBox *>(widget)) {
          modalShown = true;
          capture(*dialog, "login-error-dialog");
          dialog->accept();
        }
    });
    emit service.error("登录回调 state 无效，已拒绝该回调");
    QVERIFY(modalShown);
    QVERIFY(!message->isVisible());
    capture(window, "login-error-no-inline-message");
  }
  void repeatedErrorsUseOneDialog() {
    QSettings().setValue("general/restoreLogin", false);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("deduplicated-errors.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    int dialogs = 0;
    const QString failure = "Synthetic <b>failure</b>";
    QTimer closer;
    connect(&closer, &QTimer::timeout, &window, [&] {
      for (auto *widget : QApplication::topLevelWidgets()) {
        auto *dialog = qobject_cast<QMessageBox *>(widget);
        if (dialog && dialog->isVisible()) {
          ++dialogs;
          QCOMPARE(dialog->textFormat(), Qt::PlainText);
          if (dialogs == 1) {
            emit service.error(failure);
            emit launcher.failed(failure);
          }
          dialog->accept();
        }
      }
    });
    closer.start(10);
    emit service.error(failure);
    QCOMPARE(dialogs, 1);
    QCOMPARE(window.statusBar()->currentMessage(), failure);
  }
  void assetSelectionSurvivesLayoutAndMetadata() {
    QSettings().setValue("general/restoreLogin", false);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("selection-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    emit service.assetsReady(visualAssets(), 26);
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    auto *table = window.findChild<QTableWidget *>("assetTable");
    auto *toggle = window.findChild<QPushButton *>("layoutToggle");
    grid->item(0)->setSelected(true);
    grid->item(2)->setSelected(true);
    QMetaObject::invokeMethod(toggle, "clicked", Qt::DirectConnection);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
    emit service.metadataReady("favorites", QJsonArray{QJsonObject{{"asset", "0"}}});
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
    QMetaObject::invokeMethod(toggle, "clicked", Qt::DirectConnection);
    QCOMPARE(grid->selectedItems().size(), 2);
    QVERIFY(grid->item(0)->isSelected());
    QVERIFY(grid->item(2)->isSelected());
    emit service.loggedOut();
    QCOMPARE(grid->count(), 0);
    QVERIFY(!window.findChild<QPushButton *>("nextPage")->isEnabled());
    QVERIFY(window.findChild<QLabel *>("assetPageLabel")->text().contains("0"));
  }
  void tableFavoriteInteraction_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void tableFavoriteInteraction() {
    QFETCH(QString, theme);
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", "zh");
    QSettings().setValue("assets/layout", 1);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("favorites-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    // Exercise rejected requests without an actual login or modal error dialog.
    QObject::disconnect(&service, &AppService::error, &window, nullptr);
    emit service.assetsReady(visualAssets(), 6);
    window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
    auto *table = window.findChild<QTableWidget *>("assetTable");
    const auto star = [table](int row) {
      return table->cellWidget(row, 0)->findChild<QPushButton *>("assetFavorite");
    };
    window.show();
    table->selectRow(1);
    QTest::qWait(30);
    QVERIFY(!star(0)->property("favorite").toBool());
    QVERIFY(!star(0)->icon().isNull());
    QCOMPARE(star(0)->toolTip(), QString("收藏"));
    QSignalSpy finished(&service, &AppService::favoriteFinished);
    QSignalSpy errors(&service, &AppService::error);
    QTest::mouseClick(star(0), Qt::LeftButton);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().at(0).toString(), QString("0"));
    QCOMPARE(finished.first().at(1).toBool(), true);
    QCOMPARE(finished.first().at(2).toBool(), false);
    QTRY_VERIFY(star(0)->isEnabled());
    QVERIFY(!star(0)->property("favorite").toBool());
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(table->selectionModel()->selectedRows().first().row(), 1);
    // Successful server completion switches the icon, independent of selection.
    emit service.favoriteFinished("0", true, true);
    QTRY_VERIFY(star(0)->property("favorite").toBool());
    QCOMPARE(star(0)->accessibleName(), QString("取消收藏"));
    capture(window, "table-favorites-" + theme);
    star(0)->setFocus();
    QTest::keyClick(star(0), Qt::Key_Space);
    QCOMPARE(finished.last().at(1).toBool(), false);
    QTRY_VERIFY(star(0)->isEnabled());
    QVERIFY(star(0)->property("favorite").toBool()); // failure keeps the star
    QCOMPARE(errors.size(), 2); // exactly the two rejected favorite operations
    emit service.favoriteFinished("0", false, true);
    QTRY_VERIFY(!star(0)->property("favorite").toBool());
    // Double-clicking the favorite column's padding must never connect an asset.
    QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualItemRect(table->item(0, 0)).topLeft() + QPoint(2, 2));
    QCOMPARE(errors.size(), 2);
    auto *categories = window.findChild<QListWidget *>("categories");
    {
      const QSignalBlocker blocker(categories);
      categories->setCurrentRow(categories->count() - 1);
    }
    emit service.assetsReady(visualAssets(), 6);
    QVERIFY(star(0)->property("favorite").toBool());
    emit service.favoriteFinished("0", false, true);
    QTRY_COMPARE(table->rowCount(), 5);
    QCOMPARE(table->item(0, 1)->text(), visualAssets()[1].toObject()["name"].toString());
  }
  void connectionValidationClearsAndSmallWindowScrolls_data() {
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("fontSize");
    QTest::newRow("zh-light") << QString("zh") << QString("light") << 11;
    QTest::newRow("en-dark-large") << QString("en") << QString("dark") << 14;
  }
  void connectionValidationClearsAndSmallWindowScrolls() {
    QFETCH(QString, language);
    QFETCH(QString, theme);
    QFETCH(int, fontSize);
    QSettings().setValue("appearance/language", language);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/fontSize", fontSize);
    ClientTheme::apply();
    ConnectionDialog dialog(asset, details, methods, "validation-fixture", false, false, {}, nullptr);
    auto *account = dialog.findChild<QComboBox *>("account");
    auto *username = dialog.findChild<QLineEdit *>("username");
    auto *message = dialog.findChild<QLabel *>("connectionMessage");
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    auto *scroll = dialog.findChild<QScrollArea *>("connectionScroll");
    account->setCurrentIndex(1);
    dialog.setMaximumHeight(400); // Simulate a constrained work area.
    dialog.resize(480, 400);
    dialog.show();
    QTest::qWait(30);
    QCOMPARE(dialog.width(), 480);
    QVERIFY(dialog.height() <= 400);
    QVERIFY(scroll->verticalScrollBar()->isVisible());
    QVERIFY(buttons->visibleRegion().contains(buttons->rect()));
    buttons->button(QDialogButtonBox::Ok)->click();
    QVERIFY(!message->isHidden());
    username->setText("synthetic-user");
    QVERIFY(message->isHidden());
    username->clear();
    QVERIFY(!message->isHidden());
    account->setCurrentIndex(0);
    QVERIFY(message->isHidden());
    capture(dialog, "connection-compact-" + QString::fromLatin1(QTest::currentDataTag()));
  }
  void connectionHeightFollowsVisibleFields_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void connectionPlacement_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void connectionPlacement() {
    QFETCH(QString, theme);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", "zh");
    ClientTheme::apply();
    // Exercise each available display, including nonzero/negative origins when
    // the host has multiple monitors. No real session or user data is involved.
    for (auto *screen : QGuiApplication::screens()) {
      const QRect available = screen->availableGeometry();
      QWidget owner;
      owner.setWindowTitle("Connection placement fixture");
      owner.resize(qMin(740, available.width() - 32), qMin(640, available.height() - 32));
      owner.move(available.center() - QPoint(owner.width() / 2, owner.height() / 2));
      owner.show();
      QTest::qWait(30);
      ConnectionDialog dialog(asset, details, methods, "placement-fixture", true, false, {}, &owner);
      dialog.show();
      QTest::qWait(60);
      QCOMPARE(dialog.screen(), owner.screen());
      const QRect area = owner.screen()->availableGeometry().adjusted(16, 16, -16, -16);
      QVERIFY(area.contains(dialog.frameGeometry()));
      const QPoint center = owner.frameGeometry().center() - QPoint(0, 24);
      const QRect frame = dialog.frameGeometry();
      QRect centered = frame;
      centered.moveCenter(center);
      // When there is room, the decorated dialog must use the intended center.
      if (area.contains(centered))
        QVERIFY((frame.center() - center).manhattanLength() <= 2);
      qInfo() << "placement" << theme << "owner" << owner.frameGeometry()
              << "dialog" << frame << "available" << available;
      capture(dialog, "connection-position-" + theme);
      // A user-chosen position must survive subsequent form height changes.
      dialog.move(area.topLeft() + QPoint(20, 0));
      QTest::qWait(30);
      const QPoint moved = dialog.frameGeometry().topLeft();
      auto *account = dialog.findChild<QComboBox *>("account");
      account->setCurrentIndex(1);
      QTest::qWait(30);
      QCOMPARE(dialog.frameGeometry().topLeft(), moved);
      QVERIFY(area.contains(dialog.frameGeometry()));
      account->setCurrentIndex(0);
      QTest::qWait(30);
      QCOMPARE(dialog.frameGeometry().topLeft(), moved);
      // Growing near the bottom/right edge must keep the entire frame visible.
      dialog.move(area.bottomRight() - QPoint(dialog.frameGeometry().width() - 1,
                                             dialog.frameGeometry().height() - 1));
      account->setCurrentIndex(1);
      QTest::qWait(30);
      QVERIFY(area.contains(dialog.frameGeometry()));
      const QPoint clamped = dialog.frameGeometry().topLeft();
      account->setCurrentIndex(0);
      QTest::qWait(30);
      QCOMPARE(dialog.frameGeometry().topLeft(), clamped);
    }
  }
  void connectionHeightFollowsVisibleFields() {
    QFETCH(QString, theme);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", "zh");
    ClientTheme::apply();
    ConnectionDialog dialog(asset, details, methods, "adaptive-fixture", true, false, {}, nullptr);
    dialog.show();
    auto *account = dialog.findChild<QComboBox *>("account");
    auto *scroll = dialog.findChild<QScrollArea *>("connectionScroll");
    auto *footer = dialog.findChild<QWidget *>("dialogFooter");
    QTest::qWait(50);
    const int managedHeight = dialog.height();
    const int heightLimit = qMax(dialog.minimumHeight(), dialog.screen()->availableGeometry().height() -
                                (dialog.frameGeometry().height() - dialog.height()) - 32);
    if (managedHeight < heightLimit)
      QVERIFY(!scroll->verticalScrollBar()->isVisible());
    capture(dialog, "adaptive-managed-" + theme);
    account->setCurrentIndex(1);
    QTest::qWait(30);
    QVERIFY(dialog.height() > managedHeight || dialog.height() == heightLimit);
    const int manualHeight = dialog.height();
    QVERIFY(footer->visibleRegion().contains(footer->rect()));
    capture(dialog, "adaptive-manual-" + theme);
    account->setCurrentIndex(2);
    QTest::qWait(30);
    QVERIFY(dialog.height() < manualHeight || dialog.height() == heightLimit);
    QVERIFY(dialog.height() > managedHeight || dialog.height() == heightLimit);
    account->setCurrentIndex(0);
    QTRY_COMPARE(dialog.height(), managedHeight);
    if (managedHeight < heightLimit)
      QTRY_VERIFY(!scroll->verticalScrollBar()->isVisible());
    // No protocol removes the method tabs and adds validation text. The dialog
    // should fit the new content rather than retain the previous tab height.
    dialog.findChild<QComboBox *>("protocol")->setCurrentIndex(-1);
    QTest::qWait(30);
    QVERIFY(dialog.findChild<QLabel *>("connectionMessage")->isVisible());
    QVERIFY(dialog.findChild<QWidget *>("connectionMethodField")->isHidden());
    QVERIFY(dialog.height() < managedHeight || dialog.height() == heightLimit);
    QVERIFY(footer->visibleRegion().contains(footer->rect()));
  }
  void themedScrollbars_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void themedScrollbars() {
    QFETCH(QString, theme);
    QSettings().setValue("appearance/theme", theme);
    ClientTheme::apply();
    QScrollArea area;
    auto *content = new QLabel("Scrollable content");
    content->setAlignment(Qt::AlignCenter);
    content->setFixedSize(1200, 900);
    area.setWidget(content);
    area.resize(320, 220);
    area.show();
    QTest::qWait(30);
    for (auto *bar : {area.verticalScrollBar(), area.horizontalScrollBar()}) {
      QVERIFY(bar->isVisible());
      const QPoint start = bar->orientation() == Qt::Vertical ? QPoint(bar->width() / 2, 12)
                                                              : QPoint(12, bar->height() / 2);
      const QPoint end = start + (bar->orientation() == Qt::Vertical ? QPoint(0, 80) : QPoint(80, 0));
      QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, start);
      // Qt 5's QWidget mouseMove helper does not retain the pressed button.
      QMouseEvent move(QEvent::MouseMove, end, bar->mapToGlobal(end), Qt::NoButton,
                       Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(bar, &move);
      QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, end);
      QVERIFY(bar->value() > 0);
    }
    capture(area, "scrollbars-" + theme);
  }
  void connectionProtocolOrdering() {
    const QJsonArray unordered{QJsonObject{{"name", "telnet"}}, QJsonObject{{"name", "sftp"}},
      QJsonObject{{"name", "rdp"}}, QJsonObject{{"name", "vnc"}}, QJsonObject{{"name", "ssh"}}};
    auto permission = details;
    permission["permed_protocols"] = unordered;
    for (const QString &type : {QString("linux"), QString("windows")}) {
      auto item = asset;
      item["type"] = QJsonObject{{"value", type}};
      ConnectionDialog dialog(item, permission, methods, "ordering-" + type, false, false, {}, nullptr);
      auto *combo = dialog.findChild<QComboBox *>("protocol");
      QStringList actual;
      for (int i = 0; i < combo->count(); ++i) actual.append(combo->itemData(i).toString());
      QCOMPARE(actual, type == "windows" ? QStringList({"rdp", "vnc", "ssh", "sftp", "telnet"})
                                          : QStringList({"ssh", "sftp", "vnc", "telnet", "rdp"}));
      QCOMPARE(combo->currentData().toString(), actual.first());
    }
    auto network = asset;
    network["type"] = QJsonObject{{"value", "switch"}};
    network["category"] = QJsonObject{{"value", "device"}};
    ConnectionDialog dialog(network, permission, methods, "ordering-device", false, false, "vnc", nullptr);
    auto *combo = dialog.findChild<QComboBox *>("protocol");
    QCOMPARE(combo->itemData(0).toString(), QString("ssh"));
    QCOMPARE(combo->currentData().toString(), QString("vnc"));
  }
  void connectionMethodSelectorAlwaysShowsAvailableCategory() {
    ConnectionDialog single(asset, details, methods, "method-fixture", false, false, {}, nullptr);
    QVERIFY(!single.findChild<QWidget *>("connectionMethodField")->isHidden());
    QCOMPARE(single.requestBody()["connect_method"].toString(), QString("ssh_client"));
    auto choices = methods;
    auto ssh = choices["ssh"].toArray();
    ssh.append(QJsonObject{{"value", "alternative"}, {"label", "Alternative client"}, {"type", "native"}});
    choices["ssh"] = ssh;
    ConnectionDialog multiple(asset, details, choices, "method-fixture", false, false, {}, nullptr);
    QVERIFY(!multiple.findChild<QWidget *>("connectionMethodField")->isHidden());
    chooseMethod(multiple, "alternative");
    QCOMPARE(multiple.requestBody()["connect_method"].toString(), QString("alternative"));
  }
  void nativeMethodTabsAcrossProtocols_data() {
    QTest::addColumn<QString>("protocol");
    for (const char *protocol : {"ssh", "rdp", "vnc", "sftp", "mysql", "db2"})
      QTest::newRow(protocol) << QString::fromLatin1(protocol);
  }
  void nativeMethodTabsAcrossProtocols() {
    QFETCH(QString, protocol);
    auto permission = details;
    permission["permed_protocols"] = QJsonArray{QJsonObject{{"name", protocol}}};
    const QJsonObject available{{protocol, QJsonArray{
      QJsonObject{{"value", "fixture_client"}, {"label", "本地客户端"}, {"type", "native"},
                  {"endpoint_protocol", protocol == "db2" ? "ssh" : protocol}}}}};
    ConnectionDialog dialog(asset, permission, available, "native-tabs", false, false, {}, nullptr);
    dialog.show();
    QTest::qWait(30);
    auto *tabs = dialog.findChild<QTabWidget *>("connectionMethodTabs");
    QVERIFY(tabs->isVisible());
    QVERIFY(tabs->tabBar()->isVisible());
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentWidget()->property("methodType").toString(), QString("native"));
    QVERIFY(tabs->currentWidget()->findChild<QComboBox *>("method")->isVisible());
    QCOMPARE(dialog.requestBody()["protocol"].toString(), protocol);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("fixture_client"));
    if (protocol == "ssh") {
      for (const QString &theme : {QString("light"), QString("dark")}) {
        QSettings().setValue("appearance/theme", theme);
        ClientTheme::apply();
        QTest::qWait(30);
        capture(dialog, "native-method-tab-" + theme);
      }
    }
  }
  void settingsPlacement_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void settingsPlacement() {
    QFETCH(QString, theme);
    QSettings().setValue("appearance/theme", theme);
    ClientTheme::apply();
    ConfigStore config(nullptr, directory.filePath("settings-placement.json"));
    QWidget owner;
    const QRect available = owner.screen()->availableGeometry();
    owner.resize(700, 600);
    owner.move(available.center() - QPoint(owner.width() / 2, owner.height() / 2));
    owner.show();
    SettingsDialog dialog(&config, {}, &owner);
    dialog.show();
    QTest::qWait(60);
    QCOMPARE(dialog.screen(), owner.screen());
    const QPoint center = owner.frameGeometry().center() - QPoint(0, 24);
    QRect expected = dialog.frameGeometry();
    expected.moveCenter(center);
    if (expected.left() >= available.left() + 16 && expected.right() <= available.right() - 16)
      QVERIFY(qAbs(dialog.frameGeometry().center().x() - center.x()) <= 2);
    if (expected.top() >= available.top() + 16 && expected.bottom() <= available.bottom() - 16)
      QVERIFY(qAbs(dialog.frameGeometry().center().y() - center.y()) <= 2);
    QVERIFY(dialog.frameGeometry().left() >= available.left() + 16);
    QVERIFY(dialog.frameGeometry().top() >= available.top() + 16);
    qInfo() << "settings placement" << theme << "owner" << owner.frameGeometry()
            << "dialog" << dialog.frameGeometry() << "available" << available;
    capture(dialog, "settings-position-" + theme);
    dialog.move(available.topLeft() + QPoint(32, 32));
    QTest::qWait(30);
    const QPoint moved = dialog.frameGeometry().topLeft();
    dialog.findChild<QListWidget *>("settingsNavigation")->setCurrentRow(2);
    QTest::qWait(30);
    QCOMPARE(dialog.frameGeometry().topLeft(), moved);
    dialog.hide();
    dialog.show();
    QTest::qWait(30);
    QCOMPARE(dialog.frameGeometry().topLeft(), moved);
  }
  void localApplicationPathsRemainConfigurable() {
    ConfigStore config(nullptr, directory.filePath("application-paths.json"));
    SettingsDialog dialog(&config, {}, nullptr);
    QVERIFY(dialog.findChild<QScrollArea *>("applicationProtocols"));
    QVERIFY(dialog.findChild<QScrollArea *>("applicationCardScroll"));
    // Oracle is hidden from local application settings on every platform.
    QVERIFY(!applicationProtocolButton(&dialog, "oracle"));
    auto *ssh = applicationProtocolButton(&dialog, "ssh");
    QVERIFY(ssh);
    QVERIFY(ssh->isChecked());
    QStringList terminalNames;
    for (auto *card : applicationCards(&dialog))
      terminalNames << card->findChild<QLabel *>("applicationName")->text();
    QCOMPARE(terminalNames, supportedApplicationNames(&config, "terminal", "ssh"));
    for (auto *card : applicationCards(&dialog)) {
      QVERIFY(card->findChild<QPushButton *>("applicationBrowse"));
      QVERIFY(card->findChild<QLabel *>("applicationComment"));
      QStringList badges;
      for (auto *label : card->findChildren<QLabel *>())
        if (label->property("role").toString() == "appBadge")
          badges << label->text();
      QVERIFY(!badges.isEmpty());
      QVERIFY(!badges.contains("ORACLE"));
    }
#ifdef Q_OS_MACOS
    // macOS internal launchers are started by name, so they expose no path.
    auto *managed = applicationCards(&dialog).first();
    QCOMPARE(managed->findChild<QLabel *>("applicationPath")->text(),
             ClientTheme::text("无需配置", "Managed automatically"));
    QVERIFY(!managed->findChild<QPushButton *>("applicationBrowse")->isEnabled());
#endif
    QStringList vncNames;
    applicationProtocolButton(&dialog, "vnc")->click();
    for (auto *card : applicationCards(&dialog))
      vncNames << card->findChild<QLabel *>("applicationName")->text();
    QCOMPARE(vncNames, supportedApplicationNames(&config, "remotedesktop", "vnc"));
    QVERIFY(!vncNames.isEmpty());
    auto *vncCard = applicationCards(&dialog).first();
    QVERIFY(vncCard->findChild<QPushButton *>("applicationBrowse")->isEnabled());
#ifdef Q_OS_MACOS
    // TigerVNC is external, so its path stays configurable and visible.
    QCOMPARE(vncCard->findChild<QLabel *>("applicationPath")->text(),
             QString("/Applications/TigerVNC.app"));
#endif
    applicationProtocolButton(&dialog, "ssh")->click();
    QCOMPARE(applicationCards(&dialog).size(), terminalNames.size());
  }
  void generalProxySettingsPersistWhenChanged() {
    QSettings().remove("proxy/mode"); QSettings().remove("proxy/http");
    QSettings().remove("proxy/username"); QSettings().remove("proxy-password");
    ConfigStore config(nullptr, directory.filePath("application-default.json"));
    SettingsDialog dialog(&config, {}, nullptr);
    auto *mode = dialog.findChild<QComboBox *>("proxy/mode");
    auto *url = dialog.findChild<QLineEdit *>("proxyHttp");
    QVERIFY(mode); QVERIFY(url);
    QCOMPARE(mode->currentData().toString(), QString("system"));
    QVERIFY(!url->isEnabled());
    mode->setCurrentIndex(mode->findData("manual"));
    QVERIFY(url->isEnabled());
    QSignalSpy changed(&dialog, &SettingsDialog::preferencesChanged);
    url->setText("http://proxy.example.com:3128");
    QTest::keyClick(url, Qt::Key_Return);
    QVERIFY(changed.size() >= 1);
    QCOMPARE(QSettings().value("proxy/mode").toString(), QString("manual"));
    QCOMPARE(QSettings().value("proxy/http").toString(), QString("http://proxy.example.com:3128"));
    QVERIFY(!dialog.findChild<QLineEdit *>("proxyUsername"));
    QVERIFY(!dialog.findChild<QLineEdit *>("proxyPassword"));
    // Switching back to "off" disables the manual fields again.
    mode->setCurrentIndex(mode->findData("none"));
    QVERIFY(!url->isEnabled());
    QSettings().remove("proxy/mode"); QSettings().remove("proxy/http");
    QSettings().remove("proxy/username"); QSettings().remove("proxy-password");
  }
  void localApplicationSwitchControlsDefault() {
    ConfigStore config(nullptr, directory.filePath("application-default.json"));
    SettingsDialog dialog(&config, {}, nullptr);
    QVERIFY(applicationProtocolButton(&dialog, "mysql"));
    applicationProtocolButton(&dialog, "mysql")->click();
    QAbstractButton *current = nullptr, *spare = nullptr;
    QString spareName;
    for (auto *card : applicationCards(&dialog)) {
      auto *toggle = card->findChild<QAbstractButton *>("applicationDefault");
      QVERIFY(toggle);
      if (toggle->isChecked())
        current = toggle;
      else if (!spare && toggle->isEnabled()) {
        spare = toggle;
        spareName = card->findChild<QLabel *>("applicationName")->text();
      }
    }
    QVERIFY(current);
    QVERIFY(spare);
    QCOMPARE(defaultApplicationCount(&config, "databases", "mysql"), 1);
    QSignalSpy changes(&config, &ConfigStore::changed);
    current->click();
    QCOMPARE(changes.count(), 1);
    QCOMPARE(defaultApplicationCount(&config, "databases", "mysql"), 0);
    // The page states that the protocol lost its only default client.
    QVERIFY(!applicationNotice(&dialog)->text().isEmpty());
    QVERIFY(!applicationNotice(&dialog)->isHidden());
    QAbstractButton *replacement = nullptr;
    for (auto *card : applicationCards(&dialog))
      if (card->findChild<QLabel *>("applicationName")->text() == spareName)
        replacement = card->findChild<QAbstractButton *>("applicationDefault");
    QVERIFY(replacement);
    replacement->click();
    QCOMPARE(defaultApplicationCount(&config, "databases", "mysql"), 1);
    QVERIFY(replacement->isChecked());
    int checked = 0;
    for (auto *card : applicationCards(&dialog))
      if (card->findChild<QAbstractButton *>("applicationDefault")->isChecked())
        ++checked;
    QCOMPARE(checked, 1);
    QVERIFY(applicationNotice(&dialog)->text().isEmpty());
  }
  void keyboardLayoutSelection() {
    ConfigStore config(nullptr, directory.filePath("keyboard-layout.json"));
    SettingsDialog dialog(&config, {}, nullptr);
    auto *keyboard = dialog.findChild<QComboBox *>("connection/keyboardLayout");
    QVERIFY(keyboard);
    QVERIFY(!keyboard->isEditable());
    QCOMPARE(keyboard->count(), 21);
    QCOMPARE(keyboard->currentData().toString(), QString("en-us-qwerty"));
    QVERIFY(!QSettings().contains("connection/keyboardLayout"));
    QSignalSpy changes(&dialog, &SettingsDialog::preferencesChanged);
    keyboard->setCurrentIndex(keyboard->findData("de-de-qwertz"));
    QCOMPARE(changes.count(), 1);
    QCOMPARE(QSettings().value("connection/keyboardLayout").toString(), QString("de-de-qwertz"));
    SettingsDialog reopened(&config, {}, nullptr);
    QCOMPARE(reopened.findChild<QComboBox *>("connection/keyboardLayout")->currentData().toString(),
             QString("de-de-qwertz"));
    ConnectionDialog connection(asset, details, methods, "keyboard-fixture", false, false, {}, nullptr);
    auto *protocol = connection.findChild<QComboBox *>("protocol");
    protocol->setCurrentIndex(protocol->findData("rdp"));
    QCOMPARE(connection.requestBody()["connect_options"].toObject()["keyboard_layout"].toString(),
             QString("de-de-qwertz"));
    QSettings().setValue("connection/keyboardLayout", "legacy-layout");
    SettingsDialog legacy(&config, {}, nullptr);
    QCOMPARE(legacy.findChild<QComboBox *>("connection/keyboardLayout")->currentData().toString(),
             QString("legacy-layout"));
    QCOMPARE(QSettings().value("connection/keyboardLayout").toString(), QString("legacy-layout"));
    QSettings().setValue("connection/keyboardLayout", "");
    SettingsDialog unspecified(&config, {}, nullptr);
    QCOMPARE(unspecified.findChild<QComboBox *>("connection/keyboardLayout")->currentData().toString(), QString());
    if (!qEnvironmentVariableIsEmpty("CLIENT_QT_SCREENSHOT_DIR")) {
      dialog.findChild<QListWidget *>("settingsNavigation")->setCurrentRow(2);
      dialog.show();
      for (const QString &theme : {QString("light"), QString("dark")}) {
        QSettings().setValue("appearance/theme", theme);
        ClientTheme::apply();
        QTest::qWait(50);
        capture(dialog, "keyboard-layout-" + theme);
        keyboard->showPopup();
        QTest::qWait(50);
        capture(*keyboard->view()->window(), "keyboard-options-" + theme);
        keyboard->hidePopup();
      }
    }
  }
  void searchFieldInteractionAndScreenshots_data() {
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QString>("language");
    QTest::addColumn<int>("fontSize");
    QTest::newRow("dark") << QString("dark") << QString("zh") << 11;
    QTest::newRow("light") << QString("light") << QString("zh") << 11;
    QTest::newRow("large-font") << QString("dark") << QString("en") << 14;
  }
  void comboPopupInteractionAndScreenshots_data() {
    QTest::addColumn<QString>("theme");
    QTest::addColumn<int>("fontSize");
    QTest::addColumn<bool>("editable");
    QTest::newRow("light") << QString("light") << 11 << false;
    QTest::newRow("dark") << QString("dark") << 11 << false;
    QTest::newRow("large-font") << QString("dark") << 14 << false;
    QTest::newRow("site-editable") << QString("light") << 11 << true;
  }
  void comboPopupInteractionAndScreenshots() {
    QFETCH(QString, theme);
    QFETCH(int, fontSize);
    QFETCH(bool, editable);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/fontSize", fontSize);
    ClientTheme::apply();
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *combo = new ClientTheme::ComboBox(&window);
    combo->setEditable(editable);
    for (int i = 0; i < 14; ++i)
      combo->addItem(editable ? QString("https://workspace-%1.example.com:8443").arg(i)
                              : QString("授权组织 %1 · Operations team").arg(i), QString("id-%1").arg(i));
    combo->setCurrentIndex(1);
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    QVERIFY(model);
    model->item(2)->setEnabled(false);
    layout->addWidget(combo);
    window.resize(280, 100);
    window.show();
    QSignalSpy changed(combo, QOverload<int>::of(&QComboBox::currentIndexChanged));
    combo->showPopup();
    QTest::qWait(40);
    QVERIFY(combo->view()->isVisible());
    QVERIFY(combo->view()->width() >= combo->width());
    QVERIFY(!combo->view()->horizontalScrollBar()->isVisible());
    QVERIFY(combo->view()->verticalScrollBar()->isVisible());
    capture(*combo->view()->window(), "dropdown-" + QString::fromLatin1(QTest::currentDataTag()));
    QTest::keyClick(combo->view(), Qt::Key_Down);
    QCOMPARE(combo->view()->currentIndex().row(), 3); // Disabled options are skipped.
    QTest::keyClick(combo->view(), Qt::Key_Return);
    QCOMPARE(combo->currentData().toString(), QString("id-3"));
    QCOMPARE(changed.count(), 1);
    combo->showPopup();
    QTest::keyClick(combo->view(), Qt::Key_Down);
    QTest::keyClick(combo->view(), Qt::Key_Escape);
    QCOMPARE(combo->currentData().toString(), QString("id-3"));
    QCOMPARE(changed.count(), 1);
    if (editable) {
      combo->lineEdit()->setText("https://new.example.com");
      QCOMPARE(combo->currentText(), QString("https://new.example.com"));
    }
  }
  void searchFieldInteractionAndScreenshots() {
    QFETCH(QString, theme);
    QFETCH(QString, language);
    QFETCH(int, fontSize);
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", language);
    QSettings().setValue("appearance/fontSize", fontSize);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("search-ui-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    window.activateWindow();
    auto *search = window.findChild<QLineEdit *>("assetSearch");
    auto *collapse = window.findChild<QPushButton *>("sidebarToggle");
    auto *sidebar = window.findChild<QWidget *>("sidebar");
    search->setEnabled(true); // Isolated presentation fixture; no server session.
    connect(collapse, &QPushButton::clicked, &window, [search] {
      // Keep the authenticated presentation state after retranslation.
      search->setEnabled(true);
    });
    collapse->setFocus();
    QTest::qWait(50);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    capture(*sidebar, "search-idle-" + tag);
    QTest::mouseClick(search, Qt::LeftButton);
    QTest::keyClicks(search, "192.0.2.10");
    QCOMPARE(search->text(), QString("192.0.2.10"));
    QTest::qWait(30);
    capture(*sidebar, "search-focused-" + tag);
    QToolButton *clear = nullptr;
    for (auto *button : search->findChildren<QToolButton *>())
      if (button->isVisible()) clear = button;
    QVERIFY(clear);
    QVERIFY(search->rect().contains(clear->geometry()));
    QTest::mouseClick(clear, Qt::LeftButton);
    QVERIFY(search->text().isEmpty());
    search->setText("replace me");
    collapse->click();
    QVERIFY(search->isHidden());
    QTest::keySequence(&window, QKeySequence(QKeySequence::Find));
    QTRY_VERIFY(!search->isHidden());
    QTRY_VERIFY(search->hasFocus());
    QCOMPARE(search->selectedText(), QString("replace me"));
  }
  void oauthPendingKeepsCancelAndRejectsRepeatedLogin() {
    QSettings().setValue("general/restoreLogin", false);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
          const QByteArray header =
              socket->property("header").toByteArray() + socket->readAll();
          socket->setProperty("header", header);
          if (!header.contains("\r\n\r\n"))
            return;
          ++requests;
          const QByteArray body = header.contains("/versions/")
                                      ? "[\"4.1.2\"]"
                                      : "{\"client_id\":\"ui-test\"}";
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: "
              "application/json\r\nConnection: close\r\nContent-Length: " +
              QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
    AppService service;
    ConfigStore config(nullptr, directory.filePath("oauth-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.show();
    auto *site = window.findChild<QComboBox *>("loginSite");
    auto *login = window.findChild<QPushButton *>("loginButton");
    auto *cancel = window.findChild<QPushButton *>("cancelLoginButton");
    site->setCurrentText(
        QString("http://127.0.0.1:%1").arg(server.serverPort()));
    QSignalSpy authorization(&service, &AppService::authorizationUrlReady);
    QSignalSpy pending(&service, &AppService::loginStateChanged);
    QTest::mouseClick(login, Qt::LeftButton);
    QVERIFY(service.loginPending());
    QVERIFY(!login->isEnabled());
    QVERIFY(cancel->isVisible());
    QVERIFY(window.findChild<QLabel *>("loginHint")->isHidden());
    QTRY_COMPARE(authorization.count(), 1);
    QVERIFY(cancel->isVisible());
    QVERIFY(!login->isEnabled());
    const int priorRequests = requests;
    const int priorStates = pending.count();
    QMetaObject::invokeMethod(login, "clicked", Qt::DirectConnection);
    QCOMPARE(pending.count(), priorStates);
    QCOMPARE(requests, priorRequests);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QVERIFY(!service.loginPending());
    QVERIFY(login->isEnabled());
    QVERIFY(cancel->isHidden());
    QVERIFY(site->isEnabled());
    QVERIFY(!window.findChild<QLabel *>("loginHint")->isHidden());
  }
  void filtersMethodsAndForbiddenProtocol() {
    ConnectionDialog dialog(asset, details, methods, "site\nuser\norg", false,
                            false, {}, nullptr);
    auto *protocol = dialog.findChild<QComboBox *>("protocol");
    auto *tabs = dialog.findChild<QTabWidget *>("connectionMethodTabs");
    QCOMPARE(protocol->count(), 3);
    QCOMPARE(protocol->findData("winrm"), -1);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(),
             QString("ssh_client"));
    protocol->setCurrentIndex(protocol->findData("http"));
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(),
             QString("chrome"));
    QCOMPARE(dialog.requestBody()["connect_options"]
                 .toObject()["appletConnectMethod"]
                 .toString(),
             QString("client"));
  }
  void httpRemoteApplicationMethods_data() {
    QTest::addColumn<QJsonArray>("catalogue");
    QTest::addColumn<QString>("expectedMethod");
    QTest::newRow("empty-catalogue") << QJsonArray{} << QString();
    QTest::newRow("custom-applet")
        << QJsonArray{QJsonObject{{"value", "edge"}, {"type", "applet"}, {"listen", "rdp"}}}
        << QString("edge");
    QTest::newRow("applet-name-ending-in-guide")
        << QJsonArray{QJsonObject{{"value", "browser_guide"}, {"type", "applet"}}}
        << QString("browser_guide");
    QTest::newRow("disabled-applet")
        << QJsonArray{QJsonObject{{"value", "chrome"}, {"type", "applet"}, {"disabled", true}}}
        << QString();
    QTest::newRow("web-only")
        << QJsonArray{QJsonObject{{"value", "web_gui"}, {"type", "web"}}}
        << QString();
    QTest::newRow("virtual-app-default-is-server-resolved")
        << QJsonArray{QJsonObject{{"value", "chrome"}, {"type", "virtual_app"}}}
        << QString("chrome");
    QTest::newRow("virtual-app-with-custom-name")
        << QJsonArray{QJsonObject{{"value", "edge_remote"}, {"label", "Edge"}, {"type", "virtual_app"}}}
        << QString("edge_remote");
    QTest::newRow("web-and-disabled-default")
        << QJsonArray{QJsonObject{{"value", "web_gui"}, {"type", "web"}},
                      QJsonObject{{"value", "chrome"}, {"type", "applet"}, {"disabled", true}}}
        << QString();
  }
  void httpRemoteApplicationMethods() {
    QFETCH(QJsonArray, catalogue);
    QFETCH(QString, expectedMethod);
    auto available = methods;
    available["http"] = catalogue;
    ConnectionDialog dialog(asset, details, available, "http-fixture", false, false, "http", nullptr);
    const auto body = dialog.requestBody();
    QVERIFY(!dialog.findChild<QWidget *>("connectionMethodField")->isHidden());
    QCOMPARE(body["protocol"].toString(), QString("http"));
    QCOMPARE(body["connect_method"].toString(), expectedMethod);
    const auto *page = dialog.findChild<QTabWidget *>("connectionMethodTabs")->currentWidget();
    const bool virtualApp = page && page->property("methodType").toString() == "virtual_app";
    QCOMPARE(body["connect_options"].toObject()["appletConnectMethod"].toString(), QString(virtualApp ? "web" : "client"));
    QCOMPARE(body["connect_options"].toObject()["virtualappConnectMethod"].toString(), QString(virtualApp ? "client" : "web"));
    QCOMPARE(body["connect_options"].toObject()["reusable"].toBool(true), false);
    QCOMPARE(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled(),
             !expectedMethod.isEmpty());
    for (bool editOnly : {false, true}) {
      const QString scope = QString("http-submit-%1-%2").arg(QTest::currentDataTag()).arg(editOnly);
      ConnectionDialog submit(asset, details, available, scope, false, editOnly, "http", nullptr);
      submit.show();
      QTest::qWait(20);
      QTest::mouseClick(submit.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
      if (expectedMethod.isEmpty()) {
        QCOMPARE(submit.result(), int(QDialog::Rejected));
        QVERIFY(ConnectionDialog::saved(scope, "asset").isEmpty());
      } else {
        QCOMPARE(submit.result(), int(QDialog::Accepted));
        const auto saved = ConnectionDialog::saved(scope, "asset");
        QCOMPARE(saved["protocol"].toString(), QString("http"));
        QCOMPARE(saved["connectMethod"].toString(), expectedMethod);
        ConnectionDialog reopened(asset, details, available, scope, false, false, {}, nullptr);
        QCOMPARE(reopened.requestBody()["connect_method"].toString(), expectedMethod);
        QVERIFY(reopened.tryDirectConnection(false));
      }
    }
    if (catalogue.isEmpty()) {
      available.remove("http");
      ConnectionDialog missing(asset, details, available, "http-missing", false, false, "http", nullptr);
      QVERIFY(missing.requestBody()["connect_method"].toString().isEmpty());
    }
  }
  void httpApplicationChoicePersists() {
    QSettings().setValue("appearance/theme", "dark");
    QSettings().setValue("appearance/language", "zh");
    ClientTheme::apply();
    auto available = methods;
    available["http"] = QJsonArray{
        QJsonObject{{"value", "chrome"}, {"label", "Chrome"}, {"type", "applet"}},
        QJsonObject{{"value", "edge_remote"}, {"label", "Edge"}, {"type", "virtual_app"}},
        QJsonObject{{"value", "firefox"}, {"label", "Firefox"}, {"type", "applet"}}};
    ConnectionDialog dialog(asset, details, available, "http-choice", false, true, "http", nullptr);
    dialog.show();
    auto *tabs = dialog.findChild<QTabWidget *>("connectionMethodTabs");
    QCOMPARE(tabs->count(), 2);
    QTest::qWait(30);
    QVERIFY(tabs->isVisible());
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(1).center());
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("edge_remote"));
    QTest::qWait(30);
    capture(dialog, "http-application-choice");
    QTest::mouseClick(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    ConnectionDialog reopened(asset, details, available, "http-choice", false, false, {}, nullptr);
    QCOMPARE(reopened.requestBody()["connect_method"].toString(), QString("edge_remote"));
    QCOMPARE(reopened.requestBody()["protocol"].toString(), QString("http"));
    QCOMPARE(reopened.requestBody()["connect_options"].toObject()["virtualappConnectMethod"].toString(), QString("client"));
    QVERIFY(reopened.tryDirectConnection(false));
  }
  void httpClientRequestMatchesReference() {
    QSettings().setValue("connection/rdpClientOptions", QStringList{"full_screen"});
    const QJsonObject webAsset{{"id", "synthetic-web-asset"}, {"name", "Test website"}};
    const QJsonObject webDetails{
        {"permed_protocols", QJsonArray{QJsonObject{{"name", "http"}}}},
        {"permed_accounts", QJsonArray{QJsonObject{{"id", "synthetic-account"}, {"name", "admin"}, {"alias", "admin"}}}}};
    const QJsonObject remoteMethods{{"http", QJsonArray{
        QJsonObject{{"value", "chrome_app"}, {"label", "Chrome"}, {"type", "virtual_app"}},
        QJsonObject{{"value", "chrome"}, {"label", "Chrome Applet"}, {"type", "applet"}}}}};
    ConnectionDialog dialog(webAsset, webDetails, remoteMethods, "reference-request", false, false, "http", nullptr);
    chooseMethod(dialog, "chrome_app");
    // User-supplied successful Web client request; only the fixture IDs differ.
    const auto expected = QJsonDocument::fromJson(R"({
      "asset":"synthetic-web-asset", "account":"synthetic-account", "protocol":"http",
      "input_username":"admin", "input_secret":"", "connect_method":"chrome_app",
      "connect_options":{"charset":"default","disableautohash":false,"token_reusable":false,
        "resolution":"auto","backspaceAsCtrlH":false,"appletConnectMethod":"web",
        "virtualappConnectMethod":"client","reusable":false,"rdp_connection_speed":"auto"}
    })").object();
    QCOMPARE(dialog.requestBody(), expected);
    const QJsonObject expectedQuery{{"full_screen", "1"}, {"rdp_smart_size", "0"}, {"rdp_color_quality", "32"}};
    QCOMPARE(dialog.extraParameters(), expectedQuery);
    chooseMethod(dialog, "chrome");
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("chrome"));
    const auto appletOptions = dialog.requestBody()["connect_options"].toObject();
    QCOMPARE(appletOptions["appletConnectMethod"].toString(), QString("client"));
    QCOMPARE(appletOptions["virtualappConnectMethod"].toString(), QString("web"));
    chooseMethod(dialog, "chrome_app");
    QCOMPARE(dialog.requestBody(), expected);
  }
  void methodTabsFollowLunaGrouping_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void methodTabsFollowLunaGrouping() {
    QFETCH(QString, theme);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", "zh");
    ClientTheme::apply();
    auto available = methods;
    auto ssh = available["ssh"].toArray();
    ssh.append(QJsonObject{{"value", "remote_terminal"}, {"label", "远程终端"}, {"type", "applet"}});
    ssh.append(QJsonObject{{"value", "remote_terminal_2"}, {"label", "备用终端"}, {"type", "applet"}});
    ssh.append(QJsonObject{{"value", "virtual_terminal"}, {"label", "虚拟终端"}, {"type", "virtual_app"}});
    available["ssh"] = ssh;
    ConnectionDialog dialog(asset, details, available, "tab-groups", false, true, "ssh", nullptr);
    dialog.show();
    auto *tabs = dialog.findChild<QTabWidget *>("connectionMethodTabs");
    QCOMPARE(tabs->count(), 3);
    QCOMPARE(tabs->tabText(0), QString("本地客户端"));
    QCOMPARE(tabs->tabText(1), QString("远程应用"));
    QCOMPARE(tabs->tabText(2), QString("虚拟应用"));
    tabs->setCurrentIndex(1);
    auto *choices = tabs->currentWidget()->findChild<QComboBox *>("method");
    choices->setFocus();
    QTest::keyClick(choices, Qt::Key_Down);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("remote_terminal_2"));
    QCOMPARE(dialog.requestBody()["connect_options"].toObject()["appletConnectMethod"].toString(), QString("client"));
    QTest::qWait(30);
    capture(dialog, "method-tabs-remote-" + theme);
    tabs->setCurrentIndex(2);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("virtual_terminal"));
    QCOMPARE(dialog.requestBody()["connect_options"].toObject()["virtualappConnectMethod"].toString(), QString("client"));
    tabs->setCurrentIndex(1);
    QCOMPARE(dialog.requestBody()["connect_method"].toString(), QString("remote_terminal_2"));
    QTest::mouseClick(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton);
    ConnectionDialog reopened(asset, details, available, "tab-groups", false, false, {}, nullptr);
    QCOMPARE(reopened.findChild<QTabWidget *>("connectionMethodTabs")->currentIndex(), 1);
    QCOMPARE(reopened.requestBody()["connect_method"].toString(), QString("remote_terminal_2"));
    reopened.findChild<QComboBox *>("protocol")->setCurrentIndex(1);
    QCOMPARE(reopened.requestBody()["connect_method"].toString(), QString("mstsc"));
    QVERIFY(!reopened.requestBody()["connect_options"].toObject().contains("appletConnectMethod"));
  }
  void manualAndDynamicCredentials() {
    ConnectionDialog dialog(asset, details, methods, "site\nuser\norg", false,
                            false, {}, nullptr);
    auto *account = dialog.findChild<QComboBox *>("account");
    auto *username = dialog.findChild<QLineEdit *>("username");
    auto *secret = dialog.findChild<QLineEdit *>("secret");
    auto *remember = dialog.findChild<QCheckBox *>("rememberSecret");
    account->setCurrentIndex(1);
    username->setText("operator");
    secret->setText("test-only-secret");
    QCOMPARE(dialog.requestBody()["account"].toString(), QString("@INPUT"));
    QCOMPARE(dialog.requestBody()["input_username"].toString(),
             QString("operator"));
    QCOMPARE(dialog.requestBody()["input_secret"].toString(),
             QString("test-only-secret"));
    QVERIFY(remember->isHidden());
    account->setCurrentIndex(2);
    QVERIFY(secret->text().isEmpty());
    secret->setText("dynamic-test");
    QCOMPARE(dialog.requestBody()["account"].toString(), QString("@USER"));
    QVERIFY(dialog.requestBody()["input_username"].toString().isEmpty());
    account->setCurrentIndex(0);
    QVERIFY(dialog.requestBody()["input_secret"].toString().isEmpty());
  }
  void rdpOptionsAndScope() {
    QSettings s;
    s.setValue("connection/rdpResolution", "1920x1080");
    s.setValue("connection/rdpClientOptions",
               QStringList{"full_screen", "multi_screen", "drives_redirect"});
    ConnectionDialog dialog(asset, details, methods, "a", false, false, "rdp",
                            nullptr);
    QCOMPARE(dialog.requestBody()["protocol"].toString(), QString("rdp"));
    QCOMPARE(dialog.extraParameters()["width"].toString(), QString("1920"));
    QCOMPARE(dialog.extraParameters()["multi_mon"].toString(), QString("1"));
    QCOMPARE(dialog.requestBody()["connect_options"]
                 .toObject()["rdp_client_option"]
                 .toArray()
                 .size(),
             3);
    QVERIFY(ConnectionDialog::connectionKey("site\nuser1\norg", "asset") !=
            ConnectionDialog::connectionKey("site\nuser2\norg", "asset"));
  }
  void savedConnectionAddress() {
    QJsonObject item{{"address", "192.0.2.10"},
      {"permed_accounts", QJsonArray{QJsonObject{{"id", "managed"}, {"name", "Operator"}, {"username", "root"}}}},
      {"permed_protocols", QJsonArray{QJsonObject{{"name", "ssh"}, {"port", 2222}}}}};
    QJsonObject saved{{"protocol", "ssh"}, {"account", "managed"}, {"accountLabel", "Operator (root)"}};
    QCOMPARE(ConnectionDialog::displayAddress(item, {}), QString("192.0.2.10"));
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("ssh://root@192.0.2.10:2222"));
    saved["account"] = "@INPUT";
    saved["accountUsername"] = "user@example";
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("ssh://user%40example@192.0.2.10:2222"));
    saved["account"] = "@USER";
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("ssh://192.0.2.10:2222"));
    item["address"] = "2001:db8::1";
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("ssh://[2001:db8::1]:2222"));
    item["address"] = "[2001:db8::1]";
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("ssh://[2001:db8::1]:2222"));
    item["address"] = "https://web.example:8443/path?q=1#/login";
    saved["protocol"] = "http";
    saved["account"] = "managed";
    saved["accountUsername"] = "admin";
    saved["port"] = 80;
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("https://admin@web.example:8443/path?q=1#/login"));
    item["address"] = "https://web.example/path";
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("https://admin@web.example/path"));
    item["address"] = "192.0.2.10";
    item.remove("permed_protocols");
    saved["protocol"] = "rdp";
    saved["port"] = 3389;
    QCOMPARE(ConnectionDialog::displayAddress(item, saved), QString("rdp://admin@192.0.2.10:3389"));
  }
  void savedConnectionAppearsOnCards() {
    QSettings().setValue("general/restoreLogin", false);
    auto item = asset;
    item["address"] = "192.0.2.10";
    auto permission = details;
    permission["permed_protocols"] = QJsonArray{QJsonObject{{"name", "ssh"}, {"port", 2222}}};
    permission["permed_accounts"] = QJsonArray{QJsonObject{{"id", "managed"}, {"name", "Operator"}, {"username", "root"}}};
    // Presentation fixture uses an empty site/profile/org, with no real session.
    const QString scope = "\n\n";
    AppService service;
    ConfigStore config(nullptr, directory.filePath("card-connection-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    emit service.assetsReady(QJsonArray{item}, 1);
    QCOMPARE(grid->item(0)->data(Qt::UserRole + 1).toString(), QString("192.0.2.10"));
    ConnectionDialog dialog(item, permission, methods, scope, false, true, "ssh", nullptr);
    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto choice = ConnectionDialog::saved(scope, "asset");
    QCOMPARE(choice["accountUsername"].toString(), QString("root"));
    QCOMPARE(choice["port"].toInt(), 2222);
    QVERIFY(!choice.contains("secret"));
    QVERIFY(!choice.contains("input_secret"));
    emit service.assetsReady(QJsonArray{item}, 1);
    QCOMPARE(grid->item(0)->data(Qt::UserRole + 1).toString(), QString("ssh://root@192.0.2.10:2222"));
    QVERIFY(grid->item(0)->toolTip().contains("SSH"));
    QVERIFY(grid->item(0)->toolTip().contains("Operator (root)"));
    if (!qEnvironmentVariableIsEmpty("CLIENT_QT_SCREENSHOT_DIR")) {
      window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
      window.show();
      for (const QString &theme : {QString("light"), QString("dark")}) {
        QSettings().setValue("appearance/theme", theme);
        ClientTheme::apply();
        QTest::qWait(30);
        capture(window, "saved-connection-card-" + theme);
      }
    }
    ConnectionDialog reopen(item, permission, methods, scope, false, false, {}, nullptr);
    QVERIFY(reopen.tryDirectConnection());
    MainWindow reopened(&service, &config, &launcher);
    emit service.assetsReady(QJsonArray{item}, 1);
    QCOMPARE(reopened.findChild<QListWidget *>("assetGrid")->item(0)->data(Qt::UserRole + 1).toString(),
             QString("ssh://root@192.0.2.10:2222"));
    emit service.loggedIn(QJsonObject{{"id", "another-user"}}, QJsonArray{}, QJsonObject{}, QJsonObject{});
    emit service.assetsReady(QJsonArray{item}, 1);
    QCOMPARE(grid->item(0)->data(Qt::UserRole + 1).toString(), QString("192.0.2.10"));
    QVERIFY(ConnectionDialog::saved("another-site\n\n", "asset").isEmpty());
    QVERIFY(ConnectionDialog::saved("\n\nanother-org", "asset").isEmpty());
  }
  void manualCardMetadataDoesNotPersistSecret() {
    ConnectionDialog dialog(asset, details, methods, "manual-card", false, true, "ssh", nullptr);
    dialog.findChild<QComboBox *>("account")->setCurrentIndex(1);
    dialog.findChild<QLineEdit *>("username")->setText("manual-user");
    dialog.findChild<QLineEdit *>("secret")->setText("synthetic-password");
    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto choice = ConnectionDialog::saved("manual-card", "asset");
    QCOMPARE(choice["accountUsername"].toString(), QString("manual-user"));
    QVERIFY(!QJsonDocument(choice).toJson().contains("synthetic-password"));
    QVERIFY(!choice["rememberSecret"].toBool());
  }
  void assetHoverCard_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void assetHoverCard() {
    QFETCH(QString, theme);
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/theme", theme);
    QSettings().setValue("appearance/language", "zh");
    QJsonObject item{{"id", "hover-fixture"}, {"name", "生产 Web 服务"},
      {"address", "https://web.example.test/apps/" + QString(100, 'a')},
      {"comment", "用于业务访问的远程应用。\n备注中的 <b>内容</b> 按纯文本显示。"}};
    QJsonObject saved{{"protocol", "http"}, {"account", "managed"},
      {"accountLabel", "运维账号 (admin)"}, {"accountUsername", "admin"},
      {"connectMethodLabel", "Chrome 浏览器"}};
    QSettings().setValue(ConnectionDialog::connectionKey("\n\n", "hover-fixture"),
                         QJsonDocument(saved).toJson(QJsonDocument::Compact));
    AppService service;
    ConfigStore config(nullptr, directory.filePath("hover-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    emit service.assetsReady(QJsonArray{item}, 1);
    window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
    window.show();
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    QTest::qWait(30);
    const QString html = grid->item(0)->toolTip();
    QVERIFY(!html.contains("<b>内容</b>"));
    QTextDocument document;
    document.setHtml(html);
    const QString plain = document.toPlainText().remove(QChar(0x200b));
    QVERIFY(plain.contains(item["address"].toString()));
    QVERIFY(plain.contains("<b>内容</b>"));
    QCOMPARE(plain.count("运维账号 (admin)"), 1);
    const bool fade = QApplication::isEffectEnabled(Qt::UI_FadeTooltip);
    const bool animate = QApplication::isEffectEnabled(Qt::UI_AnimateTooltip);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    const QPoint pos = grid->visualItemRect(grid->item(0)).center();
    QHelpEvent hover(QEvent::ToolTip, pos, grid->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(grid->viewport(), &hover);
    QTRY_VERIFY(QToolTip::isVisible());
    QCOMPARE(QToolTip::text(), html);
    QWidget *tip = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->windowType() == Qt::ToolTip && widget->isVisible()) tip = widget;
    QVERIFY(tip);
    QVERIFY2(tip->width() <= 460, qPrintable(QString::number(tip->width())));
    QVERIFY(tip->height() < window.screen()->availableGeometry().height());
    capture(*tip, "asset-hover-" + theme);
    QToolTip::hideText();
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, fade);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, animate);
  }
  void missingMethodsCannotConnect() {
    ConnectionDialog dialog(asset, details, {}, "a", false, false, {}, nullptr);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
  }
  void directConnectionRequiresCurrentAuthorization() {
    const QString scope = "site\nuser\norg";
    auto save = [&scope, this](const QJsonObject &value) {
      QSettings().setValue(ConnectionDialog::connectionKey(scope, "asset"),
                           QJsonDocument(value).toJson(QJsonDocument::Compact));
    };
    ConnectionDialog first(asset, details, methods, scope, false, false, {},
                           nullptr);
    QVERIFY(!first.canConnectDirectly());
    QVERIFY(first.canConnectDirectly(true));
    const QJsonObject choice{{"protocol", "ssh"},
                             {"account", "managed"},
                             {"connectMethod", "ssh_client"}};
    save(choice);
    ConnectionDialog valid(asset, details, methods, scope, false, false, {},
                           nullptr);
    QVERIFY(valid.canConnectDirectly());
    QVERIFY(valid.tryDirectConnection());
    QCOMPARE(ConnectionDialog::saved(scope, "asset")
                 .value("availableProtocols")
                 .toArray()
                 .size(),
             3);
    auto removed = choice;
    removed["account"] = "no-longer-authorized";
    save(removed);
    ConnectionDialog staleAccount(asset, details, methods, scope, false, false,
                                  {}, nullptr);
    QVERIFY(!staleAccount.canConnectDirectly(true));
    removed = choice;
    removed["connectMethod"] = "removed-method";
    save(removed);
    ConnectionDialog staleMethod(asset, details, methods, scope, false, false,
                                 {}, nullptr);
    QVERIFY(!staleMethod.canConnectDirectly());
    removed = choice;
    removed["account"] = "@USER";
    save(removed);
    ConnectionDialog dynamic(asset, details, methods, scope, false, false, {},
                             nullptr);
    QVERIFY(!dynamic.canConnectDirectly());
  }
#ifndef Q_OS_WIN
  void preservesRawConnectionUrl() {
    QSettings().setValue("general/restoreLogin", false);
    const QString path = directory.filePath("capture-url");
    QFile helper(path);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    helper.write("#!/bin/sh\nprintf '%s' \"$1\" > \"$0.url\"\n");
    helper.close();
    QVERIFY(helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                  QFile::ExeOwner));
    AppService service;
    ConfigStore config(nullptr, directory.filePath("url-test-config.json"));
    Launcher launcher(nullptr, path,
                      directory.filePath("url-test-config.json"));
    QSignalSpy finished(&launcher, &Launcher::finished);
    MainWindow window(&service, &config, &launcher);
    const QString raw =
        "jms://" +
        QString::fromLatin1(
            QByteArray("{\"protocol\":\"ssh\",\"address\":\"MixedCase\"}")
                .toBase64());
    QVERIFY(Launcher::isConnectionUrl(raw));
    window.handleUrl(raw);
    QTRY_COMPARE(finished.count(), 1);
    QFile captured(path + ".url");
    QVERIFY(captured.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(captured.readAll()), raw);
  }
#endif
  void switchesMainWindowLanguageImmediately() {
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/language", "zh");
    AppService service;
    ConfigStore config(nullptr, directory.filePath("language-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    const QString originalWelcome = window.findChild<QLabel *>("welcomeTitle")->text();
    QSignalSpy preferences(&window, &MainWindow::preferencesChanged);
    bool changed = false;
    QTimer::singleShot(0, &window, [&window, &changed] {
      auto *settings = window.findChild<SettingsDialog *>();
      if (!settings)
        return;
      auto *language = settings->findChild<QComboBox *>("appearance/language");
      if (language) {
        language->setCurrentIndex(language->findData("en"));
        changed = true;
      }
      settings->reject();
    });
    window.showSettings();
    QVERIFY(changed);
    QCOMPARE(preferences.count(), 1);
    const QString translatedWelcome = window.findChild<QLabel *>("welcomeTitle")->text();
    QVERIFY(translatedWelcome != originalWelcome);
    QVERIFY(translatedWelcome.contains("OSM"));
    QCOMPARE(window.findChild<QPushButton *>("siteSwitcher")->text(),
             QString("Switch site"));
    QCOMPARE(window.findChild<QLabel *>("profileLabel")->text(),
             QString("Not signed in"));
    QCOMPARE(window.findChild<QListWidget *>("categories")->item(2)->text(),
             QString("Databases"));
  }
  void loginLogoRemainsComplete_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("theme");
    QTest::addColumn<bool>("feedback");
    QTest::newRow("zh-dark-small-error") << QSize(800, 480) << QString("zh") << QString("dark") << true;
    QTest::newRow("zh-dark-wide-error") << QSize(1180, 720) << QString("zh") << QString("dark") << true;
    QTest::newRow("en-light-small-error") << QSize(800, 480) << QString("en") << QString("light") << true;
    QTest::newRow("zh-light-wide") << QSize(1180, 720) << QString("zh") << QString("light") << false;
  }
  void loginLogoRemainsComplete() {
    QFETCH(QSize, size);
    QFETCH(QString, language);
    QFETCH(QString, theme);
    QFETCH(bool, feedback);
    QSettings settings;
    settings.setValue("general/restoreLogin", false);
    settings.setValue("appearance/language", language);
    settings.setValue("appearance/theme", theme);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("login-layout-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.resize(size);
    window.show();
    if (feedback)
      emit service.status(language == "zh"
                              ? "登录失败：站点认证服务暂时不可用，请稍后重试。"
                              : "Sign-in failed: the authentication service is unavailable. Please try again later.");
    auto *logo = window.findChild<QLabel *>("loginLogo");
    auto *scroll = window.findChild<QScrollArea *>("loginScroll");
    auto *button = window.findChild<QPushButton *>("loginButton");
    auto *message = window.findChild<QLabel *>("loginMessage");
    QVERIFY(logo);
    QVERIFY(scroll);
    QVERIFY(button);
    QVERIFY(message);
    QTest::qWait(50);
    QCOMPARE(window.size(), size);
    QVERIFY(!scroll->horizontalScrollBar()->isVisible());
    QVERIFY(logo->visibleRegion().contains(logo->rect()));
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const QPixmap original = logo->pixmap(Qt::ReturnByValue);
#else
    const QPixmap original = logo->pixmap();
#endif
    QVERIFY(!original.isNull());
    const QSizeF logicalSize = QSizeF(original.size()) / original.devicePixelRatioF();
    QVERIFY(logo->contentsRect().width() >= logicalSize.width());
    QVERIFY(logo->contentsRect().height() >= logicalSize.height());
    const QPixmap rendered = logo->grab();
    const QRect expectedBounds = logoColorBounds(original.toImage());
    const QRect actualBounds = logoColorBounds(rendered.toImage());
    QVERIFY(!expectedBounds.isEmpty());
    QVERIFY(!actualBounds.isEmpty());
    const QSizeF expected = QSizeF(expectedBounds.size()) / original.devicePixelRatioF();
    const QSizeF actual = QSizeF(actualBounds.size()) / rendered.devicePixelRatioF();
    QVERIFY2(actual.width() >= expected.width() - 2, "Rendered logo is cropped horizontally");
    QVERIFY2(actual.height() >= expected.height() - 2, "Rendered logo is cropped vertically");
    capture(window, "login-" + QString::fromLatin1(QTest::currentDataTag()));
    if (feedback) {
      QVERIFY(!message->isVisible());
      scroll->ensureWidgetVisible(button, 0, 12);
      QTRY_VERIFY(button->visibleRegion().contains(button->rect()));
      QVERIFY(button->visibleRegion().contains(button->rect()));
      capture(window, "login-controls-" + QString::fromLatin1(QTest::currentDataTag()));
    }
  }
  void rendersAssetViewsAndOptionalScreenshots() {
    QSettings().setValue("general/restoreLogin", false);
    QSettings().setValue("appearance/language", "zh");
    QSettings().setValue("appearance/theme", "light");
    AppService service;
    ConfigStore config(nullptr, directory.filePath("ui-test-config.json"));
    Launcher launcher(nullptr, QString(),
                      directory.filePath("ui-test-config.json"));
    MainWindow window(&service, &config, &launcher);
    window.show();
    QSettings().setValue("connection/charset", "utf8");
    emit service.metadataReady(
        "preferences",
        QJsonObject{
            {"command_line",
             QJsonObject{{"charset", "gbk"}, {"is_backspace_as_ctrl_h", true}}},
            {"graphics",
             QJsonObject{{"rdp_resolution", "1920x1080"},
                         {"rdp_client_option", QJsonArray{"full_screen"}}}}});
    QCOMPARE(QSettings().value("connection/charset").toString(),
             QString("utf8"));
    QVERIFY(QSettings().value("connection/backspaceAsCtrlH").toBool());
    QCOMPARE(QSettings().value("connection/rdpResolution").toString(),
             QString("1920x1080"));
    QCOMPARE(QSettings().value("connection/rdpClientOptions").toStringList(),
             QStringList{"full_screen"});
    QTest::qWait(50);
    const QString output = qEnvironmentVariable("CLIENT_QT_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
      QVERIFY(QDir().mkpath(output));
      QVERIFY(window.grab().save(output + "/login.png"));
    }
    // Feed the same public presentation signals as the backend, without an
    // authenticated session, network traffic, or production fixture branches.
    QJsonArray assets;
    const QStringList names{"生产 Web 01", "测试数据库",   "北京跳板机",
                            "文件服务器",  "研发构建节点", "灾备应用"};
    const QStringList types{"linux",   "mysql", "linux",
                            "windows", "linux", "linux"};
    for (int i = 0; i < names.size(); ++i) {
      QJsonObject item{
          {"id", QString::number(i)},
          {"name", names[i]},
          {"address", QString("192.0.2.%1").arg(10 + i)},
          {"type", QJsonObject{{"value", types[i]}}},
          {"platform", QJsonObject{{"name", types[i]}}},
          {"is_active", i != 5},
          {"permed_accounts", QJsonArray{QJsonObject{{"name", "运维账号"}}}},
          {"permed_protocols",
           QJsonArray{QJsonObject{{"name", i == 1 ? "mysql" : "ssh"}}}}};
      assets.append(item);
    }
    emit service.assetsReady(assets, 26);
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    auto *table = window.findChild<QTableWidget *>("assetTable");
    QCOMPARE(grid->count(), 6);
    QCOMPARE(table->rowCount(), 6);
    QCOMPARE(table->item(0, 1)->text(), names[0]);
    QVERIFY(!table->item(0, 1)->icon().isNull());
    window.findChild<QStackedWidget *>("contentPages")->setCurrentIndex(1);
    for (const QString &name : {QString("refreshAssets"), QString("sortAssets"), QString("layoutToggle")})
      window.findChild<QWidget *>(name)->show();
    window.findChild<QLabel *>("profileLabel")
        ->setText("演示用户\nhttps://osm.example.com");
    window.findChild<QListWidget *>("categories")->setEnabled(true);
    grid->setCurrentRow(0);
    QTest::qWait(50);
    if (!output.isEmpty()) {
      QVERIFY(window.grab().save(output + "/assets-grid.png"));
      window.findChild<QStackedWidget *>("assetViews")->setCurrentIndex(1);
      QTest::qWait(30);
      QVERIFY(window.grab().save(output + "/assets-table.png"));
      window.findChild<QStackedWidget *>("assetViews")->setCurrentIndex(0);
      QSettings().setValue("appearance/theme", "dark");
      ClientTheme::apply();
      QTest::qWait(30);
      QVERIFY(window.grab().save(output + "/assets-dark.png"));
      QSettings().setValue("appearance/theme", "light");
      ClientTheme::apply();
      ConnectionDialog dialog(assets.first().toObject(), details, methods,
                              "fixture", false, false, {}, &window);
      dialog.show();
      QTest::qWait(30);
      QVERIFY(dialog.grab().save(output + "/connection.png"));
      auto *account = dialog.findChild<QComboBox *>("account");
      auto *username = dialog.findChild<QLineEdit *>("username");
      auto *secret = dialog.findChild<QLineEdit *>("secret");
      account->setCurrentIndex(1);
      QTest::qWait(30);
      QVERIFY(username->isVisible());
      QVERIFY(secret->isVisible());
      QVERIFY(dialog.rect().contains(QRect(username->mapTo(&dialog, QPoint()), username->size())));
      QVERIFY(dialog.rect().contains(QRect(secret->mapTo(&dialog, QPoint()), secret->size())));
      capture(dialog, "connection-manual");
      account->setCurrentIndex(2);
      QTest::qWait(30);
      QVERIFY(!username->isVisible());
      QVERIFY(secret->isVisible());
      capture(dialog, "connection-dynamic");
      dialog.hide();
      SettingsDialog settings(&config, QJsonArray{"4.1.2", "4.1.3"}, &window);
      settings.show();
      QTest::qWait(30);
      QVERIFY(settings.grab().save(output + "/settings.png"));
      auto *navigation = settings.findChild<QListWidget *>("settingsNavigation");
      QVERIFY(navigation);
      navigation->setCurrentRow(3);
      QTest::qWait(30);
      QVERIFY(settings.grab().save(output + "/applications.png"));
      auto *database = applicationProtocolButton(&settings, "mysql");
      QVERIFY(database);
      database->click();
      QTest::qWait(30);
      QVERIFY(settings.grab().save(output + "/applications-database.png"));
      QSettings().setValue("appearance/theme", "dark");
      ClientTheme::apply();
      QTest::qWait(30);
      QVERIFY(settings.grab().save(output + "/applications-dark.png"));
      QSettings().setValue("appearance/theme", "system");
      ClientTheme::apply();
      QTest::qWait(30);
      navigation->setCurrentRow(4);
      QTest::qWait(30);
      QVERIFY(settings.grab().save(output + "/about.png"));
    }
    window.close();
  }
  void visualLayoutMatrix_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("theme");
    QTest::addColumn<bool>("collapsed");
    QTest::addColumn<int>("fontSize");
    QTest::addColumn<int>("columns");
    QTest::newRow("zh-light-wide") << QSize(1180, 720) << QString("zh") << QString("light") << false << 11 << 3;
    QTest::newRow("zh-light-narrow") << QSize(800, 480) << QString("zh") << QString("light") << false << 11 << 1;
    QTest::newRow("en-light-narrow") << QSize(800, 480) << QString("en") << QString("light") << false << 11 << 1;
    QTest::newRow("en-dark-wide") << QSize(1180, 720) << QString("en") << QString("dark") << false << 11 << 3;
    QTest::newRow("zh-light-collapsed") << QSize(800, 480) << QString("zh") << QString("light") << true << 11 << 2;
    QTest::newRow("en-large-font") << QSize(1000, 640) << QString("en") << QString("light") << false << 14 << 2;
  }
  void visualLayoutMatrix() {
    QFETCH(QSize, size);
    QFETCH(QString, language);
    QFETCH(QString, theme);
    QFETCH(bool, collapsed);
    QFETCH(int, fontSize);
    QFETCH(int, columns);
    QSettings settings;
    settings.setValue("general/restoreLogin", false);
    settings.setValue("appearance/language", language);
    settings.setValue("appearance/theme", theme);
    settings.setValue("appearance/sidebarCollapsed", collapsed);
    settings.setValue("appearance/fontSize", fontSize);
    AppService service;
    ConfigStore config(nullptr, directory.filePath("matrix-config.json"));
    Launcher launcher;
    MainWindow window(&service, &config, &launcher);
    window.resize(size);
    window.show();
    emit service.assetsReady(visualAssets(), 26);
    auto *content = window.findChild<QStackedWidget *>("contentPages");
    auto *grid = window.findChild<QListWidget *>("assetGrid");
    auto *toolbar = window.findChild<QWidget *>("assetToolbar");
    auto *topbar = window.findChild<QWidget *>("topbar");
    auto *sidebar = window.findChild<QWidget *>("sidebar");
    QVERIFY(content);
    QVERIFY(grid);
    QVERIFY(toolbar);
    QVERIFY(topbar);
    QVERIFY(sidebar);
    content->setCurrentIndex(1);
    for (const QString &name : {QString("refreshAssets"), QString("sortAssets"), QString("layoutToggle")})
      window.findChild<QWidget *>(name)->show();
    window.findChild<QLabel *>("profileLabel")->setText("Synthetic operator");
    window.findChild<QListWidget *>("categories")->setEnabled(true);
    auto *organizations = window.findChild<QComboBox *>("organizationSelector");
    QVERIFY(organizations);
    {
      const QSignalBlocker blocker(organizations);
      organizations->addItems({"Synthetic organization", "Second organization"});
      organizations->show();
    }
    grid->setCurrentRow(0);
    QTest::qWait(250);
    QCOMPARE(window.size(), size);
    QVERIFY(collapsed ? sidebar->width() < 120 : sidebar->width() > 150);
    const QString problem = layoutProblem(toolbar);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    const QString headerProblem = layoutProblem(topbar);
    QVERIFY2(headerProblem.isEmpty(), qPrintable(headerProblem));
    QVERIFY(!grid->horizontalScrollBar()->isVisible());
    qInfo() << QTest::currentDataTag() << "viewport" << grid->viewport()->size()
            << "gridSize" << grid->gridSize() << "firstRects"
            << grid->visualItemRect(grid->item(0))
            << grid->visualItemRect(grid->item(1))
            << grid->visualItemRect(grid->item(2));
    int firstRowColumns = 0;
    const int firstTop = grid->visualItemRect(grid->item(0)).top();
    for (int row = 0; row < grid->count(); ++row) {
      const QRect rect = grid->visualItemRect(grid->item(row));
      if (qAbs(rect.top() - firstTop) <= 1)
        ++firstRowColumns;
      QVERIFY(rect.width() > 100);
      QVERIFY(qAbs(rect.width() - grid->gridSize().width()) <= 1);
      QVERIFY(qAbs(rect.height() - grid->gridSize().height()) <= 1);
      QVERIFY(rect.left() >= 0);
      QVERIFY(rect.right() < grid->viewport()->width());
      for (int prior = 0; prior < row; ++prior)
        QVERIFY(!rect.intersects(grid->visualItemRect(grid->item(prior))));
    }
    QCOMPARE(firstRowColumns, columns);
    const QRect button = paintedConnectBounds(grid);
    QFont buttonFont = grid->font();
    buttonFont.setWeight(QFont::DemiBold);
    const QFontMetrics buttonText(buttonFont);
    const int textWidth = buttonText.horizontalAdvance(language == "en" ? "Connect" : "连接");
    qInfo() << "paintedConnect" << button << "requiredText" << QSize(textWidth, buttonText.height());
    // The actual button uses a font no larger than the card font; this is a
    // conservative text-fit check against the painted area at large font sizes.
    QVERIFY(button.width() >= textWidth);
    QVERIFY(button.height() >= buttonText.height());
    capture(window, "matrix-" + QString::fromLatin1(QTest::currentDataTag()));
    {
      auto *views = window.findChild<QStackedWidget *>("assetViews");
      auto *table = window.findChild<QTableWidget *>("assetTable");
      QVERIFY(views);
      QVERIFY(table);
      views->setCurrentIndex(1);
      QTest::qWait(30);
      QVERIFY(!table->wordWrap());
      QCOMPARE(table->textElideMode(), Qt::ElideRight);
      QCOMPARE(table->item(2, 3)->text(), QStringLiteral("—"));
      QCOMPARE(table->item(2, 4)->text(), QStringLiteral("—"));
      for (int column = 0; column < table->columnCount(); ++column) {
        const Qt::Alignment alignment = (column == 0 || column == 4 || column == 6)
            ? Qt::AlignHCenter : Qt::AlignLeft;
        QVERIFY(table->horizontalHeaderItem(column)->textAlignment() & alignment);
      }
      auto *cell = table->cellWidget(0, 6);
      QVERIFY(cell);
      auto *action = qobject_cast<QPushButton *>(cell);
      if (!action)
        action = cell->findChild<QPushButton *>();
      QVERIFY(action);
      const QRect actionRect(action->mapTo(cell, QPoint()), action->size());
      qInfo() << "table action" << actionRect << "cell" << cell->rect()
              << "column" << table->columnWidth(6);
      QVERIFY2(cell->rect().contains(actionRect), "Connection button must fit inside its table cell");
      QStyleOptionButton option;
      option.initFrom(action);
      option.text = action->text();
      const QRect content = action->style()->subElementRect(QStyle::SE_PushButtonContents, &option, action);
      const QFontMetrics metrics(action->font());
      qInfo() << "tableAction" << action->size() << "content" << content
              << "textAdvance" << metrics.horizontalAdvance(action->text())
              << "textInk" << metrics.tightBoundingRect(action->text());
      QVERIFY(metrics.horizontalAdvance(action->text()) <= content.width());
      QVERIFY(metrics.tightBoundingRect(action->text()).height() <= content.height());
      capture(window, "matrix-" + QString::fromLatin1(QTest::currentDataTag()) + "-table");
      views->setCurrentIndex(0);
      QTest::qWait(30);
    }

    // Exercise the actual painted-button hit regions without authenticating or
    // launching anything. The local service reports missing authorization; its
    // window feedback is detached so this expected result cannot open a dialog.
    QObject::disconnect(&service, nullptr, &window, nullptr);
    QSignalSpy requests(&service, &AppService::error);
    const auto click = [grid](int row, bool button) {
      auto *item = grid->item(row);
      grid->scrollToItem(item, QAbstractItemView::PositionAtCenter);
      QTest::qWait(10);
      const QRect rect = grid->visualItemRect(item);
      const QPoint position(button ? rect.right() - 49 : rect.left() + 70,
                            rect.center().y());
      QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    };
    click(0, false);
    QCOMPARE(requests.count(), 0);
    click(5, true);
    QCOMPARE(requests.count(), 0);
    click(0, true);
    QCOMPARE(requests.count(), 1);
  }
};
QTEST_MAIN(TestUi)
#include "test_ui.moc"
