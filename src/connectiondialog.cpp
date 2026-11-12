#include "connectiondialog.h"
#include "platform.h"
#include "theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QTabWidget>
#include <QTabBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUrl>
#include <algorithm>
using ClientTheme::text;
QString ConnectionDialog::connectionKey(const QString &scope,
                                        const QString &assetId) {
  return "connections/" +
         QString::fromLatin1(
             QCryptographicHash::hash((scope + "\n" + assetId).toUtf8(),
                                      QCryptographicHash::Sha256)
                 .toHex());
}
QJsonObject ConnectionDialog::saved(const QString &scope,
                                    const QString &assetId) {
  return QJsonDocument::fromJson(
             QSettings().value(connectionKey(scope, assetId)).toByteArray())
      .object();
}
QString ConnectionDialog::displayAddress(const QJsonObject &asset,
                                         const QJsonObject &saved) {
  const QString address = asset.value("address").toString();
  const QString protocol = saved.value("protocol").toString();
  if (protocol.isEmpty()) return address;
  QString username = saved.value("accountUsername").toString();
  const QString accountId = saved.value("account").toString();
  // Older preferences only saved a label. Resolve the actual username from
  // authorized metadata rather than treating an account UUID as a username.
  if (username.isEmpty() && !accountId.startsWith('@')) {
    for (const auto &value : asset.value("permed_accounts").toArray()) {
      const auto account = value.toObject();
      if (account.value("id").toString() == accountId) {
        username = account.value("username").toString(account.value("name").toString());
        break;
      }
    }
    if (username.isEmpty()) username = saved.value("accountLabel").toString();
  }
  if (accountId == "@USER") username.clear();
  int port = saved.value("port").toInt();
  for (const auto &value : asset.value("permed_protocols").toArray()) {
    const auto entry = value.toObject();
    if (entry.value("name").toString() == protocol) {
      const int current = entry.value("port").toVariant().toInt();
      if (current > 0 && current <= 65535) port = current;
      break;
    }
  }
  // Preserve an existing Web URL's scheme, port, path, query and fragment.
  const bool fullUrl = address.contains("://");
  QUrl url;
  if (fullUrl) url = QUrl(address);
  else {
    url.setScheme(protocol);
    QString host = address;
    if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
    url.setHost(host);
  }
  if (!url.isValid() || url.host().isEmpty())
    return protocol.toUpper() + " · " + username + " · " + address;
  url.setUserName(username);
  url.setPassword(QString());
  if (!fullUrl && port > 0 && port <= 65535) url.setPort(port);
  return url.toString(QUrl::FullyEncoded);
}
ConnectionDialog::ConnectionDialog(
    const QJsonObject &asset, const QJsonObject &details,
    const QJsonObject &methods, const QString &scope, bool rememberAllowed,
    bool editOnly, const QString &preferredProtocol, QWidget *parent)
    : QDialog(parent), m_asset(asset), m_details(details), m_methods(methods),
      m_key(connectionKey(scope, asset["id"].toString())),
      m_requestedProtocol(preferredProtocol),
      m_rememberAllowed(rememberAllowed) {
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setWindowTitle(text("连接设置", "Connection settings"));
  setObjectName("connectionDialog");
  setMinimumSize(520, 320);
  resize(560, 320);
  m_saved = saved(scope, asset["id"].toString());
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *content = new QWidget(this);
  auto *contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);
  contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
  auto *scroll = new QScrollArea(this);
  m_scroll = scroll;
  scroll->setObjectName("connectionScroll");
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(content);
  layout->addWidget(scroll, 1);
  auto *header = new QFrame(this);
  header->setObjectName("dialogHeader");
  auto *headerLayout = new QVBoxLayout(header);
  headerLayout->setContentsMargins(24, 22, 24, 20);
  headerLayout->setSpacing(8);
  auto *title = new QLabel(text("连接", "Connect") + " - " + asset["name"].toString(), header);
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  title->setProperty("role", "dialogTitle");
  headerLayout->addWidget(title);
  auto *description =
      new QLabel(text("保存后，下次连接将默认使用这些设置，右击重新设置",
                      "Once saved, these settings are used by default for future connections. Right-click to change them."),
                 header);
  description->setProperty("muted", true);
  description->setWordWrap(true);
  headerLayout->addWidget(description);
  contentLayout->addWidget(header);
  auto *body = new QWidget(this);
  body->setObjectName("connectionForm");
  auto *form = new QVBoxLayout(body);
  form->setContentsMargins(24, 20, 24, 22);
  form->setSpacing(16);
  QString assetType =
      asset.value("type").isObject()
          ? asset.value("type").toObject().value("value").toString()
          : asset.value("type").toString();
  const QString category =
      asset.value("category").toObject().value("value").toString();
  auto addField = [body, form](QLabel *label, QWidget *control) {
    auto *field = new QWidget(body);
    auto *fieldLayout = new QVBoxLayout(field);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(7);
    label->setProperty("role", "fieldLabel");
    label->setBuddy(control);
    fieldLayout->addWidget(label);
    control->setMinimumHeight(34);
    fieldLayout->addWidget(control);
    form->addWidget(field);
    return field;
  };
  contentLayout->addWidget(body, 1);
  m_protocol = new ClientTheme::ComboBox(this);
  m_account = new ClientTheme::ComboBox(this);
  m_methodTabs = new QTabWidget(this);
  m_methodTabs->setDocumentMode(true);
  m_methodTabs->tabBar()->setExpanding(true);
  m_methodTabs->tabBar()->setDrawBase(false);
  m_methodTabs->setIconSize(QSize(18, 18));
  m_protocol->setObjectName("protocol");
  m_account->setObjectName("account");
  m_methodTabs->setObjectName("connectionMethodTabs");
  m_username = new QLineEdit(this);
  m_secret = new QLineEdit(this);
  m_secret->setEchoMode(QLineEdit::Password);
  m_username->setObjectName("username");
  m_secret->setObjectName("secret");
  m_remember = new QCheckBox(
      text("记住本资产的输入凭据", "Remember credentials for this asset"),
      this);
  m_remember->setObjectName("rememberSecret");
  m_usernameLabel = new QLabel(text("用户名", "Username"), this);
  m_secretLabel = new QLabel(text("密码", "Password"), this);
  addField(new QLabel(text("协议", "Protocol"), body), m_protocol);
  m_methodField = addField(new QLabel(text("连接方式", "Connection method"), body), m_methodTabs);
  m_methodField->setObjectName("connectionMethodField");
  addField(new QLabel(text("账号", "Account"), body), m_account);
  m_usernameField = addField(m_usernameLabel, m_username);
  m_secretField = addField(m_secretLabel, m_secret);
  m_username->setPlaceholderText(text("输入用户名", "Enter username"));
  m_secret->setPlaceholderText(text("输入密码", "Enter password"));
  form->addWidget(m_remember);
  QStringList orderedProtocols;
  for (const auto &v : details["permed_protocols"].toArray()) {
    const QString p = v.toObject()["name"].toString();
    if (!p.isEmpty() && p != "winrm")
      orderedProtocols.append(p);
  }
  const QString type = assetType.toLower();
  const QStringList priority = type.startsWith("windows")
      ? QStringList{"rdp", "vnc", "ssh", "sftp"}
      : (type == "linux" || category == "device")
          ? QStringList{"ssh", "sftp", "vnc"} : QStringList{};
  std::stable_sort(orderedProtocols.begin(), orderedProtocols.end(),
                   [&priority](const QString &a, const QString &b) {
    const int ai = priority.indexOf(a.toLower()), bi = priority.indexOf(b.toLower());
    return (ai < 0 ? priority.size() : ai) < (bi < 0 ? priority.size() : bi);
  });
  for (const QString &p : orderedProtocols) m_protocol->addItem(p.toUpper(), p);
  for (const auto &v : details["permed_accounts"].toArray()) {
    auto account = v.toObject();
    QString alias = account["alias"].toString();
    QString id = account["id"].toString();
    if (alias == "@INPUT" || alias == "@USER")
      id = alias;
    if (id.isEmpty())
      continue;
    QString label = account["name"].toString();
    const QString user = account["username"].toString();
    if (id == "@INPUT")
      label = text("手动输入", "Manual input");
    else if (id == "@USER")
      label = text("同名账号", "Dynamic user");
    if (!user.isEmpty() && label != user)
      label += " (" + user + ")";
    account["token_account"] = id;
    m_account->addItem(label, account.toVariantMap());
  }
  QString protocol = preferredProtocol.isEmpty()
                         ? m_saved.value("protocol").toString()
                         : preferredProtocol;
  int pidx = m_protocol->findData(protocol);
  if (pidx >= 0)
    m_protocol->setCurrentIndex(pidx);
  const QString savedAccount = m_saved.value("account").toString();
  if (savedAccount.isEmpty()) {
    for (int i = 0; i < m_account->count(); ++i) {
      if (!m_account->itemData(i)
               .toMap()
               .value("token_account")
               .toString()
               .startsWith('@')) {
        m_account->setCurrentIndex(i);
        break;
      }
    }
  }
  for (int i = 0; i < m_account->count(); ++i)
    if (m_account->itemData(i).toMap().value("token_account").toString() ==
        savedAccount)
      m_account->setCurrentIndex(i);
  m_message = new QLabel(this);
  m_message->setObjectName("connectionMessage");
  m_message->setTextFormat(Qt::PlainText);
  m_message->setWordWrap(true);
  m_message->setProperty("role", "validation");
  form->addWidget(m_message);
  form->addStretch();
  auto *footer = new QFrame(this);
  m_footer = footer;
  footer->setObjectName("dialogFooter");
  auto *footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(24, 14, 24, 14);
  footerLayout->addStretch();
  m_buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  m_buttons->button(QDialogButtonBox::Ok)
      ->setText(editOnly ? text("保存", "Save") : text("连接", "Connect"));
  m_buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
  m_buttons->button(QDialogButtonBox::Cancel)->setText(text("取消", "Cancel"));
  m_buttons->button(QDialogButtonBox::Ok)->setMinimumWidth(88);
  m_buttons->button(QDialogButtonBox::Cancel)->setMinimumWidth(88);
  m_buttons->button(QDialogButtonBox::Cancel)->setProperty("quiet", true);
  setTabOrder(m_protocol, m_methodTabs);
  setTabOrder(m_methodTabs, m_account);
  setTabOrder(m_account, m_username);
  setTabOrder(m_username, m_secret);
  setTabOrder(m_secret, m_remember);
  setTabOrder(m_remember, m_buttons->button(QDialogButtonBox::Ok));
  footerLayout->addWidget(m_buttons);
  layout->addWidget(footer);
  connect(m_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { updateMethods(); });
  connect(m_methodTabs, &QTabWidget::currentChanged, this, &ConnectionDialog::updateMethodPage);
  connect(m_account, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] {
            m_secret->clear();
            m_username->clear();
            updateAccount();
          });
  connect(m_username, &QLineEdit::textChanged, this, &ConnectionDialog::updateValidation);
  connect(m_buttons, &QDialogButtonBox::accepted, this,
          &ConnectionDialog::saveAndAccept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  updateMethods();
  updateAccount();
  if (m_rememberAllowed && m_saved.value("rememberSecret").toBool() &&
      m_account->currentData().toMap().value("token_account").toString() ==
          savedAccount) {
    QString error;
    const auto data = CredentialStore::read(m_key, &error);
    if (error.isEmpty()) {
      auto secret = QJsonDocument::fromJson(data).object();
      m_username->setText(secret["username"].toString());
      m_secret->setText(secret["secret"].toString());
      m_remember->setChecked(true);
    }
  }
}
bool ConnectionDialog::canConnectDirectly(bool explicitProtocol) const {
  if (!m_buttons->button(QDialogButtonBox::Ok)->isEnabled())
    return false;
  const auto body = requestBody();
  const QString account = body["account"].toString();
  const QString protocol = body["protocol"].toString();
  if (!m_requestedProtocol.isEmpty() && protocol != m_requestedProtocol)
    return false;
  if (!explicitProtocol && m_saved.value("protocol").toString() != protocol)
    return false;
  if (!m_saved.isEmpty()) {
    if (m_saved.value("account").toString() != account)
      return false;
    if (m_saved.value("protocol").toString() == protocol &&
        m_saved.value("connectMethod").toString() !=
            body["connect_method"].toString())
      return false;
    if (!m_saved.value("connectMethodType").toString().isEmpty() &&
        m_saved.value("connectMethodType") != m_selectedMethod.value("type"))
      return false;
  } else if (!explicitProtocol) {
    return false;
  }
  if (account == "@INPUT" || account == "@USER")
    return m_rememberAllowed && m_remember->isChecked() &&
           !m_secret->text().isEmpty() &&
           (account != "@INPUT" || !m_username->text().trimmed().isEmpty());
  return !account.isEmpty();
}
bool ConnectionDialog::tryDirectConnection(bool explicitProtocol) {
  if (!canConnectDirectly(explicitProtocol))
    return false;
  saveAndAccept();
  return result() == QDialog::Accepted;
}
bool ConnectionDialog::isClientMethod(const QString &protocol, const QJsonObject &method) {
  const QString type = method.value("type").toString();
  const QString value = method.value("value").toString();
  if (value.isEmpty() || method.value("disabled").toBool()) return false;
  if (type == "applet" || type == "virtual_app") return true;
  if (type != "native" || protocol == "http" || value.endsWith("_guide")) return false;
  const QString endpoint = method.value("endpoint_protocol").toString();
  static const QSet<QString> supported{"ssh", "sftp", "telnet", "rdp", "vnc", "mysql", "mariadb", "postgresql", "redis", "oracle", "sqlserver", "mongodb"};
  return supported.contains(endpoint.isEmpty() ? protocol : endpoint);
}
void ConnectionDialog::updateMethods() {
  const QSignalBlocker blocker(m_methodTabs);
  while (m_methodTabs->count()) {
    auto *page = m_methodTabs->widget(0);
    m_methodTabs->removeTab(0);
    delete page;
  }
  m_selectedMethod = {};
  const QString protocol = m_protocol->currentData().toString();
  const auto methods = m_methods[protocol].toArray();
  const QString savedValue = m_saved.value("protocol").toString() == protocol
      ? m_saved.value("connectMethod").toString() : QString();
  const QString savedType = m_saved.value("connectMethodType").toString();
  int selectedTab = 0;
  const QStringList types{"native", "applet", "virtual_app"};
  const QStringList labels{text("本地客户端", "Native"), text("远程应用", "Remote apps"), text("虚拟应用", "Virtual apps")};
  for (int typeIndex = 0; typeIndex < types.size(); ++typeIndex) {
    const QString type = types[typeIndex];
    auto *page = new QWidget;
    page->setObjectName("connectionMethodPage");
    page->setProperty("methodType", type);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    auto *choices = new ClientTheme::ComboBox(page);
    choices->setObjectName("method");
    choices->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(choices);
    int preferred = -1;
    for (const auto &entry : methods) {
      const auto method = entry.toObject();
      const QString value = method["value"].toString();
      if (method["type"].toString() != type || !isClientMethod(protocol, method)) continue;
      choices->addItem(method["label"].toString(value), value);
      choices->setItemData(choices->count() - 1, method["label"].toString(value), Qt::ToolTipRole);
      if (value == savedValue && (savedType.isEmpty() || savedType == type)) preferred = choices->count() - 1;
    }
    if (!choices->count()) { delete page; continue; }
    const int index = m_methodTabs->addTab(page, labels[typeIndex]);
    if (preferred >= 0) { choices->setCurrentIndex(preferred); selectedTab = index; }
    connect(choices, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, page] {
      if (m_methodTabs->currentWidget() == page) updateMethodPage();
    });
  }
  m_methodTabs->setCurrentIndex(selectedTab);
  m_methodField->setVisible(protocol == "http" || m_methodTabs->count() > 0);
  updateMethodPage();
}
void ConnectionDialog::updateMethodPage() {
  m_selectedMethod = {};
  if (auto *page = m_methodTabs->currentWidget()) {
    auto *choices = page->findChild<QComboBox *>("method");
    if (choices->currentIndex() >= 0) {
      m_selectedMethod = {{"value", choices->currentData().toString()},
                          {"label", choices->currentText()},
                          {"type", page->property("methodType").toString()}};
      choices->setToolTip(choices->currentText());
    }
    m_methodTabs->setFixedHeight(page->sizeHint().height() + m_methodTabs->tabBar()->sizeHint().height() + 4);
  } else {
    m_methodTabs->setFixedHeight(36);
  }
  updateValidation();
}
void ConnectionDialog::updateValidation() {
  const bool hasProtocol = m_protocol->currentIndex() >= 0;
  const bool hasAccount = !m_account->currentData().toMap().value("token_account").toString().isEmpty();
  const bool hasMethod = !m_selectedMethod["value"].toString().isEmpty();
  const bool valid = hasProtocol && hasAccount && hasMethod;
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
  const bool missingUsername = m_usernameRequired &&
      m_account->currentData().toMap().value("token_account").toString() == "@INPUT" &&
      m_username->text().trimmed().isEmpty();
  m_message->setText(!hasProtocol ? text("没有可用的授权协议。", "No authorized protocol is available.")
                           : !hasAccount ? text("没有可用的授权账号。", "No authorized account is available.")
                           : !hasMethod ? (m_protocol->currentData().toString() == "http"
                               ? text("站点未提供可用的远程应用连接方式。", "No remote application connection method is available from this site.")
                               : text("没有可用的本地连接方式。", "No local connection method is available."))
                           : missingUsername ? text("请输入用户名。", "Enter a username.") : QString());
  m_message->setVisible(!m_message->text().isEmpty());
  scheduleFitHeight();
}
void ConnectionDialog::scheduleFitHeight() {
  if (m_fitQueued) return;
  m_fitQueued = true;
  QTimer::singleShot(0, this, [this] {
    m_fitQueued = false;
    if (isVisible()) {
      fitHeightToContents();
      m_initialPlacement = false;
    }
  });
}
void ConnectionDialog::fitHeightToContents() {
  if (!m_scroll || !m_footer) return;
  if (auto *page = m_methodTabs->currentWidget())
    m_methodTabs->setFixedHeight(page->sizeHint().height() + m_methodTabs->tabBar()->sizeHint().height() + 4);
  layout()->activate();
  auto *contentLayout = m_scroll->widget()->layout();
  contentLayout->invalidate();
  contentLayout->activate();
  const int contentWidth = qMax(1, m_scroll->viewport()->width());
  const int contentHeight = contentLayout->hasHeightForWidth()
      ? contentLayout->totalHeightForWidth(contentWidth) : contentLayout->sizeHint().height();
  const int desired = contentHeight + m_footer->sizeHint().height() + 2 * m_scroll->frameWidth();
  // On first show Qt may still associate the dialog with the primary screen.
  // Use the owner's screen before calculating height and placing the window.
  const QRect available = ClientTheme::dialogAvailableGeometry(this, m_initialPlacement);
  const int frameHeight = qMax(0, frameGeometry().height() - height());
  const int limit = qMin(maximumHeight(), available.height() - frameHeight - 32);
  resize(width(), qBound(minimumHeight(), desired, qMax(minimumHeight(), limit)));
  // Center using the final decorated size, not the constructor's 320px height.
  // Later field changes keep the top-left stable unless the frame would leave
  // the available screen; this also respects a position chosen by the user.
  // Sit higher than the owner's center so the expanded manual-account form
  // (which grows downward) still stays comfortably on screen.
  if (isVisible()) ClientTheme::positionDialog(this, m_initialPlacement, 70);
}
void ConnectionDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  fitHeightToContents();
  scheduleFitHeight(); // Native frame metrics become available after first show.
}
void ConnectionDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  if (event->size().width() != event->oldSize().width()) scheduleFitHeight();
}
void ConnectionDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
    scheduleFitHeight();
}
void ConnectionDialog::updateAccount() {
  const QString id =
      m_account->currentData().toMap().value("token_account").toString();
  const bool manual = id == "@INPUT";
  const bool dynamic = id == "@USER";
  m_usernameField->setVisible(manual);
  m_secretField->setVisible(manual || dynamic);
  m_username->setVisible(manual);
  m_usernameLabel->setVisible(manual);
  m_secret->setVisible(manual || dynamic);
  m_secretLabel->setVisible(manual || dynamic);
  m_remember->setVisible((manual || dynamic) && m_rememberAllowed);
  m_usernameRequired = false;
  updateValidation();
}
QJsonObject ConnectionDialog::requestBody() const {
  QSettings settings;
  auto account = QJsonObject::fromVariantMap(m_account->currentData().toMap());
  const QString id = account["token_account"].toString();
  const QString protocol = m_protocol->currentData().toString();
  QJsonObject options{
      {"charset", settings.value("connection/charset", "default").toString()},
      {"backspaceAsCtrlH",
       settings.value("connection/backspaceAsCtrlH", false).toBool()},
      {"resolution",
       settings.value("connection/rdpResolution", "auto").toString()},
      {"token_reusable", false},
      {"disableautohash", false}};
  if (protocol == "http" || m_selectedMethod["type"] == "applet" || m_selectedMethod["type"] == "virtual_app") {
    // The site chooses the application type. A VirtualApp client request uses
    // a different mode flag from a Windows Applet client request.
    const bool virtualApp = m_selectedMethod["type"].toString() == "virtual_app";
    options["appletConnectMethod"] = virtualApp ? "web" : "client";
    options["virtualappConnectMethod"] = virtualApp ? "client" : "web";
    options["reusable"] = false;
    options["rdp_connection_speed"] = "auto";
  } else {
    options["rdp_resolution"] = settings.value("connection/rdpResolution", "auto").toString();
    options["keyboard_layout"] = settings.value("connection/keyboardLayout", "en-us-qwerty").toString();
    options["rdp_client_option"] = QJsonArray::fromStringList(settings.value("connection/rdpClientOptions").toStringList());
    options["rdp_color_quality"] = settings.value("connection/rdpColorQuality", "32").toString();
    options["rdp_smart_size"] = settings.value("connection/rdpSmartSize", "0").toString();
  }
  return QJsonObject{
      {"asset", m_asset["id"]},
      {"account", id},
      {"protocol", protocol},
      {"input_username", id == "@INPUT"  ? m_username->text()
                         : id == "@USER" ? QString()
                                         : account["name"].toString()},
      {"input_secret",
       id == "@INPUT" || id == "@USER" ? m_secret->text() : QString()},
      {"connect_method", m_selectedMethod["value"].toString()},
      {"connect_options", options}};
}
QJsonObject ConnectionDialog::extraParameters() const {
  QSettings settings;
  QJsonObject out;
  const QString resolution =
      settings.value("connection/rdpResolution", "auto").toString();
  const auto parts = resolution.split('x');
  if (parts.size() == 2) {
    out["width"] = parts[0];
    out["height"] = parts[1];
  }
  const auto options =
      settings.value("connection/rdpClientOptions").toStringList();
  if (options.contains("full_screen"))
    out["full_screen"] = "1";
  if (options.contains("multi_screen"))
    out["multi_mon"] = "1";
  if (options.contains("drives_redirect"))
    out["drives_redirect"] = "1";
  out["rdp_smart_size"] =
      settings.value("connection/rdpSmartSize", "0").toString();
  out["rdp_color_quality"] =
      settings.value("connection/rdpColorQuality", "32").toString();
  return out;
}
void ConnectionDialog::saveAndAccept() {
  const auto body = requestBody();
  if (body["account"].toString() == "@INPUT" &&
      m_username->text().trimmed().isEmpty()) {
    m_usernameRequired = true;
    updateValidation();
    m_username->setFocus();
    return;
  }
  const bool remember =
      m_rememberAllowed && !m_remember->isHidden() && m_remember->isChecked();
  QString error;
  if (remember) {
    if (!CredentialStore::write(
            m_key,
            QJsonDocument(QJsonObject{{"username", m_username->text()},
                                      {"secret", m_secret->text()}})
                .toJson(QJsonDocument::Compact),
            &error)) {
      QMessageBox::warning(this, text("保存失败", "Could not save"), error);
      return;
    }
  } else if (m_saved.value("rememberSecret").toBool()) {
    CredentialStore::remove(m_key, &error);
    if (!error.isEmpty()) {
      QMessageBox::warning(this, text("凭据存储", "Credential storage"), error);
      return;
    }
  }
  QJsonObject data{{"protocol", body["protocol"]},
                   {"account", body["account"]},
                   {"accountLabel", m_account->currentText()},
                   {"connectMethod", body["connect_method"]},
                   {"connectMethodType", m_selectedMethod["type"]},
                   {"rememberSecret", remember}};
  const auto account = m_account->currentData().toMap();
  const QString accountId = body["account"].toString();
  QString username;
  if (accountId == "@INPUT") username = m_username->text().trimmed();
  else if (accountId != "@USER") {
    username = account.value("username").toString();
    if (username.isEmpty()) username = account.value("name").toString();
  }
  data["accountUsername"] = username;
  data["connectMethodLabel"] = m_selectedMethod.value("label").toString(
      m_selectedMethod.value("value").toString());
  for (const auto &value : m_details.value("permed_protocols").toArray()) {
    const auto protocol = value.toObject();
    if (protocol.value("name") == body["protocol"]) {
      const int port = protocol.value("port").toVariant().toInt();
      if (port > 0 && port <= 65535) data["port"] = port;
      break;
    }
  }
  QJsonArray protocols;
  for (int index = 0; index < m_protocol->count(); ++index)
    protocols.append(m_protocol->itemData(index).toString());
  data["availableProtocols"] = protocols;
  QSettings().setValue(m_key,
                       QJsonDocument(data).toJson(QJsonDocument::Compact));
  accept();
}
