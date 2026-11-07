#include "mainwindow.h"
#include "appservice.h"
#include "connectiondialog.h"
#include "platform.h"
#include "settingsdialog.h"
#include "theme.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
using ClientTheme::text;
namespace {
class RestoreSpinner : public QWidget {
public:
  explicit RestoreSpinner(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedSize(36, 36);
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, [this] {
      m_angle = (m_angle + 10) % 360;
      update();
    });
  }
protected:
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    m_timer.start();
  }
  void hideEvent(QHideEvent *event) override {
    m_timer.stop();
    QWidget::hideEvent(event);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = QRectF(rect()).adjusted(3, 3, -3, -3);
    QColor track = palette().color(QPalette::Text);
    track.setAlpha(25);
    painter.setPen(QPen(track, 3));
    painter.drawEllipse(ring);
    painter.setPen(QPen(palette().color(QPalette::Highlight), 3,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, -m_angle * 16, 100 * 16);
  }
private:
  QTimer m_timer;
  int m_angle = 0;
};
const QStringList categoryKeys = {"linux", "windows", "database", "device",
                                  "web",   "other",   "favorite"};
QStringList categoryLabels() {
  return {"Linux",
          "Windows",
          text("数据库", "Databases"),
          text("网络设备", "Devices"),
          text("Web 应用", "Web apps"),
          text("其他", "Other"),
          text("我的收藏", "Favorites")};
}
QString displayName(const QJsonValue &value) {
  return value.isObject() ? value.toObject()["name"].toString(
                                value.toObject()["label"].toString())
                          : value.toString();
}
QIcon assetIcon(const QJsonObject &asset) {
  auto type = asset["type"];
  QString name =
      type.isObject() ? type.toObject()["value"].toString() : type.toString();
  if (name.isEmpty())
    name = displayName(asset["platform"]).toLower();
  name = name.toLower();
  if (name == "postgresql")
    name = "postgre";
  if (name == "windows_ad")
    name = "windows";
  if (name == "website" || name == "web" || name == "http")
    name = "browser";
  const QSet<QString> icons = {"linux",   "windows",   "browser",    "mysql",
                               "mariadb", "mongodb",   "oracle",     "postgre",
                               "redis",   "sqlserver", "clickhouse", "dameng", "db2"};
  if (!icons.contains(name))
    return ClientTheme::icon("desktop");
  return QIcon(":/icons/" + name + ".png");
}
class AssetSearch : public QLineEdit {
public:
  AssetSearch() {
    setTextMargins(36, 0, 0, 0);
    auto *clearAction = addAction(ClientTheme::icon("close"), QLineEdit::TrailingPosition);
    clearAction->setObjectName("clearAssetSearch");
    clearAction->setVisible(false);
    connect(this, &QLineEdit::textChanged, this, [clearAction](const QString &value) {
      clearAction->setVisible(!value.isEmpty());
    });
    connect(clearAction, &QAction::triggered, this, &QLineEdit::clear);
  }
protected:
  void paintEvent(QPaintEvent *event) override {
    QLineEdit::paintEvent(event);
    // QLineEdit actions use platform tool-button metrics, which can shift the
    // leading icon below the text when combined with the application stylesheet.
    QPainter painter(this);
    const QRect rect(12, (height() - 16) / 2, 16, 16);
    ClientTheme::icon("search").paint(&painter, rect, Qt::AlignCenter,
                                     hasFocus() ? QIcon::Normal : QIcon::Disabled);
  }
};
class AssetGrid : public QListWidget {
public:
  using QListWidget::QListWidget;
protected:
  void resizeEvent(QResizeEvent *event) override {
    QListWidget::resizeEvent(event);
    updateGridSize();
  }
  void changeEvent(QEvent *event) override {
    QListWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) updateGridSize();
  }
private:
  void updateGridSize() {
    // Reserve the scrollbar plus rounding space even while it is hidden, so
    // showing/hiding it cannot push the last column onto a new row.
    const int inset = verticalScrollBar()->sizeHint().width() + 2;
    const int width = qMax(1, viewport()->width() - inset);
    const int columns = qMax(1, width / 284);
    const int height = qMax(92, fontMetrics().height() * 2 + 52);
    const QSize cell(width / columns, height);
    if (gridSize() != cell) setGridSize(cell);
  }
};
QString assetToolTip(const QJsonObject &asset, const QJsonObject &saved) {
  const auto colors = ClientTheme::colors();
  // Qt's rich text labels need break opportunities in long URLs/identifiers.
  // Escape each chunk so server-provided names and comments remain plain text.
  const auto wrapped = [](const QString &value) {
    QString result;
    int run = 0;
    for (const QChar ch : value) {
      if (ch == '\n') { result += "<br>"; run = 0; continue; }
      result += QString(ch).toHtmlEscaped();
      if (ch.isSpace()) run = 0;
      else if (!ch.isHighSurrogate() && ++run >= 20) {
        result += QChar(0x200b);
        run = 0;
      }
    }
    return result;
  };
  const auto row = [&](const QString &label, const QString &value) {
    if (value.trimmed().isEmpty()) return QString();
    return QStringLiteral("<tr><td width='72' valign='top' style='color:%1'>%2</td>"
                          "<td valign='top'>%3</td></tr>")
        .arg(colors.muted.name(), label.toHtmlEscaped(), wrapped(value));
  };
  QString rows = row(text("地址", "Address"), asset["address"].toString());
  if (!saved.isEmpty()) {
    QString account = saved.value("accountLabel").toString();
    const QString username = saved.value("accountUsername").toString();
    if (account.isEmpty()) account = username;
    else if (saved.value("account").toString() == "@INPUT" && !username.isEmpty())
      account += " (" + username + ")";
    rows += row(text("账号", "Account"), account);
    const int port = saved.value("port").toInt();
    if (!asset["address"].toString().contains("://") && port > 0)
      rows += row(text("端口", "Port"), QString::number(port));
    rows += row(text("连接方式", "Method"), saved.value("connectMethodLabel").toString(
        saved.value("connectMethod").toString()));
  }
  rows += row(text("备注", "Notes"), asset.value("comment").toString());
  const QString protocol = saved.value("protocol").toString().toUpper();
  const int width = qBound(300, QFontMetrics(QApplication::font()).averageCharWidth() * 42, 360);
  return QStringLiteral("<qt><table width='%1' cellspacing='0' cellpadding='3'>"
                        "<tr><td colspan='2' style='font-weight:600'>%2</td></tr>"
                        "%3<tr><td colspan='2' height='8'></td></tr>%4</table></qt>")
      .arg(width).arg(wrapped(asset.value("name").toString()),
          protocol.isEmpty() ? QString() : QStringLiteral("<tr><td colspan='2' style='color:%1;font-weight:600'>%2</td></tr>")
              .arg(colors.accent.name(), wrapped(protocol)), rows);
}
class AssetTable : public QTableWidget {
public:
  using QTableWidget::QTableWidget;
  int hoveredRow() const { return m_hoveredRow; }
  void updateColumns() {
    if (columnCount() != 7) return;
    const QFontMetrics metrics(font());
    const int favorite = qMax(56, metrics.horizontalAdvance(text("收藏", "Favorite")) + 24);
    const int protocol = qMax(80, metrics.horizontalAdvance("HTTPS") + 28);
    // Include the item padding as well as the action widget's layout margins.
    const int action = qMax(120, metrics.horizontalAdvance(text("连接", "Connect")) + 72);
    setColumnWidth(0, favorite);
    setColumnWidth(4, protocol);
    setColumnWidth(6, action);
    const int available = qMax(0, viewport()->width() - favorite - protocol - action);
    const int extra = qMax(0, available - 580);
    setColumnWidth(1, 180 + extra * 30 / 100);
    setColumnWidth(2, 180 + extra * 34 / 100);
    setColumnWidth(3, 110 + extra * 18 / 100);
    setColumnWidth(5, 110 + extra - extra * 30 / 100 - extra * 34 / 100 - extra * 18 / 100);
    horizontalHeader()->setFixedHeight(qMax(40, metrics.height() + 18));
  }
protected:
  void resizeEvent(QResizeEvent *event) override {
    QTableWidget::resizeEvent(event);
    updateColumns();
  }
  void changeEvent(QEvent *event) override {
    QTableWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) updateColumns();
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    QTableWidget::mouseMoveEvent(event);
    const int row = indexAt(event->pos()).row();
    if (row != m_hoveredRow) { m_hoveredRow = row; viewport()->update(); }
  }
  void leaveEvent(QEvent *event) override {
    m_hoveredRow = -1;
    viewport()->update();
    QTableWidget::leaveEvent(event);
  }
private:
  int m_hoveredRow = -1;
};
class AssetTableDelegate : public QStyledItemDelegate {
public:
  explicit AssetTableDelegate(AssetTable *table) : QStyledItemDelegate(table), m_table(table) {}
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem cell(option);
    initStyleOption(&cell, index);
    cell.state.setFlag(QStyle::State_MouseOver, index.row() == m_table->hoveredRow());
    const QString protocol = cell.text;
    const bool badge = index.column() == 4 && !protocol.isEmpty() && protocol != QStringLiteral("—");
    if (badge) cell.text.clear();
    const QWidget *widget = cell.widget;
    (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &cell, painter, widget);
    if (!badge) return;
    const auto colors = ClientTheme::colors();
    const bool active = QJsonObject::fromVariantMap(index.data(Qt::UserRole).toMap()).value("is_active").toBool(true);
    QFont font = cell.font;
    font.setPointSizeF(qMax(9.0, font.pointSizeF() - 1));
    font.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(font);
    const int width = qMin(cell.rect.width() - 16, metrics.horizontalAdvance(protocol) + 16);
    const int height = metrics.height() + 8;
    QRect badgeRect(0, 0, width, height);
    badgeRect.moveCenter(cell.rect.center());
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(active ? colors.softAccent : colors.background);
    painter->drawRoundedRect(badgeRect, 5, 5);
    painter->setFont(font);
    painter->setPen(active ? colors.accent : colors.muted);
    painter->drawText(badgeRect, Qt::AlignCenter, metrics.elidedText(protocol, Qt::ElideRight, width - 12));
    painter->restore();
  }
private:
  AssetTable *m_table;
};
class AssetDelegate : public QStyledItemDelegate {
public:
  AssetDelegate(QObject *parent, std::function<void(QJsonObject)> connect)
      : QStyledItemDelegate(parent), m_connect(std::move(connect)) {}
  static QFont buttonFont(QFont font) {
    font.setPointSizeF(qMax(9.0, font.pointSizeF() - 1.0));
    font.setWeight(QFont::DemiBold);
    return font;
  }
  static QRect connectRect(const QRect &rect, const QFont &font) {
    const QRect card = rect.adjusted(6, 6, -6, -6);
    const QFontMetrics metrics(buttonFont(font));
    const int width = qMax(60, qMax(metrics.horizontalAdvance(text("连接", "Connect")),
                                    metrics.horizontalAdvance(text("已禁用", "Disabled"))) + 20);
    const int height = qMax(28, metrics.height() + 8);
    return {card.right() - width - 13, card.center().y() - height / 2, width, height};
  }
  bool editorEvent(QEvent *event, QAbstractItemModel *,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    if (event->type() != QEvent::MouseButtonRelease) return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    auto asset = QJsonObject::fromVariantMap(index.data(Qt::UserRole).toMap());
    if (mouse->button() != Qt::LeftButton ||
        !connectRect(option.rect, option.font).contains(mouse->pos()) ||
        !asset.value("is_active").toBool(true)) return false;
    m_connect(asset);
    return true;
  }
  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
    const auto *view = qobject_cast<QListView *>(parent());
    if (view && view->gridSize().isValid()) return view->gridSize();
    return {284, qMax(92, option.fontMetrics.height() * 2 + 52)};
  }
  void paint(QPainter *p, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const auto c = ClientTheme::colors();
    const QRect r = option.rect.adjusted(6, 6, -6, -6);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    const auto asset = QJsonObject::fromVariantMap(index.data(Qt::UserRole).toMap());
    const bool active = asset.value("is_active").toBool(true);
    // A restrained border/shadow keeps the compact row readable on both themes.
    QColor shadow = Qt::black;
    shadow.setAlpha(hover ? 10 : 4);
    p->setPen(Qt::NoPen);
    p->setBrush(shadow);
    p->drawRoundedRect(r.translated(0, 1), 10, 10);
    p->setPen(QPen(selected || hover ? c.accent : c.border, 1));
    p->setBrush(selected ? c.softAccent : c.panel);
    p->drawRoundedRect(r, 10, 10);
    const QRect badge(r.left() + 14, r.center().y() - 19, 38, 38);
    p->setPen(Qt::NoPen);
    p->setBrush(c.background);
    p->drawRoundedRect(badge, 8, 8);
    assetIcon(asset).paint(p, badge.adjusted(6, 6, -6, -6));
    const QRect button = connectRect(option.rect, option.font);
    const int textLeft = badge.right() + 12;
    const int textWidth = qMax(0, button.left() - textLeft - 10);
    QFont title = option.font;
    title.setWeight(QFont::DemiBold);
    QFont detail = option.font;
    detail.setPointSizeF(qMax(9.0, detail.pointSizeF() - 1.0));
    const int titleHeight = QFontMetrics(title).height();
    const int detailHeight = QFontMetrics(detail).height();
    const int textTop = r.center().y() - (titleHeight + detailHeight + 3) / 2;
    p->setFont(title);
    p->setPen(active ? c.text : c.muted);
    p->drawText(QRect(textLeft, textTop, textWidth, titleHeight), Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));
    p->setFont(detail);
    p->setPen(c.muted);
    p->drawText(QRect(textLeft, textTop + titleHeight + 3, textWidth, detailHeight), Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(index.data(Qt::UserRole + 1).toString(), Qt::ElideRight, textWidth));
    p->setPen(Qt::NoPen);
    p->setBrush(active ? c.softAccent : c.background);
    p->drawRoundedRect(button, 8, 8);
    p->setPen(active ? c.accent : c.muted);
    p->setFont(buttonFont(option.font));
    p->drawText(button, Qt::AlignCenter, active ? text("连接", "Connect") : text("已禁用", "Disabled"));
    p->restore();
  }
private:
  std::function<void(QJsonObject)> m_connect;
};
} // namespace
MainWindow::MainWindow(AppService *service, ConfigStore *config,
                       Launcher *launcher, QWidget *parent)
    : QMainWindow(parent), m_service(service), m_config(config),
      m_launcher(launcher) {
  setWindowTitle("OSMClient");
  setWindowIcon(ClientTheme::applicationIcon());
  setMinimumSize(800, 480);
  resize(1180, 720);
  ClientTheme::apply();
  buildUi();
  connect(service, &AppService::loginStateChanged, this,
          [this](bool pending) {
            if (m_restoring && !pending && !m_service->authenticated()) {
              m_restoring = false;
              updateSessionUi();
            }
            updateLoginState();
          });
  connect(service, &AppService::status, this, [this](const QString &message, bool) {
    statusBar()->showMessage(message);
    m_loginMessage->clear();
    m_loginMessage->hide();
  });
  connect(service, &AppService::authorizationUrlReady, this,
          [this](const QUrl &url) {
            m_authorizationUrl = url;
            m_authTools->setVisible(url.isValid());
          });
  connect(service, &AppService::error, this, [this](const QString &message, bool statusOnly) {
    if (m_restoring) {
      m_restoring = false;
      updateSessionUi();
    }
    m_loading = false;
    m_connectionInFlight = false;
    updateLoginState();
    m_pendingAsset = {};
    m_waitingDetail = {};
    m_connectionQueue.clear();
    m_refreshButton->setEnabled(m_service->authenticated());
    showError(message, statusOnly);
  });
  connect(service, &AppService::loggedIn, this,
          [this](QJsonObject profile, QJsonValue permissions, QJsonObject org,
                 QJsonObject publicSettings) {
            m_restoring = false;
            m_profileData = profile;
            m_publicSettings = publicSettings;
            m_methods = {};
            m_methodsLoaded = false;
            m_favorites.clear();
            m_pendingFavorites.clear();
            m_pendingAsset = {};
            m_waitingDetail = {};
            m_connectionQueue.clear();
            m_connectionInFlight = false;
            m_authorizationUrl = QUrl();
            m_authTools->hide();
            updateLoginState();
            QSignalBlocker blocker(m_organizations);
            m_organizations->clear();
            QJsonArray all;
            if (permissions.isArray())
              all = permissions.toArray();
            else {
              auto object = permissions.toObject();
              for (const auto &key :
                   QStringList{"pam_orgs", "audit_orgs", "console_orgs",
                               "workbench_orgs"})
                for (const auto &v : object[key].toArray())
                  all.append(v);
            }
            QSet<QString> seen;
            for (const auto &v : all) {
              auto o = v.toObject();
              const QString id = o["id"].toString();
              if (id.isEmpty() || seen.contains(id))
                continue;
              seen.insert(id);
              m_organizations->addItem(o["name"].toString(), id);
            }
            if (!seen.contains(org["id"].toString()) &&
                !org["id"].toString().isEmpty())
              m_organizations->addItem(org["name"].toString(),
                                       org["id"].toString());
            int idx = m_organizations->findData(m_service->org());
            if (idx >= 0)
              m_organizations->setCurrentIndex(idx);
            m_organizations->setVisible(
                m_organizations->count() > 1 &&
                publicSettings.value("XPACK_LICENSE_IS_VALID").toBool());
            QSettings settings;
            auto sites = settings.value("recentSites").toStringList();
            sites.removeAll(m_service->site());
            sites.prepend(m_service->site());
            while (sites.size() > 12)
              sites.removeLast();
            settings.setValue("recentSites", sites);
            settings.setValue("lastSite", m_service->site());
            {
              QSignalBlocker block(m_sites);
              m_sites->clear();
              m_sites->addItems(sites);
              m_sites->setCurrentText(m_service->site());
            }
            updateSessionUi();
            refresh(true);
          });
  connect(service, &AppService::loggedOut, this, [this] {
    m_connectionInFlight = false;
    m_loading = false;
    m_count = 0;
    m_offset = 0;
    m_assets = {};
    m_profileData = {};
    m_publicSettings = {};
    m_version = {};
    m_authorizationUrl = QUrl();
    m_methods = {};
    m_methodsLoaded = false;
    m_pendingAsset = {};
    m_waitingDetail = {};
    m_connectionQueue.clear();
    m_favorites.clear();
    m_pendingFavorites.clear();
    updateLoginState();
    updateSessionUi();
    renderAssets();
  });
  connect(service, &AppService::assetsReady, this,
          [this](QJsonArray assets, int count) {
            m_loading = false;
            m_refreshButton->setEnabled(true);
            m_assets = assets;
            m_count = count;
            renderAssets();
          });
  connect(service, &AppService::detailReady, this, &MainWindow::consumeDetails);
  connect(service, &AppService::favoriteFinished, this,
          [this](const QString &id, bool enabled, bool success) {
    m_pendingFavorites.remove(id);
    if (success) {
      if (enabled) m_favorites.insert(id); else m_favorites.remove(id);
      if (!enabled && categoryKeys.value(m_categories->currentRow()) == "favorite") {
        for (int i = m_assets.size() - 1; i >= 0; --i) {
          if (m_assets[i].toObject()["id"].toString() == id) {
            m_assets.removeAt(i);
            m_count = qMax(0, m_count - 1);
          }
        }
      }
    }
    // A failed authorization can finish synchronously inside the click handler.
    // Defer rebuilding cell widgets until the button event has returned.
    QTimer::singleShot(0, this, [this] { renderAssets(); });
  });
  connect(service, &AppService::changed, this, [this] {
    m_service->metadata();
    refresh();
  });
  connect(service, &AppService::metadataReady, this,
          [this](QString kind, QJsonValue value) {
            if (kind == "methods") {
              m_methods = value.toObject();
              m_methodsLoaded = true;
              if (value.isObject()) { m_methodsAge.restart(); m_methodsCacheRevision = m_service->methodsRevision(); }
              else m_methodsAge.invalidate();
              if (!m_waitingDetail.isEmpty()) {
                auto detail = m_waitingDetail;
                m_waitingDetail = {};
                consumeDetails(m_pendingAsset["id"].toString(), detail);
              }
            } else if (kind == "version")
              m_version = value;
            else if (kind == "favorites") {
              m_favorites.clear();
              auto values = value.isArray()
                                ? value.toArray()
                                : value.toObject()["results"].toArray();
              for (const auto &v : values) {
                auto o = v.toObject();
                QString id = o["asset"].isObject()
                                 ? o["asset"].toObject()["id"].toString()
                                 : o["asset"].toString();
                if (id.isEmpty())
                  id = o["id"].toString();
                if (!id.isEmpty())
                  m_favorites.insert(id);
              }
              renderAssets();
            } else if (kind == "preferences") {
              auto p = value.toObject();
              const auto graphics = p.value("graphics").toObject();
              const auto commandLine = p.value("command_line").toObject();
              for (auto it = graphics.begin(); it != graphics.end(); ++it)
                p[it.key()] = it.value();
              if (commandLine.contains("charset"))
                p["charset"] = commandLine.value("charset");
              if (commandLine.contains("is_backspace_as_ctrl_h"))
                p["backspace_as_ctrl_h"] =
                    commandLine.value("is_backspace_as_ctrl_h");
              QSettings settings;
              const QHash<QString, QString> keys = {
                  {"charset", "charset"},
                  {"rdp_resolution", "rdpResolution"},
                  {"keyboard_layout", "keyboardLayout"},
                  {"rdp_color_quality", "rdpColorQuality"},
                  {"rdp_smart_size", "rdpSmartSize"},
                  {"backspace_as_ctrl_h", "backspaceAsCtrlH"}};
              for (auto it = keys.begin(); it != keys.end(); ++it)
                if (p.contains(it.key()) &&
                    !settings.contains("connection/" + it.value()))
                  settings.setValue("connection/" + it.value(),
                                    p[it.key()].toVariant());
              if (p["rdp_client_option"].isArray() &&
                  !settings.contains("connection/rdpClientOptions")) {
                QStringList values;
                for (const auto &v : p["rdp_client_option"].toArray())
                  values << v.toString();
                settings.setValue("connection/rdpClientOptions", values);
              }
            }
          });
  connect(service, &AppService::clientUrlReady, launcher, &Launcher::launch);
  connect(launcher, &Launcher::started, this, [this] {
    m_connectionInFlight = false;
    statusBar()->showMessage(text("已启动本地客户端", "Local client started"),
                             5000);
    processNextConnection();
  });
  connect(launcher, &Launcher::failed, this, [this](const QString &message) {
    m_connectionInFlight = false;
    m_connectionQueue.clear();
    m_pendingAsset = {};
    m_waitingDetail = {};
    showError(message);
  });
  auto *findShortcut = new QShortcut(QKeySequence::Find, this);
  connect(findShortcut, &QShortcut::activated, this, [this] {
    if (!m_search->isEnabled()) return;
    if (m_collapsed) m_collapse->click();
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
  });
  auto *refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
  connect(refreshShortcut, &QShortcut::activated, this, [this] { refresh(); });
  restoreGeometry(QSettings().value("window/geometry").toByteArray());
  updateSessionUi();
  if (!QCoreApplication::arguments().contains("--smoke-test") &&
      QSettings().value("general/restoreLogin", true).toBool()) {
    const QString last = QSettings().value("lastSite").toString();
    if (!last.isEmpty()) {
      // Select the transition before the first paint, including credential lookup.
      m_restoring = true;
      updateSessionUi();
      QTimer::singleShot(0, this, [this, last] {
        if (m_restoring) m_service->restore(last);
      });
    }
  }
}
void MainWindow::buildUi() {
  auto *central = new QWidget(this);
  setCentralWidget(central);
  auto *root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  auto iconButton = [this](const QString &name, const QString &objectName) {
    auto *button = new QPushButton(this);
    button->setObjectName(objectName);
    button->setIcon(ClientTheme::icon(name));
    button->setIconSize(QSize(19, 19));
    button->setProperty("quiet", true);
    button->setProperty("iconOnly", true);
    button->setFixedSize(34, 34);
    button->setCursor(Qt::PointingHandCursor);
    return button;
  };

  auto *sidebar = new QFrame;
  sidebar->setObjectName("sidebar");
  sidebar->setFixedWidth(220);
  root->addWidget(sidebar);
  auto *side = new QVBoxLayout(sidebar);
  side->setContentsMargins(12, 12, 12, 10);
  side->setSpacing(10);
  auto *brandRow = new QHBoxLayout;
  brandRow->setSpacing(8);
  auto *brand = new QLabel("OSMClient");
  brand->setObjectName("brandName");
  brandRow->addWidget(brand, 1);
  m_collapse = iconButton("sidebar", "sidebarToggle");
  brandRow->addWidget(m_collapse);
  side->addLayout(brandRow);
  m_search = new AssetSearch;
  m_search->setObjectName("assetSearch");
  side->addWidget(m_search);
  m_searchTimer = new QTimer(this);
  m_searchTimer->setSingleShot(true);
  m_searchTimer->setInterval(250);
  connect(m_search, &QLineEdit::textChanged, m_searchTimer, QOverload<>::of(&QTimer::start));
  connect(m_searchTimer, &QTimer::timeout, this, [this] { refresh(true); });
  connect(m_search, &QLineEdit::returnPressed, this, [this] {
    m_searchTimer->stop();
    refresh(true);
  });
  side->addSpacing(8);
  m_catalogLabel = new QLabel;
  m_catalogLabel->setProperty("role", "caption");
  m_catalogLabel->setContentsMargins(10, 0, 0, 0);
  side->addWidget(m_catalogLabel);
  m_categories = new QListWidget;
  m_categories->setObjectName("categories");
  m_categories->setFrameShape(QFrame::NoFrame);
  m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_categories->addItems(categoryLabels());
  const QStringList categoryIcons{"terminal", "windows", "database", "network", "browser", "box", "star"};
  for (int row = 0; row < m_categories->count(); ++row)
    m_categories->item(row)->setIcon(ClientTheme::icon(categoryIcons[row]));
  m_categories->setIconSize(QSize(20, 20));
  m_categories->setCurrentRow(0);
  side->addWidget(m_categories, 1);
  connect(m_categories, &QListWidget::currentRowChanged, this, [this] { refresh(true); });

  auto *profilePanel = new QFrame;
  profilePanel->setObjectName("profilePanel");
  auto *profileRow = new QHBoxLayout(profilePanel);
  profileRow->setContentsMargins(2, 12, 2, 2);
  profileRow->setSpacing(9);
  auto *avatar = iconButton("user", "avatar");
  profileRow->addWidget(avatar);
  connect(avatar, &QPushButton::clicked, this, [this, avatar] {
    QMenu menu(this);
    menu.addAction(text("切换站点", "Switch site"), m_switcher, &QPushButton::click);
    if (m_service->authenticated()) {
      menu.addSeparator();
      menu.addAction(text("退出登录", "Sign out"), m_service, &AppService::logout);
    }
    menu.exec(avatar->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu.sizeHint().height()));
  });
  m_profile = new QLabel;
  m_profile->setTextFormat(Qt::PlainText);
  m_profile->setObjectName("profileLabel");
  m_profile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_profile->setProperty("role", "caption");
  profileRow->addWidget(m_profile, 1);
  m_logout = iconButton("logout", "signOut");
  profileRow->addWidget(m_logout);
  connect(m_logout, &QPushButton::clicked, m_service, &AppService::logout);
  side->addWidget(profilePanel);
  connect(m_collapse, &QPushButton::clicked, this, [this, sidebar, brand] {
    m_collapsed = !m_collapsed;
    sidebar->setFixedWidth(m_collapsed ? 75 : 220);
    brand->setVisible(!m_collapsed);
    m_search->setVisible(!m_collapsed);
    m_catalogLabel->setVisible(!m_collapsed);
    m_profile->setVisible(!m_collapsed);
    QSettings().setValue("appearance/sidebarCollapsed", m_collapsed);
    retranslateUi();
  });

  auto *workspace = new QWidget;
  auto *body = new QVBoxLayout(workspace);
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);
  root->addWidget(workspace, 1);
  auto *topbar = new QFrame;
  topbar->setObjectName("topbar");
  topbar->setFixedHeight(56);
  auto *top = new QHBoxLayout(topbar);
  top->setContentsMargins(22, 0, 16, 0);
  top->setSpacing(12);
  m_organizations = new ClientTheme::ComboBox;
  m_organizations->setObjectName("organizationSelector");
  m_organizations->setMaximumWidth(180);
  top->addWidget(m_organizations);
  connect(m_organizations, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (index < 0) return;
    m_connectionQueue.clear();
    m_pendingAsset = {};
    m_waitingDetail = {};
    m_methodsLoaded = false;
    m_methods = {};
    m_favorites.clear();
    m_pendingFavorites.clear();
    m_connectionInFlight = false;
    m_service->setOrg(m_organizations->itemData(index).toString());
    m_service->metadata();
    refresh(true);
  });
  top->addStretch();
  m_switcher = new QPushButton;
  m_switcher->setObjectName("siteSwitcher");
  m_switcher->setProperty("quiet", true);
  m_switcher->setIcon(ClientTheme::icon("globe"));
  m_switcher->setIconSize(QSize(16, 16));
  top->addWidget(m_switcher);
  connect(m_switcher, &QPushButton::clicked, this, [this] {
    QMenu menu(this);
    for (const QString &site : QSettings().value("recentSites").toStringList()) {
      auto *action = menu.addAction(site);
      connect(action, &QAction::triggered, this, [this, site] { switchSite(site); });
    }
    menu.addSeparator();
    menu.addAction(text("添加站点…", "Add site…"), this, [this] {
      m_content->setCurrentIndex(0);
      m_sites->setCurrentText(QString());
      m_sites->setFocus();
    });
    menu.addAction(text("清空最近站点", "Clear recent sites"), this, [this] {
      QSettings().setValue("recentSites", QStringList());
      m_sites->clear();
    });
    menu.exec(m_switcher->mapToGlobal(QPoint(0, m_switcher->height())));
  });
  auto *tools = new QWidget;
  tools->setObjectName("assetToolbar");
  auto *toolsRow = new QHBoxLayout(tools);
  toolsRow->setContentsMargins(0, 0, 0, 0);
  toolsRow->setSpacing(4);
  m_refreshButton = iconButton("refresh", "refreshAssets");
  toolsRow->addWidget(m_refreshButton);
  connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
  m_order = new ClientTheme::ComboBox(tools);
  m_order->setObjectName("assetOrder");
  m_order->hide();
  connect(m_order, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    QSettings().setValue("assets/order", m_order->currentData());
    refresh(true);
  });
  m_sortButton = iconButton("sort", "sortAssets");
  toolsRow->addWidget(m_sortButton);
  connect(m_sortButton, &QPushButton::clicked, this, [this] {
    QMenu menu(this);
    for (int i = 0; i < m_order->count(); ++i) {
      auto *action = menu.addAction(m_order->itemText(i));
      action->setCheckable(true);
      action->setChecked(i == m_order->currentIndex());
      connect(action, &QAction::triggered, this, [this, i] { m_order->setCurrentIndex(i); });
    }
    menu.exec(m_sortButton->mapToGlobal(QPoint(0, m_sortButton->height())));
  });
  m_layoutButton = iconButton("grid", "layoutToggle");
  toolsRow->addWidget(m_layoutButton);
  connect(m_layoutButton, &QPushButton::clicked, this, [this] {
    QSet<QString> selected;
    for (const auto &asset : selectedAssets()) selected.insert(asset["id"].toString());
    m_views->setCurrentIndex(1 - m_views->currentIndex());
    // Transfer selection before retranslation renders the newly active view.
    {
      const QSignalBlocker gridBlock(m_grid), tableBlock(m_table);
      for (int row = 0; row < m_grid->count(); ++row) {
        const bool chosen = selected.contains(m_assets.at(row).toObject()["id"].toString());
        m_grid->item(row)->setSelected(chosen);
        m_table->selectionModel()->select(m_table->model()->index(row, 0),
            (chosen ? QItemSelectionModel::Select : QItemSelectionModel::Deselect) | QItemSelectionModel::Rows);
      }
    }
    QSettings().setValue("assets/layout", m_views->currentIndex());
    retranslateUi();
  });
  m_settingsButton = iconButton("settings", "openSettings");
  toolsRow->addWidget(m_settingsButton);
  connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);
  top->addWidget(tools);
  body->addWidget(topbar);
  m_content = new QStackedWidget;
  m_content->setObjectName("contentPages");
  body->addWidget(m_content, 1);

  auto *loginPage = new QWidget;
  auto *loginLayout = new QVBoxLayout(loginPage);
  loginLayout->setSizeConstraint(QLayout::SetMinimumSize);
  loginLayout->setContentsMargins(24, 24, 24, 24);
  loginLayout->addStretch();
  auto *card = new QFrame;
  card->setObjectName("loginCard");
  card->setProperty("surface", true);
  card->setMinimumWidth(350);
  card->setMaximumWidth(420);
  auto *cl = new QVBoxLayout(card);
  cl->setContentsMargins(32, 30, 32, 28);
  cl->setSpacing(14);
  auto *logo = new QLabel;
  logo->setObjectName("loginLogo");
  logo->setPixmap(QIcon(":/icons/icon.png").pixmap(56, 56));
  // A pixmap label can otherwise shrink below its image height when login
  // feedback grows. Reserve logical pixels; Qt handles the display scale.
  logo->setFixedSize(56, 56);
  logo->setAlignment(Qt::AlignCenter);
  cl->addWidget(logo, 0, Qt::AlignHCenter);
  m_welcome = new QLabel;
  m_welcome->setObjectName("welcomeTitle");
  m_welcome->setAlignment(Qt::AlignCenter);
  cl->addWidget(m_welcome);
  m_loginSubtitle = new QLabel;
  m_loginSubtitle->setProperty("muted", true);
  m_loginSubtitle->setAlignment(Qt::AlignCenter);
  m_loginSubtitle->setWordWrap(true);
  cl->addWidget(m_loginSubtitle);
  cl->addSpacing(10);
  m_siteLabel = new QLabel;
  m_siteLabel->setProperty("role", "caption");
  cl->addWidget(m_siteLabel);
  m_sites = new ClientTheme::ComboBox;
  m_sites->setObjectName("loginSite");
  m_sites->setEditable(true);
  m_sites->addItems(QSettings().value("recentSites").toStringList());
  m_sites->setCurrentText(QSettings().value("lastSite").toString());
  m_sites->lineEdit()->setPlaceholderText("https://osm.example.com");
  cl->addWidget(m_sites);
  connect(m_sites->lineEdit(), &QLineEdit::returnPressed, this, &MainWindow::login);
  connect(m_sites->lineEdit(), &QLineEdit::textChanged, this, [this] {
    m_loginMessage->clear();
    m_loginMessage->hide();
  });
  m_loginButton = new QPushButton;
  m_loginButton->setObjectName("loginButton");
  m_loginButton->setProperty("primary", true);
  m_loginButton->setMinimumHeight(26);
  cl->addWidget(m_loginButton);
  connect(m_loginButton, &QPushButton::clicked, this, &MainWindow::login);
  m_cancelLogin = new QPushButton;
  m_cancelLogin->setObjectName("cancelLoginButton");
  m_cancelLogin->setProperty("quiet", true);
  m_cancelLogin->hide();
  cl->addWidget(m_cancelLogin);
  connect(m_cancelLogin, &QPushButton::clicked, this, [this] {
    m_service->cancelLogin();
    m_authorizationUrl = QUrl();
    updateSessionUi();
  });
  m_loginMessage = new QLabel;
  m_loginMessage->setObjectName("loginMessage");
  m_loginMessage->setTextFormat(Qt::PlainText);
  m_loginMessage->setProperty("role", "validation");
  m_loginMessage->setWordWrap(true);
  m_loginMessage->hide();
  cl->addWidget(m_loginMessage);
  m_authTools = new QWidget;
  auto *browserTools = new QHBoxLayout(m_authTools);
  browserTools->setContentsMargins(0, 0, 0, 0);
  browserTools->setSpacing(6);
  m_reopen = new QPushButton;
  m_copyLogin = new QPushButton;
  m_reopen->setProperty("quiet", true);
  m_copyLogin->setProperty("quiet", true);
  browserTools->addWidget(m_reopen);
  browserTools->addWidget(m_copyLogin);
  cl->addWidget(m_authTools);
  m_authTools->hide();
  connect(m_reopen, &QPushButton::clicked, this, [this] {
    if (m_authorizationUrl.isValid()) QDesktopServices::openUrl(m_authorizationUrl);
  });
  connect(m_copyLogin, &QPushButton::clicked, this, [this] {
    if (m_authorizationUrl.isValid()) QApplication::clipboard()->setText(m_authorizationUrl.toString());
  });
  m_loginHint = new QLabel;
  m_loginHint->setObjectName("loginHint");
  m_loginHint->setProperty("role", "caption");
  m_loginHint->setAlignment(Qt::AlignCenter);
  cl->addWidget(m_loginHint);
  loginLayout->addWidget(card, 0, Qt::AlignHCenter);
  loginLayout->addStretch();
  auto *loginScroll = new QScrollArea;
  loginScroll->setObjectName("loginScroll");
  loginScroll->setFrameShape(QFrame::NoFrame);
  loginScroll->setWidgetResizable(true);
  loginScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  loginScroll->setWidget(loginPage);
  m_content->addWidget(loginScroll);

  auto *assetPage = new QWidget;
  auto *al = new QVBoxLayout(assetPage);
  al->setContentsMargins(16, 14, 16, 10);
  al->setSpacing(10);
  m_views = new QStackedWidget;
  m_views->setObjectName("assetViews");
  al->addWidget(m_views, 1);
  m_grid = new AssetGrid;
  m_grid->setObjectName("assetGrid");
  m_grid->setViewMode(QListView::IconMode);
  m_grid->setResizeMode(QListView::Adjust);
  m_grid->setMovement(QListView::Static);
  m_grid->setUniformItemSizes(true);
  m_grid->setSpacing(0);
  m_grid->setMouseTracking(true);
  m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_grid->setItemDelegate(new AssetDelegate(m_grid, [this](const QJsonObject &asset) { requestConnection(asset); }));
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  m_views->addWidget(m_grid);
  connect(m_grid, &QListWidget::itemDoubleClicked, this, [this] { requestConnection(selectedAsset(true)); });
  connect(m_grid, &QWidget::customContextMenuRequested, this, [this](QPoint pos) { showContextMenu(pos, true); });
  auto *assetTable = new AssetTable;
  m_table = assetTable;
  m_table->setObjectName("assetTable");
  m_table->setColumnCount(7);
  m_table->setItemDelegate(new AssetTableDelegate(assetTable));
  m_table->setMouseTracking(true);
  m_table->setWordWrap(false);
  m_table->setTextElideMode(Qt::ElideRight);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->verticalHeader()->hide();
  m_table->setShowGrid(false);
  m_table->setAlternatingRowColors(false);
  m_table->setIconSize(QSize(22, 22));
  m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  assetTable->updateColumns();
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);
  m_views->addWidget(m_table);
  connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int column) {
    if (column != 0) requestConnection(selectedAsset(false));
  });
  connect(m_table, &QWidget::customContextMenuRequested, this, [this](QPoint pos) { showContextMenu(pos, false); });
  m_views->setCurrentIndex(QSettings().value("assets/layout", 0).toInt() == 1 ? 1 : 0);
  m_empty = new QLabel;
  m_empty->setObjectName("assetEmptyState");
  m_empty->setAlignment(Qt::AlignCenter);
  m_empty->setProperty("muted", true);
  al->addWidget(m_empty, 1);
  auto *footer = new QHBoxLayout;
  footer->setContentsMargins(6, 0, 6, 0);
  footer->setSpacing(8);
  m_pageLabel = new QLabel;
  m_pageLabel->setObjectName("assetPageLabel");
  m_pageLabel->setProperty("role", "caption");
  footer->addWidget(m_pageLabel);
  footer->addStretch();
  m_connectButton = new QPushButton;
  m_connectButton->setObjectName("connectSelected");
  m_connectButton->setProperty("soft", true);
  footer->addWidget(m_connectButton);
  connect(m_connectButton, &QPushButton::clicked, this, [this] {
    if (m_connectionInFlight || !m_pendingAsset.isEmpty() || !m_connectionQueue.isEmpty()) return;
    m_connectionQueue = selectedAssets();
    processNextConnection();
  });
  m_pageSize = new ClientTheme::ComboBox;
  m_pageSize->setObjectName("pageSize");
  m_pageSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  m_pageSize->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  for (int value : {20, 50, 100}) m_pageSize->addItem(QString::number(value), value);
  m_pageSize->setCurrentIndex(qMax(0, m_pageSize->findData(QSettings().value("assets/pageSize", 20))));
  footer->addWidget(m_pageSize);
  connect(m_pageSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
    QSettings().setValue("assets/pageSize", m_pageSize->currentData());
    refresh(true);
  });
  m_previous = iconButton("chevron-left", "previousPage");
  m_next = iconButton("chevron-right", "nextPage");
  footer->addWidget(m_previous);
  footer->addWidget(m_next);
  connect(m_previous, &QPushButton::clicked, this, [this] {
    m_offset = qMax(0, m_offset - m_pageSize->currentData().toInt());
    refresh();
  });
  connect(m_next, &QPushButton::clicked, this, [this] {
    m_offset += m_pageSize->currentData().toInt();
    refresh();
  });
  al->addLayout(footer);
  m_content->addWidget(assetPage);
  m_restorePage = new QWidget;
  m_restorePage->setObjectName("restoreSessionPage");
  auto *restoreLayout = new QVBoxLayout(m_restorePage);
  restoreLayout->setContentsMargins(24, 24, 24, 24);
  restoreLayout->setSpacing(18);
  restoreLayout->addStretch();
  restoreLayout->addWidget(new RestoreSpinner, 0, Qt::AlignHCenter);
  m_restoreTitle = new QLabel;
  m_restoreTitle->setObjectName("restoreSessionTitle");
  m_restoreTitle->setProperty("muted", true);
  m_restoreTitle->setAlignment(Qt::AlignCenter);
  restoreLayout->addWidget(m_restoreTitle);
  m_cancelRestore = new QPushButton;
  m_cancelRestore->setObjectName("cancelRestoreButton");
  m_cancelRestore->setProperty("ghost", true);
  restoreLayout->addWidget(m_cancelRestore, 0, Qt::AlignHCenter);
  restoreLayout->addStretch();
  m_content->addWidget(m_restorePage);
  connect(m_cancelRestore, &QPushButton::clicked, this, [this] {
    m_restoring = false;
    m_service->cancelLogin();
    updateSessionUi();
    updateLoginState();
  });
  connect(m_grid, &QListWidget::itemSelectionChanged, this, &MainWindow::updateSelectionUi);
  connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateSelectionUi);
  statusBar()->setSizeGripEnabled(false);
  statusBar()->hide();
  connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString &message) { statusBar()->setVisible(!message.isEmpty()); });
  retranslateUi();
  if (QSettings().value("appearance/sidebarCollapsed", false).toBool()) m_collapse->click();
}
void MainWindow::retranslateUi() {
  m_restoreTitle->setText(text("正在恢复登录…", "Restoring your session…"));
  m_cancelRestore->setText(text("返回登录", "Back to sign in"));
  m_switcher->setText(text("切换站点", "Switch site"));
  auto *avatar = findChild<QPushButton *>("avatar");
  avatar->setToolTip(text("账户与站点", "Account and sites"));
  avatar->setAccessibleName(avatar->toolTip());
  m_pageSize->setAccessibleName(text("每页条数", "Assets per page"));
  m_welcome->setText(text("欢迎使用 OSMClient", "Welcome to OSMClient"));
  m_loginSubtitle->setText(text("连接工作空间，安全访问你的授权资产", "Connect to your workspace and access your assets."));
  m_siteLabel->setText(text("站点地址", "Workspace URL"));
  m_loginHint->setText(text("使用默认浏览器完成登录", "Continue securely in your default browser"));
  m_reopen->setText(text("重新打开浏览器", "Reopen browser"));
  m_copyLogin->setText(text("复制登录链接", "Copy sign-in link"));
  m_catalogLabel->setText(text("资源目录", "RESOURCES"));
  const auto labels = categoryLabels();
  for (int i = 0; i < labels.size(); ++i) {
    m_categories->item(i)->setText(m_collapsed ? QString() : labels[i]);
    m_categories->item(i)->setToolTip(labels[i]);
    m_categories->item(i)->setSizeHint(QSize(m_collapsed ? 48 : 180, 40));
  }
  m_search->setPlaceholderText(text("搜索资产", "Search assets"));
  m_search->setAccessibleName(m_search->placeholderText());
  m_search->findChild<QAction *>("clearAssetSearch")->setText(text("清空搜索", "Clear search"));
  m_search->setToolTip(text("搜索当前分类资产", "Search assets in this category") +
                      " · " + QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText));
  const QList<QPair<QPushButton *, QString>> tools{
    {m_settingsButton, text("设置", "Settings")},
    {m_logout, text("退出登录", "Sign out")},
    {m_refreshButton, text("刷新资产", "Refresh assets")},
    {m_sortButton, text("排序", "Sort")},
    {m_layoutButton, m_views->currentIndex() == 0 ? text("切换为表格视图", "Switch to table view") : text("切换为卡片视图", "Switch to card view")},
    {m_collapse, m_collapsed ? text("展开侧栏", "Expand sidebar") : text("收起侧栏", "Collapse sidebar")},
    {m_previous, text("上一页", "Previous page")},
    {m_next, text("下一页", "Next page")}
  };
  for (const auto &tool : tools) {
    tool.first->setText(QString());
    tool.first->setToolTip(tool.second);
    tool.first->setAccessibleName(tool.second);
  }
  m_layoutButton->setIcon(ClientTheme::icon(m_views->currentIndex() == 0 ? "list" : "grid"));
  m_loginButton->setText(text("浏览器登录", "Sign in with browser"));
  m_cancelLogin->setText(text("取消登录", "Cancel sign-in"));
  m_table->setHorizontalHeaderLabels(
      {text("收藏", "Favorite"), text("资产名称", "Asset"),
       text("地址", "Address"), text("账号", "Account"),
       text("协议", "Protocol"), text("平台", "Platform"),
       text("操作", "Actions")});
  for (int column = 0; column < m_table->columnCount(); ++column)
    m_table->horizontalHeaderItem(column)->setTextAlignment(
        (column == 0 || column == 4 || column == 6 ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter);
  static_cast<AssetTable *>(m_table)->updateColumns();
  {
    QSignalBlocker blocker(m_order);
    QString order = QSettings().value("assets/order", "name").toString();
    m_order->clear();
    m_order->addItem(text("名称升序", "Name ascending"), "name");
    m_order->addItem(text("名称降序", "Name descending"), "-name");
    m_order->addItem(text("地址升序", "Address ascending"), "address");
    m_order->addItem(text("地址降序", "Address descending"), "-address");
    m_order->addItem(text("最近更新", "Recently updated"), "-date_updated");
    m_order->addItem(text("最早更新", "Oldest updated"), "date_updated");
    m_order->setCurrentIndex(qMax(0, m_order->findData(order)));
  }
  updateSessionUi();
  renderAssets();
}
void MainWindow::login() {
  if (m_service->loginPending())
    return;
  const QString site = m_sites->currentText().trimmed();
  if (site.isEmpty()) {
    m_loginMessage->setText(text("请输入站点地址。", "Enter a site URL."));
    m_loginMessage->show();
    return;
  }
  m_authorizationUrl = QUrl();
  m_authTools->hide();
  m_service->login(site, QSettings().value("auth/loopback", false).toBool());
}
void MainWindow::refresh(bool reset) {
  if (!m_service->authenticated())
    return;
  if (reset)
    m_offset = 0;
  m_loading = true;
  m_refreshButton->setEnabled(false);
  m_previous->setEnabled(false);
  m_next->setEnabled(false);
  m_service->assets(categoryKeys.value(m_categories->currentRow(), "linux"),
                    m_search->text(), m_offset,
                    m_pageSize->currentData().toInt(),
                    m_order->currentData().toString());
}
QString MainWindow::scope() const {
  return m_service->site() + "\n" + m_profileData["id"].toString() + "\n" +
         m_service->org();
}
void MainWindow::setFavorite(const QString &id, bool enabled) {
  if (id.isEmpty() || m_pendingFavorites.contains(id)) return;
  m_pendingFavorites.insert(id);
  for (auto *button : m_table->findChildren<QPushButton *>("assetFavorite"))
    if (button->property("assetId").toString() == id) button->setEnabled(false);
  m_service->favorite(id, enabled);
}
void MainWindow::renderAssets() {
  if (!m_grid || !m_table)
    return;
  QSet<QString> selected;
  for (const auto &a : selectedAssets())
    selected.insert(a["id"].toString());
  const QSignalBlocker gridBlock(m_grid), tableBlock(m_table);
  m_grid->clear();
  m_table->setRowCount(0);
  for (const auto &value : m_assets) {
    auto asset = value.toObject();
    const QString id = asset["id"].toString();
    auto saved = ConnectionDialog::saved(scope(), id);
    QString account = saved["accountLabel"].toString();
    QString protocol = saved["protocol"].toString();
    if (account.isEmpty()) {
      auto accounts = asset["permed_accounts"].toArray();
      if (!accounts.isEmpty())
        account = accounts[0].toObject()["name"].toString();
    }
    if (protocol.isEmpty()) {
      auto protocols = asset["permed_protocols"].toArray();
      if (!protocols.isEmpty())
        protocol = protocols[0].toObject()["name"].toString();
    }
    const bool favorite =
        m_favorites.contains(id) ||
        categoryKeys.value(m_categories->currentRow()) == "favorite";
    auto *item = new QListWidgetItem(
        (favorite ? "★  " : "") + asset["name"].toString(), m_grid);
    item->setData(Qt::UserRole, asset.toVariantMap());
    const QString displayAddress = ConnectionDialog::displayAddress(asset, saved);
    item->setData(Qt::UserRole + 1, displayAddress);
    item->setData(Qt::UserRole + 2,
                  account.isEmpty()
                      ? text("选择账号后连接", "Select an account to connect")
                      : account);
    item->setData(Qt::UserRole + 3, protocol.toUpper());
    item->setToolTip(assetToolTip(asset, saved));
    item->setSelected(selected.contains(id));
    int row = m_table->rowCount();
    m_table->insertRow(row);
    const QStringList fields = {
        "",                           asset["name"].toString(),
        asset["address"].toString(), account,
        protocol.toUpper(),          displayName(asset["platform"])};
    for (int column = 0; column < fields.size(); ++column) {
      const bool missing = column > 0 && fields[column].isEmpty();
      auto *cell = new QTableWidgetItem(missing ? QStringLiteral("—") : fields[column]);
      cell->setTextAlignment((column == 0 || column == 4 ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter);
      if (missing || !asset.value("is_active").toBool(true)) cell->setForeground(ClientTheme::colors().muted);
      if (column == 1)
        cell->setIcon(assetIcon(asset));
      cell->setData(Qt::UserRole, asset.toVariantMap());
      cell->setToolTip(fields[column]);
      m_table->setItem(row, column, cell);
    }
    auto *favoriteButton = new QPushButton(m_table);
    favoriteButton->setObjectName("assetFavorite");
    favoriteButton->setProperty("assetId", id);
    favoriteButton->setProperty("favorite", favorite);
    favoriteButton->setProperty("quiet", true);
    favoriteButton->setIcon(ClientTheme::icon(favorite ? "star-filled" : "star-outline"));
    favoriteButton->setIconSize(QSize(18, 18));
    favoriteButton->setFixedSize(28, 28);
    favoriteButton->setCursor(Qt::PointingHandCursor);
    const QString favoriteLabel = favorite ? text("取消收藏", "Remove favorite") : text("收藏", "Add favorite");
    favoriteButton->setToolTip(favoriteLabel);
    favoriteButton->setAccessibleName(favoriteLabel);
    favoriteButton->setEnabled(!id.isEmpty() && !m_pendingFavorites.contains(id));
    auto *favoriteCell = new QWidget;
    auto *favoriteLayout = new QHBoxLayout(favoriteCell);
    favoriteLayout->setContentsMargins(0, 0, 0, 0);
    favoriteLayout->addWidget(favoriteButton, 0, Qt::AlignCenter);
    m_table->setCellWidget(row, 0, favoriteCell);
    connect(favoriteButton, &QPushButton::clicked, this,
            [this, id, favorite] { setFavorite(id, !favorite); });
    if (selected.contains(id))
      m_table->selectionModel()->select(m_table->model()->index(row, 0),
          QItemSelectionModel::Select | QItemSelectionModel::Rows);
    auto *connectButton = new QPushButton(text("连接", "Connect"), m_table);
    connectButton->setEnabled(asset.value("is_active").toBool(true));
    connectButton->setProperty("soft", true);
    connectButton->ensurePolished();
    const QSize buttonSize = connectButton->sizeHint().expandedTo(QSize(64, 30));
    connectButton->setFixedSize(buttonSize);
    m_table->setRowHeight(row, qMax(48, qMax(buttonSize.height() + 16, m_table->fontMetrics().height() + 20)));
    auto *actionCell = new QWidget;
    auto *actionLayout = new QHBoxLayout(actionCell);
    actionLayout->setContentsMargins(8, 4, 8, 4);
    actionLayout->addWidget(connectButton, 0, Qt::AlignCenter);
    m_table->setCellWidget(row, 6, actionCell);
    connect(connectButton, &QPushButton::clicked, this,
            [this, asset] { requestConnection(asset); });
  }
  m_empty->setText(
      text("没有符合条件的授权资产", "No matching authorized assets"));
  const bool empty = m_assets.isEmpty() && m_service->authenticated();
  m_views->setVisible(!empty);
  m_empty->setVisible(empty);
  const int size = m_pageSize ? m_pageSize->currentData().toInt() : 20;
  m_pageLabel->setText(text("共 %1 项 · 第 %2 页", "%1 assets · Page %2")
                           .arg(m_count)
                           .arg(m_offset / qMax(1, size) + 1));
  m_previous->setEnabled(!m_loading && m_offset > 0);
  m_next->setEnabled(!m_loading && m_offset + size < m_count);
  updateSelectionUi();
}
void MainWindow::updateSelectionUi() {
  const int selected = selectedAssets().size();
  m_connectButton->setVisible(selected > 0);
  m_connectButton->setEnabled(m_service->authenticated() && selected > 0);
  m_connectButton->setText(text("连接所选 (%1)", "Connect selected (%1)").arg(selected));
}
QJsonObject MainWindow::selectedAsset(bool grid) const {
  if (grid) {
    auto *item = m_grid->currentItem();
    return item ? QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap())
                : QJsonObject();
  }
  auto *item = m_table->item(m_table->currentRow(), 0);
  return item ? QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap())
              : QJsonObject();
}
QList<QJsonObject> MainWindow::selectedAssets() const {
  QList<QJsonObject> out;
  if (!m_views)
    return out;
  if (m_views->currentIndex() == 0) {
    for (auto *item : m_grid->selectedItems())
      out << QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
  } else {
    for (const auto &index : m_table->selectionModel()->selectedRows()) {
      auto *item = m_table->item(index.row(), 0);
      if (item)
        out << QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
    }
  }
  return out;
}
void MainWindow::showContextMenu(const QPoint &pos, bool grid) {
  QJsonObject asset;
  if (grid) {
    auto *item = m_grid->itemAt(pos);
    if (!item)
      return;
    m_grid->setCurrentItem(item);
    asset = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
  } else {
    auto *item = m_table->itemAt(pos);
    if (!item)
      return;
    m_table->setCurrentCell(item->row(), 0);
    asset = QJsonObject::fromVariantMap(item->data(Qt::UserRole).toMap());
  }
  QMenu menu(this);
  menu.addAction(text("连接", "Connect"), this,
                 [this, asset] { requestConnection(asset); });
  menu.addAction(text("编辑连接", "Edit connection"), this,
                 [this, asset] { requestConnection(asset, true); });
  menu.setProperty("protocolMenuActive", true);
  connect(&menu, &QMenu::aboutToHide, &menu, [&menu] { menu.setProperty("protocolMenuActive", false); });
  auto *protocols = menu.addMenu(text("使用协议连接", "Connect using"));
  protocols->setObjectName("protocolMenu");
  protocols->setEnabled(false);
  if (asset.value("is_active").toBool(true)) loadProtocolMenu(protocols, asset);
  connect(m_service, &AppService::contextChanged, &menu, &QMenu::close);
  connect(m_service, &AppService::methodsLookupStarted, &menu, &QMenu::close);
  menu.addSeparator();
  const QString id = asset["id"].toString();
  bool favorite = m_favorites.contains(id) ||
                  categoryKeys.value(m_categories->currentRow()) == "favorite";
  menu.addAction(favorite ? text("取消收藏", "Remove favorite")
                          : text("收藏", "Add favorite"),
                 this,
                 [this, id, favorite] { setFavorite(id, !favorite); })
      ->setEnabled(!m_pendingFavorites.contains(id));
  menu.addAction(text("重命名", "Rename"), this, [this, asset, id] {
    bool ok = false;
    QString name =
        QInputDialog::getText(this, text("重命名资产", "Rename asset"),
                              text("显示名称", "Display name"),
                              QLineEdit::Normal, asset["name"].toString(), &ok);
    if (ok && !name.trimmed().isEmpty())
      m_service->rename(id, name.trimmed());
  });
  menu.addAction(text("复制地址", "Copy address"), this, [asset] {
    QApplication::clipboard()->setText(asset["address"].toString());
  });
  const QString menuScope = scope();
  const quint64 menuContext = m_service->contextRevision();
  menu.exec((grid ? static_cast<QWidget *>(m_grid->viewport())
                  : static_cast<QWidget *>(m_table->viewport()))
                ->mapToGlobal(pos));
  if (menu.property("protocolRetry").toBool() && scope() == menuScope && m_service->contextRevision() == menuContext) {
    const QString assetId = asset.value("id").toString();
    QTimer::singleShot(0, this, [this, pos, grid, assetId, menuScope, menuContext] {
      if (scope() != menuScope || m_service->contextRevision() != menuContext) return;
      const auto *item = grid ? m_grid->itemAt(pos) : nullptr;
      const auto *cell = !grid ? m_table->itemAt(pos) : nullptr;
      const QVariant data = item ? item->data(Qt::UserRole) : cell ? cell->data(Qt::UserRole) : QVariant();
      if (data.toMap().value("id").toString() == assetId) showContextMenu(pos, grid);
    });
  }
}
void MainWindow::loadProtocolMenu(QMenu *protocols, const QJsonObject &asset) {
  struct Lookup { QJsonObject detail, methods; bool detailDone = false, methodsDone = false; QString error; QElapsedTimer methodsAge; quint64 methodsRevision = 0; };
  auto state = std::make_shared<Lookup>();
  QPointer<QMenu> menu(protocols), parent(qobject_cast<QMenu *>(protocols->parentWidget()));
  const QString currentScope = scope();
  const quint64 context = m_service->contextRevision();
  const auto update = [this, menu, parent, state, asset, currentScope, context] {
    if (!menu || !parent || !parent->property("protocolMenuActive").toBool() || scope() != currentScope || m_service->contextRevision() != context) return;
    if (!state->error.isEmpty()) {
      menu->clear();
      menu->addAction(state->error)->setEnabled(false);
      menu->addAction(text("重试", "Retry"), parent.data(), [parent] { if (parent) parent->setProperty("protocolRetry", true); });
      menu->setEnabled(true);
      return;
    }
    if (!state->detailDone || !state->methodsDone) return;
    if (!state->methodsAge.isValid() || state->methodsAge.elapsed() >= 30000 || state->methodsRevision != m_service->methodsRevision()) {
      state->error = text("连接方式已过期", "Connection methods expired");
      menu->clear();
      menu->addAction(state->error)->setEnabled(false);
      menu->addAction(text("重试", "Retry"), parent.data(), [parent] { if (parent) parent->setProperty("protocolRetry", true); });
      menu->setEnabled(true);
      return;
    }
    menu->clear();
    const QJsonArray authorized = state->detail.value("permed_protocols").toArray();
    const QJsonArray accounts = state->detail.value("permed_accounts").toArray();
    bool hasAccount = false;
    for (const auto &entry : accounts) {
      const auto account = entry.toObject();
      const QString alias = account.value("alias").toString();
      if (!account.value("id").toString().isEmpty() || alias == "@INPUT" || alias == "@USER") { hasAccount = true; break; }
    }
    QSet<QString> unique;
    for (const auto &entry : authorized) {
      const QString name = entry.toObject().value("name").toString().trimmed();
      if (name.isEmpty() || name == "winrm" || unique.contains(name)) continue;
      for (const auto &value : state->methods.value(name).toArray())
        if (ConnectionDialog::isClientMethod(name, value.toObject())) { unique.insert(name); break; }
    }
    QStringList names = unique.values();
    names.sort(Qt::CaseInsensitive);
    if (!hasAccount || names.isEmpty()) {
      const QString reason = !hasAccount ? text("没有可用的授权账号", "No authorized account available")
          : authorized.isEmpty() ? text("没有授权协议", "No authorized protocols")
          : text("没有可用的客户端连接方式", "No client connection method available");
      menu->addAction(reason)->setEnabled(false);
    } else {
      for (const QString &name : names) {
        menu->addAction(name.toUpper(), this, [this, asset, state, name, currentScope, context, parent] {
          if (scope() != currentScope || m_service->contextRevision() != context || !m_service->authenticated() || !asset.value("is_active").toBool(true) || m_connectionInFlight || !m_pendingAsset.isEmpty()) return;
          if (state->methodsAge.elapsed() >= 30000 || state->methodsRevision != m_service->methodsRevision()) {
            if (parent) parent->setProperty("protocolRetry", true);
            return;
          }
          m_methods = state->methods;
          m_methodsLoaded = true;
          m_methodsAge = state->methodsAge;
          m_methodsCacheRevision = state->methodsRevision;
          m_pendingAsset = asset;
          m_pendingProtocol = name;
          m_editOnly = false;
          m_forceConnectionDialog = true;
          consumeDetails(asset.value("id").toString(), state->detail);
        });
      }
    }
    menu->setEnabled(true);
    const int remaining = qMax(0, 30000 - int(state->methodsAge.elapsed()));
    QTimer::singleShot(remaining + 1, menu.data(), [menu, parent] {
      if (!menu || !parent || !parent->property("protocolMenuActive").toBool()) return;
      menu->clear();
      menu->addAction(text("连接方式已过期", "Connection methods expired"))->setEnabled(false);
      menu->addAction(text("重试", "Retry"), parent.data(), [parent] { if (parent) parent->setProperty("protocolRetry", true); });
    });
  };
  m_service->lookupDetail(asset.value("id").toString(), [state, update](QJsonValue result, QString error) {
    state->detailDone = true; state->detail = result.toObject();
    if (!error.isEmpty()) state->error = text("获取资产授权失败", "Failed to load asset permissions");
    update();
  });
  if (m_methodsLoaded && m_methodsAge.isValid() && m_methodsAge.elapsed() < 30000 && m_methodsCacheRevision == m_service->methodsRevision()) {
    state->methodsDone = true; state->methods = m_methods; state->methodsAge = m_methodsAge; state->methodsRevision = m_methodsCacheRevision; update();
  } else {
    m_service->lookupMethods([this, menu, parent, state, update, currentScope, context](QJsonValue result, QString error) {
      if (!menu || !parent || !parent->property("protocolMenuActive").toBool() || scope() != currentScope || m_service->contextRevision() != context) return;
      state->methodsDone = true; state->methods = result.toObject();
      if (!error.isEmpty()) state->error = text("获取站点连接方式失败", "Failed to load connection methods");
      else { m_methods = state->methods; m_methodsLoaded = true; m_methodsAge.restart(); m_methodsCacheRevision = m_service->methodsRevision(); state->methodsAge = m_methodsAge; state->methodsRevision = m_methodsCacheRevision; }
      update();
    });
  }
}
void MainWindow::requestConnection(const QJsonObject &asset, bool editOnly,
                                   const QString &protocol) {
  if (m_connectionInFlight || !asset.value("is_active").toBool(true) ||
      asset["id"].toString().isEmpty() || !m_pendingAsset.isEmpty())
    return;
  m_pendingAsset = asset;
  m_editOnly = editOnly;
  m_pendingProtocol = protocol;
  statusBar()->showMessage(
      text("正在读取连接权限…", "Loading connection permissions…"));
  if (!m_methodsLoaded) {
    const QString id = asset.value("id").toString();
    m_service->lookupMethods([this, id](QJsonValue value, QString failure) {
      if (m_pendingAsset.value("id").toString() != id) return;
      if (!failure.isEmpty()) {
        m_pendingAsset = {};
        m_waitingDetail = {};
        emit m_service->error(text("无法获取站点连接方式，请重试", "Unable to load connection methods; please retry"));
        return;
      }
      m_methods = value.toObject();
      m_methodsLoaded = true;
      m_methodsAge.restart();
      m_methodsCacheRevision = m_service->methodsRevision();
      if (!m_waitingDetail.isEmpty()) {
        const auto detail = m_waitingDetail;
        m_waitingDetail = {};
        consumeDetails(id, detail);
      }
    });
  }
  if (m_pendingAsset.isEmpty()) return;
  m_service->detail(asset["id"].toString());
}
void MainWindow::consumeDetails(const QString &id, const QJsonObject &details) {
  if (m_pendingAsset["id"].toString() != id)
    return;
  if (!m_methodsLoaded) {
    m_waitingDetail = details;
    return;
  }
  auto asset = m_pendingAsset;
  m_pendingAsset = {};
  asset["permed_protocols"] = details.value("permed_protocols");
  asset["permed_accounts"] = details.value("permed_accounts");
  for (int row = 0; row < m_assets.size(); ++row) {
    if (m_assets[row].toObject().value("id").toString() == id) {
      m_assets[row] = asset;
      break;
    }
  }
  const bool remember =
      m_publicSettings.value("SECURITY_LUNA_REMEMBER_AUTH").toBool(true);
  const QString connectionScope = scope();
  const quint64 connectionContext = m_service->contextRevision();
  const bool editOnly = m_editOnly;
  const bool forceDialog = m_forceConnectionDialog;
  m_forceConnectionDialog = false;
  ConnectionDialog dialog(asset, details, m_methods, connectionScope, remember,
                          editOnly, m_pendingProtocol, this);
  connect(m_service, &AppService::contextChanged, &dialog, &QDialog::reject);
  statusBar()->clearMessage();
  const bool accepted =
      (!editOnly && !forceDialog && dialog.tryDirectConnection(!m_pendingProtocol.isEmpty())) ||
      dialog.exec() == QDialog::Accepted;
  if (accepted && m_service->authenticated() && scope() == connectionScope && m_service->contextRevision() == connectionContext) {
    renderAssets();
    if (!editOnly) {
      m_connectionInFlight = true;
      m_service->connection(dialog.requestBody(), dialog.extraParameters());
    } else
      processNextConnection();
  } else {
    m_connectionQueue.clear();
  }
}
void MainWindow::processNextConnection() {
  while (!m_connectionQueue.isEmpty() &&
         !m_connectionQueue.first().value("is_active").toBool(true))
    m_connectionQueue.removeFirst();
  if (m_connectionQueue.isEmpty() || !m_pendingAsset.isEmpty())
    return;
  auto asset = m_connectionQueue.takeFirst();
  requestConnection(asset);
}
void MainWindow::updateSessionUi() {
  updateLoginState();
  const bool active = m_service->authenticated();
  if (m_restoring) m_content->setCurrentWidget(m_restorePage);
  else m_content->setCurrentIndex(active ? 1 : 0);
  m_switcher->setEnabled(!m_restoring);
  m_settingsButton->setEnabled(!m_restoring);
  findChild<QPushButton *>("avatar")->setEnabled(!m_restoring);
  m_categories->setEnabled(active);
  m_search->setEnabled(active);
  m_refreshButton->setEnabled(active && !m_loading);
  m_refreshButton->setVisible(active);
  m_sortButton->setVisible(active);
  m_layoutButton->setVisible(active);
  m_sortButton->setEnabled(active);
  m_layoutButton->setEnabled(active);
  m_logout->setVisible(active && !m_collapsed);
  m_organizations->setVisible(
      active && m_organizations->count() > 1 &&
      m_publicSettings.value("XPACK_LICENSE_IS_VALID").toBool());
  m_profile->setText(active ? m_profileData["name"].toString(
                                  m_profileData["username"].toString()) +
                                  "\n" + m_service->site()
                            : text("尚未登录", "Not signed in"));
  m_profile->setToolTip(m_profile->text());
}
void MainWindow::updateLoginState() {
  const bool pending = m_service->loginPending();
  m_loginButton->setEnabled(!pending);
  m_cancelLogin->setVisible(pending);
  m_loginHint->setVisible(!pending);
  m_authTools->setVisible(pending && m_authorizationUrl.isValid());
  m_sites->setEnabled(!pending);
  if (pending) {
    m_loginMessage->clear();
    m_loginMessage->hide();
  }
}
void MainWindow::switchSite(const QString &site) {
  m_connectionInFlight = false;
  m_sites->setCurrentText(site);
  m_connectionQueue.clear();
  m_pendingAsset = {};
  m_waitingDetail = {};
  m_service->restore(site);
}
void MainWindow::showError(const QString &message, bool statusOnly) {
  statusBar()->showMessage(message);
  m_loginMessage->clear();
  m_loginMessage->hide();
  if (statusOnly || m_visibleErrors.contains(message)) return;
  m_visibleErrors.insert(message);
  QMessageBox dialog(QMessageBox::Warning, text("操作失败", "Operation failed"),
                     message, QMessageBox::Ok, this);
  dialog.setTextFormat(Qt::PlainText);
  dialog.exec();
  m_visibleErrors.remove(message);
}
void MainWindow::showSettings() {
  SettingsDialog dialog(m_config, m_version, this);
  connect(&dialog, &SettingsDialog::preferencesChanged, this, [this] {
    ClientTheme::apply();
    retranslateUi();
    emit preferencesChanged();
  });
  dialog.exec();
}
void MainWindow::handleUrl(const QString &rawUrl) {
  if (!rawUrl.startsWith("jms://", Qt::CaseInsensitive))
    return;
  if (rawUrl.startsWith("jms://auth/callback", Qt::CaseInsensitive)) {
    showNormal();
    raise();
    activateWindow();
    m_service->acceptCallback(QUrl(rawUrl));
  } else
    m_launcher->launch(rawUrl);
}
void MainWindow::closeEvent(QCloseEvent *event) {
  QSettings().setValue("window/geometry", saveGeometry());
  if (QSettings().value("general/closeAction", "quit").toString() == "tray" &&
      QSystemTrayIcon::isSystemTrayAvailable()) {
    event->ignore();
    hide();
    return;
  }
  m_service->cancelLogin();
  QMainWindow::closeEvent(event);
}
void MainWindow::changeEvent(QEvent *event) {
  QMainWindow::changeEvent(event);
}
