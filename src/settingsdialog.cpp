#include "settingsdialog.h"
#include "platform.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
using ClientTheme::text;
SettingsDialog::SettingsDialog(ConfigStore *config, const QJsonValue &version,
                               QWidget *parent)
    : QDialog(parent), m_config(config) {
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setWindowTitle(text("设置", "Settings"));
  setObjectName("settingsDialog");
  resize(960, 640);
  setMinimumSize(820, 520);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto *body = new QHBoxLayout;
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);
  layout->addLayout(body, 1);
  auto *sidebar = new QFrame(this);
  sidebar->setObjectName("settingsSidebar");
  sidebar->setFixedWidth(174);
  auto *side = new QVBoxLayout(sidebar);
  side->setContentsMargins(14, 26, 14, 20);
  side->setSpacing(20);
  auto *heading = new QLabel(text("设置", "Settings"), sidebar);
  heading->setProperty("role", "dialogTitle");
  side->addWidget(heading);
  auto *navigation = new QListWidget(sidebar);
  navigation->setObjectName("settingsNavigation");
  navigation->setFrameShape(QFrame::NoFrame);
  navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  navigation->setSpacing(3);
  navigation->setIconSize(QSize(18, 18));
  side->addWidget(navigation, 1);
  auto *sideCaption = new QLabel("OSMClient", sidebar);
  sideCaption->setProperty("muted", true);
  side->addWidget(sideCaption);
  body->addWidget(sidebar);
  // Keep the page container and indices stable for keyboard navigation and
  // existing automation; the native tab strip is replaced by the side menu.
  auto *tabs = new QTabWidget(this);
  tabs->setObjectName("settingsPages");
  tabs->tabBar()->hide();
  tabs->setDocumentMode(true);
  body->addWidget(tabs, 1);
  auto addPage = [tabs, navigation](QWidget *content, const QString &title,
                                    const QString &description,
                                    bool stretch = false) {
    auto *page = new QWidget;
    page->setObjectName("settingsPage");
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(28, 26, 28, 28);
    pageLayout->setSpacing(10);
    auto *titleLabel = new QLabel(title, page);
    titleLabel->setProperty("role", "sectionTitle");
    pageLayout->addWidget(titleLabel);
    auto *descriptionLabel = new QLabel(description, page);
    descriptionLabel->setProperty("muted", true);
    descriptionLabel->setWordWrap(true);
    pageLayout->addWidget(descriptionLabel);
    pageLayout->addSpacing(14);
    content->setProperty("surface", true);
    pageLayout->addWidget(content, stretch ? 1 : 0);
    if (!stretch)
      pageLayout->addStretch();
    auto *scroll = new QScrollArea(tabs);
    scroll->setObjectName("settingsScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    tabs->addTab(scroll, title);
    auto *item = new QListWidgetItem(title, navigation);
    const QStringList icons{"settings", "palette", "desktop", "app", "info"};
    item->setIcon(ClientTheme::icon(icons.value(navigation->count() - 1)));
    item->setSizeHint(QSize(140, 40));
    item->setToolTip(title);
  };
  auto styleForm = [](QFormLayout *form) {
    form->setContentsMargins(20, 20, 20, 20);
    form->setVerticalSpacing(18);
    form->setHorizontalSpacing(28);
    form->setFormAlignment(Qt::AlignTop | Qt::AlignLeft);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  };
  auto alignField = [](QFormLayout *form, QWidget *field,
                       bool compact = false) {
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!compact) {
      field->setMinimumWidth(224);
      field->setMaximumWidth(260);
    }
    form->setAlignment(field, Qt::AlignRight | Qt::AlignVCenter);
  };
  auto addChoice = [this,
                    alignField](QFormLayout *form, const QString &label,
                                const QString &key,
                                const QList<QPair<QString, QString>> &choices,
                                const QString &fallback) {
    auto *combo = new ClientTheme::ComboBox(this);
    combo->setObjectName(key);
    combo->setMinimumHeight(34);
    for (const auto &p : choices)
      combo->addItem(p.first, p.second);
    int idx = combo->findData(QSettings().value(key, fallback));
    combo->setCurrentIndex(qMax(0, idx));
    form->addRow(label, combo);
    alignField(form, combo);
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, combo, key] {
              QSettings().setValue(key, combo->currentData());
              emit preferencesChanged();
            });
    return combo;
  };
  auto addBool = [this, alignField](QFormLayout *form, const QString &label,
                                    const QString &key, bool fallback) {
    auto *check = new QCheckBox(this);
    check->setObjectName(key);
    check->setMinimumHeight(34);
    check->setChecked(QSettings().value(key, fallback).toBool());
    form->addRow(label, check);
    alignField(form, check, true);
    connect(check, &QCheckBox::toggled, this, [this, key](bool value) {
      QSettings().setValue(key, value);
      emit preferencesChanged();
    });
    return check;
  };
  auto *general = new QFrame;
  auto *g = new QFormLayout(general);
  styleForm(g);
  addPage(general, text("通用", "General"),
          text("语言、终端与桌面行为。",
               "Language, terminal and desktop preferences."));
  auto *language = addChoice(g, text("语言", "Language"), "appearance/language",
                             {{text("跟随系统", "System"), "system"},
                              {"简体中文", "zh"},
                              {"English", "en"}},
                             "system");
  language->setToolTip(text("重新打开设置后应用新的设置页语言。",
                            "Reopen settings to update this page’s language."));
  addChoice(g, text("字符集", "Character set"), "connection/charset",
            {{text("默认", "Default"), "default"},
             {"UTF-8", "utf8"},
             {"GBK", "gbk"},
             {"GB2312", "gb2312"},
             {"ISO-8859-1", "iso-8859-1"}},
            "default");
  addBool(g, text("退格键发送 Ctrl+H", "Backspace sends Ctrl+H"),
          "connection/backspaceAsCtrlH", false);
  addChoice(g, text("关闭主窗口", "Close main window"), "general/closeAction",
            {{text("退出", "Quit"), "quit"},
             {text("最小化到托盘", "Hide to tray"), "tray"}},
            "quit");
  addBool(g, text("下次启动恢复登录", "Restore login on startup"),
          "general/restoreLogin", true);
  auto *appearance = new QFrame;
  auto *a = new QFormLayout(appearance);
  styleForm(a);
  addPage(appearance, text("外观", "Appearance"),
          text("选择适合你的主题、字体与强调色。",
               "Choose your theme, typeface and accent colors."));
  addChoice(a, text("主题", "Theme"), "appearance/theme",
            {{text("跟随系统", "System"), "system"},
             {text("浅色", "Light"), "light"},
             {text("深色", "Dark"), "dark"}},
            "system");
  auto *font = new QFontComboBox(this);
  font->setCurrentFont(
      QFont(QSettings()
                .value("appearance/font", QApplication::font().family())
                .toString()));
  a->addRow(text("字体", "Font"), font);
  alignField(a, font);
  connect(font, &QFontComboBox::currentFontChanged, this,
          [this](const QFont &font) {
            QSettings().setValue("appearance/font", font.family());
            emit preferencesChanged();
          });
  auto *size = new QSpinBox(this);
  size->setRange(9, 20);
  size->setValue(QSettings().value("appearance/fontSize", 11).toInt());
  a->addRow(text("字号", "Font size"), size);
  alignField(a, size);
  connect(size, QOverload<int>::of(&QSpinBox::valueChanged), this,
          [this](int value) {
            QSettings().setValue("appearance/fontSize", value);
            emit preferencesChanged();
          });
  for (const auto &mode : QStringList{"Light", "Dark"}) {
    auto *button = new QPushButton(text("选择颜色…", "Choose color…"), this);
    const QString key = "appearance/accent" + mode;
    a->addRow(mode == "Light" ? text("浅色主题色", "Light accent")
                              : text("深色主题色", "Dark accent"),
              button);
    alignField(a, button);
    connect(button, &QPushButton::clicked, this, [this, key, mode] {
      QColor color = QColorDialog::getColor(
          QColor(QSettings()
                     .value(key, mode == "Light" ? "#1ab394" : "#34d399")
                     .toString()),
          this);
      if (color.isValid()) {
        QSettings().setValue(key, color.name());
        emit preferencesChanged();
      }
    });
  }
  auto *remote = new QFrame;
  auto *r = new QFormLayout(remote);
  styleForm(r);
  addPage(
      remote, text("远程桌面", "Remote desktop"),
      text("调整桌面显示与重定向选项。",
           "Adjust the display and redirection options for remote sessions."));
  addChoice(r, text("分辨率", "Resolution"), "connection/rdpResolution",
            {{text("自动", "Auto"), "auto"},
             {"1024×768", "1024x768"},
             {"1366×768", "1366x768"},
             {"1600×900", "1600x900"},
             {"1920×1080", "1920x1080"}},
            "auto");
  addChoice(r, text("色彩质量", "Color quality"), "connection/rdpColorQuality",
            {{"32 bit", "32"}, {"24 bit", "24"}, {"16 bit", "16"}}, "32");
  addChoice(r, text("智能缩放", "Smart sizing"), "connection/rdpSmartSize",
            {{text("关闭", "Off"), "0"}, {text("开启", "On"), "1"}}, "0");
  // Keep values aligned with users.const.KeyboardLayout (also used by Luna).
  QList<QPair<QString, QString>> keyboardLayouts{
      {text("美国英语 (QWERTY)", "US English (QWERTY)"), "en-us-qwerty"},
      {text("英国英语 (QWERTY)", "UK English (QWERTY)"), "en-gb-qwerty"},
      {text("日语 (QWERTY)", "Japanese (QWERTY)"), "ja-jp-qwerty"},
      {text("法语 (AZERTY)", "French (AZERTY)"), "fr-fr-azerty"},
      {text("瑞士法语 (QWERTZ)", "Swiss French (QWERTZ)"), "fr-ch-qwertz"},
      {text("比利时法语 (AZERTY)", "Belgian French (AZERTY)"), "fr-be-azerty"},
      {text("土耳其语 Q (QWERTY)", "Turkish-Q (QWERTY)"), "tr-tr-qwerty"},
      {text("西班牙语", "Spanish"), "es-es-qwerty"},
      {text("拉丁美洲西班牙语", "Spanish (Latin American)"), "es-latam-qwerty"},
      {text("德语 (QWERTZ)", "German (QWERTZ)"), "de-de-qwertz"},
      {text("瑞士德语 (QWERTZ)", "Swiss German (QWERTZ)"), "de-ch-qwertz"},
      {text("瑞典语 (QWERTY)", "Swedish (QWERTY)"), "sv-se-qwerty"},
      {text("意大利语 (QWERTY)", "Italian (QWERTY)"), "it-it-qwerty"},
      {text("葡萄牙语 (QWERTY)", "Portuguese (QWERTY)"), "pt-pt-qwerty"},
      {text("巴西葡萄牙语 (QWERTY)", "Brazilian Portuguese (QWERTY)"), "pt-br-qwerty"},
      {text("波兰语 (QWERTY)", "Polish (QWERTY)"), "pl-pl-qwerty"},
      {text("匈牙利语 (QWERTZ)", "Hungarian (QWERTZ)"), "hu-hu-qwertz"},
      {text("挪威语 (QWERTY)", "Norwegian (QWERTY)"), "no-no-qwerty"},
      {text("罗马尼亚语 (QWERTY)", "Romanian (QWERTY)"), "ro-ro-qwerty"},
      {text("丹麦语 (QWERTY)", "Danish (QWERTY)"), "da-dk-qwerty"},
      {text("加拿大法语 (QWERTY)", "Canadian French (QWERTY)"), "fr-ca-qwerty"}};
  const QString savedKeyboard = QSettings().value("connection/keyboardLayout", "en-us-qwerty").toString();
  bool knownKeyboard = false;
  for (const auto &layout : keyboardLayouts) knownKeyboard |= layout.second == savedKeyboard;
  if (!knownKeyboard)
    keyboardLayouts.append({savedKeyboard.isEmpty()
        ? text("未指定（已保存）", "Unspecified (saved)")
        : text("已保存的布局：", "Saved layout: ") + savedKeyboard, savedKeyboard});
  auto *keyboard = addChoice(r, text("键盘布局", "Keyboard layout"),
      "connection/keyboardLayout", keyboardLayouts, "en-us-qwerty");
  for (int i = 0; i < keyboard->count(); ++i)
    keyboard->setItemData(i, keyboard->itemText(i) + "\n" + keyboard->itemData(i).toString(), Qt::ToolTipRole);
  const QList<QPair<QString, QString>> options = {
      {text("全屏", "Full screen"), "full_screen"},
      {text("多显示器", "Multiple monitors"), "multi_screen"},
      {text("磁盘映射", "Drive redirection"), "drives_redirect"}};
  for (const auto &option : options) {
    auto *box = new QCheckBox(this);
    box->setChecked(QSettings()
                        .value("connection/rdpClientOptions")
                        .toStringList()
                        .contains(option.second));
    r->addRow(option.first, box);
    alignField(r, box, true);
    connect(box, &QCheckBox::toggled, this, [key = option.second](bool value) {
      QSettings s;
      auto all = s.value("connection/rdpClientOptions").toStringList();
      all.removeAll(key);
      if (value)
        all.append(key);
      s.setValue("connection/rdpClientOptions", all);
    });
  }
  auto *apps = new QFrame;
  auto *al = new QVBoxLayout(apps);
  al->setContentsMargins(20, 20, 20, 20);
  al->setSpacing(16);
  addPage(apps, text("本地应用", "Applications"),
          text("为每种协议选择默认客户端，或指定已安装的应用。",
               "Choose a default client for each protocol or locate an "
               "installed application."),
          true);
  m_protocol = new ClientTheme::ComboBox(this);
  m_protocol->setObjectName("applicationProtocol");
  for (const auto &p :
       QStringList{"ssh", "telnet", "sftp", "rdp", "vnc", "mysql", "mariadb",
                   "postgresql", "redis", "mongodb", "oracle", "sqlserver"})
    m_protocol->addItem(p.toUpper(), p);
  auto *protocolRow = new QHBoxLayout;
  protocolRow->addWidget(new QLabel(text("连接协议", "Protocol"), apps));
  protocolRow->addStretch();
  m_protocol->setMinimumWidth(180);
  protocolRow->addWidget(m_protocol);
  al->addLayout(protocolRow);
  m_applications = new QTableWidget(this);
  m_applications->setObjectName("applicationTable");
  m_applications->setShowGrid(false);
  m_applications->setWordWrap(false);
  m_applications->setFrameShape(QFrame::NoFrame);
  m_applications->verticalHeader()->hide();
  m_applications->setSelectionMode(QAbstractItemView::NoSelection);
  m_applications->setColumnCount(5);
  m_applications->setHorizontalHeaderLabels(
      {text("应用", "Application"), text("路径", "Path"),
       text("操作", "Action"), text("默认", "Default"),
       text("下载", "Download")});
  m_applications->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                          Qt::AlignVCenter);
  m_applications->horizontalHeader()->setSectionResizeMode(
      1, QHeaderView::Stretch);
  m_applications->setEditTriggers(QAbstractItemView::NoEditTriggers);
  al->addWidget(m_applications);
  m_applicationError = new QLabel(apps);
  m_applicationError->setWordWrap(true);
  m_applicationError->setProperty("role", "validation");
  al->addWidget(m_applicationError);
  connect(m_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { reloadApplications(); });
  reloadApplications();
  auto *about = new QFrame;
  auto *ab = new QVBoxLayout(about);
  ab->setContentsMargins(28, 32, 28, 32);
  ab->setSpacing(14);
  ab->addStretch();
  addPage(about, text("关于", "About"),
          text("OSMClient 桌面客户端。", "The OSMClient desktop client."), true);
  auto *logo = new QLabel(about);
  logo->setObjectName("aboutLogo");
  logo->setPixmap(QIcon(":/icons/icon.png").pixmap(56, 56));
  logo->setFixedSize(56, 56);
  logo->setAlignment(Qt::AlignCenter);
  ab->addWidget(logo, 0, Qt::AlignHCenter);
  auto *brand = new QLabel("OSMClient", this);
  brand->setProperty("role", "dialogTitle");
  ab->addWidget(brand, 0, Qt::AlignHCenter);
  auto *appVersion =
      new QLabel("v" + QApplication::applicationVersion(), about);
  appVersion->setProperty("muted", true);
  appVersion->setToolTip(QString("Qt %1").arg(QString::fromLatin1(qVersion())));
  ab->addWidget(appVersion, 0, Qt::AlignHCenter);
  const QUrl download(version.toObject()["download_url"].toString());
  if (download.isValid() && !download.host().isEmpty() &&
      (download.scheme() == "https" || download.scheme() == "http")) {
    auto *button = new QPushButton(text("下载客户端", "Download client"), this);
    button->setProperty("primary", true);
    ab->addWidget(button, 0, Qt::AlignHCenter);
    connect(button, &QPushButton::clicked, this,
            [download] { QDesktopServices::openUrl(download); });
  }
  ab->addStretch();
  connect(navigation, &QListWidget::currentRowChanged, tabs,
          &QTabWidget::setCurrentIndex);
  connect(tabs, &QTabWidget::currentChanged, navigation,
          QOverload<int>::of(&QListWidget::setCurrentRow));
  navigation->setCurrentRow(0);
  auto *footer = new QFrame(this);
  footer->setObjectName("dialogFooter");
  auto *footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(24, 14, 24, 14);
  auto *savedNote = new QLabel(
      text("更改会自动保存", "Changes are saved automatically"), footer);
  savedNote->setProperty("muted", true);
  footerLayout->addWidget(savedNote);
  footerLayout->addStretch();
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
  buttons->button(QDialogButtonBox::Close)->setText(text("关闭", "Close"));
  buttons->button(QDialogButtonBox::Close)->setMinimumWidth(88);
  buttons->button(QDialogButtonBox::Close)->setProperty("primary", true);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  footerLayout->addWidget(buttons);
  layout->addWidget(footer);
}
void SettingsDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  if (!m_initialPlacement) return;
  layout()->activate();
  ClientTheme::positionDialog(this, true);
  // Recheck once the native title-bar metrics are available. Page switches and
  // subsequent shows keep a position chosen by the user.
  QTimer::singleShot(0, this, [this] {
    if (!isVisible() || !m_initialPlacement) return;
    ClientTheme::positionDialog(this, true);
    m_initialPlacement = false;
  });
}
void SettingsDialog::reloadApplications() {
  m_applications->setRowCount(0);
  const QString protocol = m_protocol->currentData().toString();
  QString category = "databases";
  if (protocol == "ssh" || protocol == "telnet")
    category = "terminal";
  else if (protocol == "sftp")
    category = "filetransfer";
  else if (protocol == "rdp" || protocol == "vnc")
    category = "remotedesktop";
  const QJsonObject applications = m_config->applications();
  m_applicationError->setText(m_config->error());
  m_applicationError->setVisible(!m_config->error().isEmpty());
  for (const auto &v : applications[category].toArray()) {
    const auto item = v.toObject();
    bool supports = false;
    for (const auto &p : item["protocol"].toArray())
      if (p.toString() == protocol)
        supports = true;
    if (!supports)
      continue;
    const QString name = item["name"].toString();
    int row = m_applications->rowCount();
    m_applications->insertRow(row);
    m_applications->setRowHeight(row, 56);
    m_applications->setItem(
        row, 0, new QTableWidgetItem(item["display_name"].toString(name)));
    // Only macOS internal terminal/RDP launchers ignore the configured path.
    // Windows and Linux internal entries can still use a configurable binary.
    const bool managedPath = ConfigStore::platformKey() == "macos" &&
                             item["is_internal"].toBool() && protocol != "vnc";
    auto *pathItem = new QTableWidgetItem(managedPath ? text("无需配置", "Managed automatically")
                                                  : item["path"].toString());
    pathItem->setToolTip(managedPath ? text("此客户端无需指定程序路径。", "This client does not require a program path.")
                                 : item["path"].toString());
    m_applications->setItem(row, 1, pathItem);
    auto *browse = new QPushButton(text("浏览", "Browse"), this);
    browse->setProperty("quiet", true);
    browse->setEnabled(!managedPath);
    if (managedPath) browse->setToolTip(pathItem->toolTip());
    m_applications->setCellWidget(row, 2, browse);
    connect(browse, &QPushButton::clicked, this,
            [this, category, protocol, name] {
              const QString path = QFileDialog::getOpenFileName(
                  this, text("选择客户端程序", "Choose application"));
              if (path.isEmpty())
                return;
              if (!m_config->selectApplication(category, protocol, name, path))
                QMessageBox::warning(this, text("保存失败", "Save failed"),
                                     m_config->error());
              reloadApplications();
            });
    bool preferred = false;
    for (const auto &p : item["match_first"].toArray())
      if (p.toString() == protocol)
        preferred = true;
    auto *choose = new QPushButton(preferred ? text("已选择", "Selected")
                                             : text("设为默认", "Set default"),
                                   this);
    choose->setEnabled(!preferred);
    choose->setProperty("quiet", true);
    choose->setProperty("selected", preferred);
    m_applications->setCellWidget(row, 3, choose);
    connect(choose, &QPushButton::clicked, this,
            [this, category, protocol, name] {
              if (!m_config->selectApplication(category, protocol, name))
                QMessageBox::warning(this, text("保存失败", "Save failed"),
                                     m_config->error());
              reloadApplications();
            });
    const QUrl download(item.value("download_url").toString());
    if (download.isValid() && !download.host().isEmpty() &&
        (download.scheme() == "http" || download.scheme() == "https")) {
      auto *downloadButton = new QPushButton(text("下载", "Download"), this);
      downloadButton->setProperty("quiet", true);
      m_applications->setCellWidget(row, 4, downloadButton);
      connect(downloadButton, &QPushButton::clicked, this,
              [download] { QDesktopServices::openUrl(download); });
    }
  }
  if (m_applications->rowCount() == 0 && m_config->error().isEmpty()) {
    m_applicationError->setText(text("当前协议没有配置本地应用。", "No local applications are configured for this protocol."));
    m_applicationError->show();
  }
  m_applications->resizeColumnToContents(0);
  for (int column : {2, 3, 4}) {
    int width = 72;
    for (int row = 0; row < m_applications->rowCount(); ++row) {
      auto *button = m_applications->cellWidget(row, column);
      if (!button)
        continue;
      button->ensurePolished();
      width = qMax(width, button->sizeHint().width() + 12);
    }
    m_applications->setColumnWidth(column, width);
  }
}
