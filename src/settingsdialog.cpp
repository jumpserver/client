#include "settingsdialog.h"
#include "platform.h"
#include "theme.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QSpinBox>
#include <QToolButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
using ClientTheme::text;
namespace {
struct ProtocolSection {
  QString category;
  QString titleZh;
  QString titleEn;
  QString icon;
  QList<QPair<QString, QString>> protocols;
};
// Oracle is hidden from local application settings on purpose; the helper keeps
// its Oracle routing, so existing launches are unaffected.
const QStringList &hiddenProtocols() {
  static const QStringList values{QStringLiteral("oracle")};
  return values;
}
const QList<ProtocolSection> &protocolSections() {
  static const QList<ProtocolSection> sections{
      {QStringLiteral("terminal"), QStringLiteral("命令行终端"),
       QStringLiteral("Command line"), QStringLiteral("terminal"),
       {{QStringLiteral("ssh"), QStringLiteral("SSH")},
        {QStringLiteral("telnet"), QStringLiteral("Telnet")}}},
      {QStringLiteral("filetransfer"), QStringLiteral("文件传输"),
       QStringLiteral("File transfer"), QStringLiteral("folder"),
       {{QStringLiteral("sftp"), QStringLiteral("SFTP")}}},
      {QStringLiteral("remotedesktop"), QStringLiteral("远程桌面"),
       QStringLiteral("Remote desktop"), QStringLiteral("desktop"),
       {{QStringLiteral("rdp"), QStringLiteral("RDP")},
        {QStringLiteral("vnc"), QStringLiteral("VNC")}}},
      {QStringLiteral("databases"), QStringLiteral("数据库"),
       QStringLiteral("Database"), QStringLiteral("database"),
       {{QStringLiteral("mysql"), QStringLiteral("MySQL")},
        {QStringLiteral("mariadb"), QStringLiteral("MariaDB")},
        {QStringLiteral("mongodb"), QStringLiteral("MongoDB")},
        {QStringLiteral("redis"), QStringLiteral("Redis")},
        {QStringLiteral("postgresql"), QStringLiteral("PostgreSQL")},
        {QStringLiteral("sqlserver"), QStringLiteral("SQL Server")},
        {QStringLiteral("oracle"), QStringLiteral("Oracle")}}}};
  return sections;
}
QString sectionIcon(const QString &category) {
  for (const auto &section : protocolSections())
    if (section.category == category)
      return section.icon;
  return QStringLiteral("app");
}
QString sectionForProtocol(const QString &protocol) {
  for (const auto &section : protocolSections())
    for (const auto &entry : section.protocols)
      if (entry.first == protocol)
        return section.category;
  return QStringLiteral("databases");
}
// Upstream ships a few application icons under names that differ from the
// application key in config.json.
QString applicationImageName(const QString &name) {
  static const QHash<QString, QString> aliases{
      {QStringLiteral("iterm"), QStringLiteral("item2")},
      {QStringLiteral("tigervnc"), QStringLiteral("tigerVnc")},
      {QStringLiteral("mongo_compass"), QStringLiteral("mongodb")},
      {QStringLiteral("securefx"), QStringLiteral("securecrt")},
      {QStringLiteral("vncviewer"), QStringLiteral("realvnc")},
      {QStringLiteral("royalts"), QStringLiteral("royalts")}};
  return aliases.value(name, name);
}
bool supportsProtocol(const QJsonObject &item, const QString &protocol) {
  for (const QJsonValue &value : item.value("protocol").toArray())
    if (value.toString() == protocol)
      return true;
  return false;
}
bool isDefaultApplication(const QJsonObject &item, const QString &protocol) {
  for (const QJsonValue &value : item.value("match_first").toArray())
    if (value.toString() == protocol)
      return true;
  return false;
}
QString localizedComment(const QJsonObject &item) {
  const QJsonObject comment = item.value("comment").toObject();
  const QString zh = comment.value("zh").toString();
  const QString en = comment.value("en").toString();
  if (zh.isEmpty())
    return en;
  if (en.isEmpty())
    return zh;
  return ClientTheme::text(zh.toUtf8().constData(), en.toUtf8().constData());
}
} // namespace
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
  body->addWidget(sidebar);
  // Keep the page container and indices stable for keyboard navigation and
  // existing automation; the native tab strip is replaced by the side menu.
  auto *tabs = new QTabWidget(this);
  tabs->setObjectName("settingsPages");
  tabs->tabBar()->hide();
  tabs->setDocumentMode(true);
  body->addWidget(tabs, 1);
  auto addPage = [this, tabs, navigation](QWidget *content, const char *titleZh,
                                    const char *titleEn, const char *descriptionZh,
                                    const char *descriptionEn,
                                    bool stretch = false) {
    const QString title = text(titleZh, titleEn);
    const QString description = text(descriptionZh, descriptionEn);
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
    m_translations.append([tabs, scroll, item, titleLabel, descriptionLabel,
                           titleZh, titleEn, descriptionZh, descriptionEn] {
      const QString translated = text(titleZh, titleEn);
      item->setText(translated);
      item->setToolTip(translated);
      tabs->setTabText(tabs->indexOf(scroll), translated);
      titleLabel->setText(translated);
      descriptionLabel->setText(text(descriptionZh, descriptionEn));
    });
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
  general->setObjectName("generalSettingsForm");
  auto *g = new QFormLayout(general);
  styleForm(g);
  addPage(general, "通用", "General", "语言、终端与桌面行为。",
          "Language, terminal and desktop preferences.");
  auto *language = addChoice(g, text("语言", "Language"), "appearance/language",
                             {{text("跟随系统", "System"), "system"},
                              {"简体中文", "zh"},
                              {"English", "en"}},
                             "system");
  connect(language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { retranslateUi(); });
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
  auto *proxyHeader = new QLabel(text("代理", "Proxy"), general);
  proxyHeader->setProperty("role", "groupLabel");
  g->addRow(proxyHeader);
  auto *proxyMode = addChoice(g, text("模式", "Mode"), "proxy/mode",
                             {{text("系统代理", "System proxy"), "system"},
                              {text("手动", "Manual"), "manual"},
                              {text("关闭", "Off"), "none"}},
                             "system");
  auto *proxyUrl = new QLineEdit(this);
  proxyUrl->setObjectName("proxyHttp");
  proxyUrl->setPlaceholderText(text("http://proxy.example.com:8080",
                                    "http://proxy.example.com:8080"));
  proxyUrl->setText(QSettings().value("proxy/http").toString());
  g->addRow(text("地址", "Address"), proxyUrl);
  alignField(g, proxyUrl);
  connect(proxyMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, proxyMode, proxyUrl] {
            proxyUrl->setEnabled(proxyMode->currentData() == "manual");
          });
  const bool manualInitially = proxyMode->currentData() == "manual";
  proxyUrl->setEnabled(manualInitially);
  auto persistProxy = [this, proxyMode, proxyUrl] {
    QSettings().setValue("proxy/mode", proxyMode->currentData());
    QSettings().setValue("proxy/http", proxyUrl->text().trimmed());
    emit preferencesChanged();
  };
  connect(proxyUrl, &QLineEdit::editingFinished, this, persistProxy);
  auto *appearance = new QFrame;
  appearance->setObjectName("appearanceSettingsForm");
  auto *a = new QFormLayout(appearance);
  styleForm(a);
  addPage(appearance, "外观", "Appearance", "选择适合你的主题、字体与强调色。",
          "Choose your theme, typeface and accent colors.");
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
  remote->setObjectName("remoteSettingsForm");
  auto *r = new QFormLayout(remote);
  styleForm(r);
  addPage(remote, "远程桌面", "Remote desktop", "调整桌面显示与重定向选项。",
          "Adjust the display and redirection options for remote sessions.");
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
  auto *al = new QHBoxLayout(apps);
  al->setContentsMargins(0, 0, 0, 0);
  al->setSpacing(0);
  addPage(apps, "本地应用", "Applications",
          "先选择连接协议，再为它指定默认客户端或已安装的应用。",
          "Pick a connection protocol, then choose its default client or point to an installed application.",
          true);
  const QJsonObject platformApplications = m_config->applications();
  auto *protocolColumn = new QWidget(apps);
  protocolColumn->setObjectName("settingsProtocolColumn");
  auto *pc = new QVBoxLayout(protocolColumn);
  pc->setContentsMargins(10, 10, 10, 10);
  pc->setSpacing(1);
  m_protocolGroup = new QButtonGroup(protocolColumn);
  m_protocolGroup->setExclusive(true);
  QPushButton *firstProtocol = nullptr;
  for (const auto &section : protocolSections()) {
    const QJsonArray items =
        platformApplications.value(section.category).toArray();
    QList<QPair<QString, QString>> protocols;
    for (const auto &entry : section.protocols) {
      if (hiddenProtocols().contains(entry.first))
        continue;
      bool configured = false;
      for (const auto &value : items)
        configured |= supportsProtocol(value.toObject(), entry.first);
      if (configured)
        protocols.append(entry);
    }
    if (protocols.isEmpty())
      continue;
    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(8, 8, 0, 1);
    heading->setSpacing(6);
    auto *headingIcon = new QLabel(protocolColumn);
    headingIcon->setFixedSize(14, 14);
    headingIcon->setAlignment(Qt::AlignCenter);
    headingIcon->setPixmap(ClientTheme::icon(section.icon).pixmap(QSize(14, 14)));
    heading->addWidget(headingIcon, 0, Qt::AlignVCenter);
    auto *headingLabel =
        new QLabel(text(section.titleZh.toUtf8().constData(),
                        section.titleEn.toUtf8().constData()),
                   protocolColumn);
    headingLabel->setProperty("role", "groupLabel");
    headingLabel->setProperty("sectionCategory", section.category);
    headingLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    heading->addWidget(headingLabel, 1, Qt::AlignVCenter);
    pc->addLayout(heading);
    for (const auto &entry : protocols) {
      auto *button = new QPushButton(entry.second, protocolColumn);
      button->setObjectName("applicationProtocol");
      button->setProperty("role", "protocolRow");
      button->setProperty("protocol", entry.first);
      button->setCheckable(true);
      button->setCursor(Qt::PointingHandCursor);
      button->setFocusPolicy(Qt::TabFocus);
      m_protocolGroup->addButton(button);
      connect(button, &QPushButton::clicked, this,
              [this] { reloadApplications(); });
      if (!firstProtocol)
        firstProtocol = button;
      pc->addWidget(button);
    }
  }
  pc->addStretch();
  auto *protocolScroll = new QScrollArea(apps);
  protocolScroll->setObjectName("applicationProtocols");
  protocolScroll->setFrameShape(QFrame::NoFrame);
  protocolScroll->setWidgetResizable(true);
  protocolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  protocolScroll->setWidget(protocolColumn);
  protocolScroll->setFixedWidth(190);
  protocolScroll->setMinimumHeight(320);
  al->addWidget(protocolScroll);
  auto *cardHost = new QWidget;
  cardHost->setObjectName("applicationCardHost");
  auto *ch = new QVBoxLayout(cardHost);
  ch->setContentsMargins(16, 14, 10, 14);
  ch->setSpacing(12);
  m_applicationCards = new QVBoxLayout;
  m_applicationCards->setSpacing(12);
  ch->addLayout(m_applicationCards);
  m_applicationError = new QLabel(cardHost);
  m_applicationError->setWordWrap(true);
  m_applicationError->setProperty("role", "validation");
  ch->addWidget(m_applicationError);
  ch->addStretch();
  auto *cardScroll = new QScrollArea(apps);
  cardScroll->setObjectName("applicationCardScroll");
  cardScroll->setFrameShape(QFrame::NoFrame);
  cardScroll->setWidgetResizable(true);
  cardScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  cardScroll->setWidget(cardHost);
  cardScroll->setMinimumHeight(320);
  al->addWidget(cardScroll, 1);
  if (firstProtocol)
    firstProtocol->setChecked(true);
  reloadApplications();
  auto *about = new QFrame;
  about->setObjectName("aboutSettings");
  auto *ab = new QVBoxLayout(about);
  ab->setContentsMargins(28, 32, 28, 32);
  ab->setSpacing(14);
  ab->addStretch();
  addPage(about, "关于", "About", "OSMClient 桌面客户端。", "The OSMClient desktop client.", true);
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
void SettingsDialog::retranslateUi() {
  setWindowTitle(text("设置", "Settings"));
  auto *sidebar = findChild<QFrame *>("settingsSidebar");
  for (auto *label : sidebar->findChildren<QLabel *>())
    if (label->property("role") == "dialogTitle") label->setText(text("设置", "Settings"));
  for (const auto &translate : m_translations) translate();
  auto *general = findChild<QFrame *>("generalSettingsForm");
  auto *appearance = findChild<QFrame *>("appearanceSettingsForm");
  auto *remote = findChild<QFrame *>("remoteSettingsForm");
  auto updateRows = [](QFrame *frame, const QStringList &labels) {
    auto *form = qobject_cast<QFormLayout *>(frame->layout());
    for (int index = 0; index < labels.size(); ++index) {
      auto *item = form->itemAt(index, QFormLayout::LabelRole);
      if (!item) item = form->itemAt(index, QFormLayout::SpanningRole);
      if (item) if (auto *label = qobject_cast<QLabel *>(item->widget())) label->setText(labels[index]);
    }
  };
  updateRows(general, {text("语言", "Language"), text("字符集", "Character set"),
      text("退格键发送 Ctrl+H", "Backspace sends Ctrl+H"), text("关闭主窗口", "Close main window"),
      text("下次启动恢复登录", "Restore login on startup"), text("代理", "Proxy"),
      text("模式", "Mode"), text("地址", "Address")});
  updateRows(appearance, {text("主题", "Theme"), text("字体", "Font"), text("字号", "Font size"),
      text("浅色主题色", "Light accent"), text("深色主题色", "Dark accent")});
  updateRows(remote, {text("分辨率", "Resolution"), text("色彩质量", "Color quality"),
      text("智能缩放", "Smart sizing"), text("键盘布局", "Keyboard layout"),
      text("全屏", "Full screen"), text("多显示器", "Multiple monitors"), text("磁盘映射", "Drive redirection")});
  for (auto *button : appearance->findChildren<QPushButton *>())
    button->setText(text("选择颜色…", "Choose color…"));
  auto updateChoices = [this](const QString &key, const QList<QPair<QString, QString>> &choices) {
    auto *combo = findChild<QComboBox *>(key);
    const QSignalBlocker blocker(combo);
    for (const auto &choice : choices) {
      const int index = combo->findData(choice.first);
      if (index >= 0) combo->setItemText(index, choice.second);
    }
  };
  updateChoices("appearance/language", {{"system", text("跟随系统", "System")}});
  updateChoices("connection/charset", {{"default", text("默认", "Default")}});
  updateChoices("general/closeAction", {{"quit", text("退出", "Quit")}, {"tray", text("最小化到托盘", "Hide to tray")}});
  updateChoices("proxy/mode", {{"system", text("系统代理", "System proxy")}, {"manual", text("手动", "Manual")}, {"none", text("关闭", "Off")}});
  updateChoices("appearance/theme", {{"system", text("跟随系统", "System")}, {"light", text("浅色", "Light")}, {"dark", text("深色", "Dark")}});
  updateChoices("connection/rdpResolution", {{"auto", text("自动", "Auto")}});
  updateChoices("connection/rdpSmartSize", {{"0", text("关闭", "Off")}, {"1", text("开启", "On")}});
  auto *keyboard = findChild<QComboBox *>("connection/keyboardLayout");
  const QStringList chinese{"美国英语 (QWERTY)", "英国英语 (QWERTY)", "日语 (QWERTY)", "法语 (AZERTY)", "瑞士法语 (QWERTZ)", "比利时法语 (AZERTY)", "土耳其语 Q (QWERTY)", "西班牙语", "拉丁美洲西班牙语", "德语 (QWERTZ)", "瑞士德语 (QWERTZ)", "瑞典语 (QWERTY)", "意大利语 (QWERTY)", "葡萄牙语 (QWERTY)", "巴西葡萄牙语 (QWERTY)", "波兰语 (QWERTY)", "匈牙利语 (QWERTZ)", "挪威语 (QWERTY)", "罗马尼亚语 (QWERTY)", "丹麦语 (QWERTY)", "加拿大法语 (QWERTY)"};
  const QStringList english{"US English (QWERTY)", "UK English (QWERTY)", "Japanese (QWERTY)", "French (AZERTY)", "Swiss French (QWERTZ)", "Belgian French (AZERTY)", "Turkish-Q (QWERTY)", "Spanish", "Spanish (Latin American)", "German (QWERTZ)", "Swiss German (QWERTZ)", "Swedish (QWERTY)", "Italian (QWERTY)", "Portuguese (QWERTY)", "Brazilian Portuguese (QWERTY)", "Polish (QWERTY)", "Hungarian (QWERTZ)", "Norwegian (QWERTY)", "Romanian (QWERTY)", "Danish (QWERTY)", "Canadian French (QWERTY)"};
  {
    const QSignalBlocker blocker(keyboard);
    for (int index = 0; index < keyboard->count(); ++index) {
      const QString value = keyboard->itemData(index).toString();
      const QString label = index < chinese.size() ? text(chinese[index].toUtf8().constData(), english[index].toUtf8().constData())
          : value.isEmpty() ? text("未指定（已保存）", "Unspecified (saved)") : text("已保存的布局：", "Saved layout: ") + value;
      keyboard->setItemText(index, label);
      keyboard->setItemData(index, label + "\n" + value, Qt::ToolTipRole);
    }
  }
  for (auto *label : findChildren<QLabel *>()) {
    const QString category = label->property("sectionCategory").toString();
    for (const auto &section : protocolSections())
      if (!category.isEmpty() && section.category == category)
        label->setText(text(section.titleZh.toUtf8().constData(), section.titleEn.toUtf8().constData()));
  }
  auto *about = findChild<QFrame *>("aboutSettings");
  for (auto *button : about->findChildren<QPushButton *>()) button->setText(text("下载客户端", "Download client"));
  auto *footer = findChild<QFrame *>("dialogFooter");
  for (auto *label : footer->findChildren<QLabel *>()) label->setText(text("更改会自动保存", "Changes are saved automatically"));
  footer->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->setText(text("关闭", "Close"));
  auto *cardScroll = findChild<QScrollArea *>("applicationCardScroll");
  const int scrollValue = cardScroll->verticalScrollBar()->value();
  reloadApplications();
  cardScroll->verticalScrollBar()->setValue(scrollValue);
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
QString SettingsDialog::selectedProtocol() const {
  const QAbstractButton *button =
      m_protocolGroup ? m_protocolGroup->checkedButton() : nullptr;
  return button ? button->property("protocol").toString() : QString();
}
QWidget *SettingsDialog::applicationCard(const QJsonObject &item,
                                         const QString &category,
                                         const QString &protocol) {
  const QString name = item.value("name").toString();
  const QString display = item.value("display_name").toString(name);
  const QString path = item.value("path").toString();
  const bool internal = item.value("is_internal").toBool();
  // Only macOS internal launchers resolve their application when they start, so
  // their path is not configurable. Windows and Linux keep a configurable binary.
  const bool managedPath = ConfigStore::platformKey() == QLatin1String("macos") &&
                           internal && protocol != QLatin1String("vnc");
  auto *card = new QFrame;
  card->setObjectName("appCard");
  auto makeBadge = [](const QString &label, QWidget *parent) {
    auto *badge = new QLabel(label, parent);
    badge->setProperty("role", "appBadge");
    badge->setAlignment(Qt::AlignCenter);
    QFont font = badge->font();
    font.setPointSizeF(qMax(8.0, font.pointSizeF() - 1.0));
    badge->setFont(font);
    badge->setFixedHeight(19);
    badge->setFixedWidth(QFontMetrics(font).horizontalAdvance(label) + 12);
    return badge;
  };
  auto *outer = new QVBoxLayout(card);
  outer->setContentsMargins(14, 12, 14, 12);
  outer->setSpacing(8);
  auto *header = new QHBoxLayout;
  header->setSpacing(12);
  auto *icon = new QLabel(card);
  icon->setProperty("role", "appIcon");
  icon->setFixedSize(40, 40);
  icon->setAlignment(Qt::AlignCenter);
  const QPixmap image =
      QPixmap(QStringLiteral(":/images/%1.png").arg(applicationImageName(name)));
  icon->setPixmap(image.isNull()
                      ? ClientTheme::icon(sectionIcon(category)).pixmap(QSize(22, 22))
                      : image.scaled(30, 30, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
  icon->setToolTip(display);
  header->addWidget(icon, 0, Qt::AlignTop);
  auto *info = new QVBoxLayout;
  info->setSpacing(6);
  auto *titleRow = new QHBoxLayout;
  titleRow->setSpacing(10);
  auto *title = new ClientTheme::ElidedLabel(display, card);
  title->setObjectName("applicationName");
  title->setProperty("role", "appName");
  titleRow->addWidget(title, 1);
  auto *toggle = new ClientTheme::Switch(card);
  toggle->setObjectName("applicationDefault");
  const bool preferred = isDefaultApplication(item, protocol);
  toggle->setChecked(preferred);
  // The helper only launches an application it can find, so a non-internal
  // entry without a path cannot become the default until the user locates it.
  const bool canEnable = internal || !path.trimmed().isEmpty();
  toggle->setEnabled(canEnable);
  toggle->setAccessibleName(text("默认客户端", "Default client"));
  toggle->setToolTip(
      !canEnable ? text("请先选择客户端程序路径。",
                        "Choose the application path first.")
                 : preferred ? text("当前协议的默认客户端",
                                    "Default client for this protocol")
                             : text("设为当前协议的默认客户端",
                                    "Set as the default client for this protocol"));
  connect(toggle, &QAbstractButton::toggled, this,
          [this, category, protocol, name](bool on) {
            const bool saved =
                on ? m_config->selectApplication(category, protocol, name)
                   : m_config->clearDefaultApplication(category, protocol);
            if (!saved)
              QMessageBox::warning(this, text("保存失败", "Save failed"),
                                   m_config->error());
            reloadApplications();
          });
  titleRow->addWidget(toggle, 0, Qt::AlignTop);
  info->addLayout(titleRow);
  auto *pathRow = new QHBoxLayout;
  pathRow->setSpacing(6);
  auto *chip = new ClientTheme::ElidedLabel(
      managedPath ? text("无需配置", "Managed automatically")
                  : (path.isEmpty() ? QStringLiteral("-") : path),
      card);
  chip->setObjectName("applicationPath");
  chip->setProperty("role", "appPath");
  chip->setContentsMargins(8, 3, 8, 3);
  chip->setFixedHeight(24);
  chip->setMaximumWidth(360);
  chip->setToolTip(managedPath
                       ? text("此客户端无需指定程序路径。",
                              "This client does not require a program path.")
                       : path);
  pathRow->addWidget(chip, 1);
  if (!managedPath && !path.isEmpty()) {
    auto *copy = new QToolButton(card);
    copy->setObjectName("applicationCopyPath");
    copy->setAutoRaise(true);
    copy->setCursor(Qt::PointingHandCursor);
    copy->setIconSize(QSize(14, 14));
    copy->setFixedSize(20, 20);
    copy->setIcon(ClientTheme::icon("copy"));
    copy->setToolTip(text("复制路径", "Copy path"));
    copy->setAccessibleName(copy->toolTip());
    pathRow->addWidget(copy, 0, Qt::AlignVCenter);
    connect(copy, &QToolButton::clicked, this, [this, copy, path] {
      QApplication::clipboard()->setText(path);
      copy->setIcon(ClientTheme::icon("check"));
      QTimer::singleShot(1200, copy, [copy] {
        copy->setIcon(ClientTheme::icon("copy"));
      });
    });
  }
  auto *browse = new QPushButton(
      managedPath || !path.isEmpty() ? text("浏览", "Browse")
                                     : text("选择路径", "Select path"),
      card);
  browse->setObjectName("applicationBrowse");
  browse->setProperty("quiet", true);
  browse->setEnabled(!managedPath);
  if (managedPath)
    browse->setToolTip(chip->toolTip());
  pathRow->addWidget(browse, 0, Qt::AlignVCenter);
  connect(browse, &QPushButton::clicked, this, [this, category, protocol, name] {
    const QString selected = QFileDialog::getOpenFileName(
        this, text("选择客户端程序", "Choose application"));
    if (selected.isEmpty())
      return;
    if (!m_config->selectApplication(category, protocol, name, selected))
      QMessageBox::warning(this, text("保存失败", "Save failed"),
                           m_config->error());
    reloadApplications();
  });
  info->addLayout(pathRow);
  header->addLayout(info, 1);
  outer->addLayout(header);
  auto *badges = new QHBoxLayout;
  badges->setSpacing(5);
  QStringList supported;
  for (const QJsonValue &value : item.value("protocol").toArray()) {
    const QString key = value.toString();
    if (!hiddenProtocols().contains(key))
      supported << key.toUpper();
  }
  // Badge widths are measured with the font they render with, so the pills never
  // clip. Long protocol lists collapse into a "+N" pill that keeps the full list
  // in its tooltip, which also keeps narrow windows readable.
  const int visible = qMin(supported.size(), 5);
  for (int i = 0; i < visible; ++i)
    badges->addWidget(makeBadge(supported.at(i), card));
  if (supported.size() > visible) {
    auto *more = makeBadge(QStringLiteral("+%1").arg(supported.size() - visible), card);
    more->setToolTip(supported.join(QStringLiteral(" · ")));
    badges->addWidget(more);
  }
  badges->addStretch();
  const QUrl download(item.value("download_url").toString());
  if (download.isValid() && !download.host().isEmpty() &&
      (download.scheme() == QLatin1String("http") ||
       download.scheme() == QLatin1String("https"))) {
    auto *downloadButton = new QPushButton(text("下载应用", "Download app"), card);
    downloadButton->setObjectName("applicationDownload");
    downloadButton->setProperty("soft", true);
    downloadButton->setIcon(ClientTheme::icon("download"));
    badges->addWidget(downloadButton, 0, Qt::AlignVCenter);
    connect(downloadButton, &QPushButton::clicked, this,
            [download] { QDesktopServices::openUrl(download); });
  }
  outer->addLayout(badges);
  const QString comment = localizedComment(item);
  if (!comment.isEmpty()) {
    auto *description = new QLabel(comment, card);
    description->setObjectName("applicationComment");
    description->setProperty("role", "appComment");
    description->setWordWrap(true);
    outer->addWidget(description);
  }
  return card;
}
void SettingsDialog::reloadApplications() {
  while (QLayoutItem *item = m_applicationCards->takeAt(0)) {
    if (auto *widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    }
    delete item;
  }
  const QString protocol = selectedProtocol();
  const QString category = sectionForProtocol(protocol);
  const QJsonObject applications = m_config->applications();
  int shown = 0;
  bool hasDefault = false;
  for (const auto &value : applications.value(category).toArray()) {
    const QJsonObject item = value.toObject();
    if (!supportsProtocol(item, protocol))
      continue;
    hasDefault |= isDefaultApplication(item, protocol);
    m_applicationCards->addWidget(applicationCard(item, category, protocol));
    ++shown;
  }
  if (!m_config->error().isEmpty())
    m_applicationError->setText(m_config->error());
  else if (shown == 0)
    m_applicationError->setText(
        text("当前协议没有配置本地应用。",
             "No local applications are configured for this protocol."));
  else if (!hasDefault)
    m_applicationError->setText(
        text("当前协议还没有默认客户端，连接该协议的资产会失败。请打开一个应用的开关。",
             "This protocol has no default client, so connections using it will "
             "fail. Turn on one of the applications above."));
  else
    m_applicationError->clear();
  m_applicationError->setVisible(!m_applicationError->text().isEmpty());
}
