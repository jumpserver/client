#include "pluginsettings.h"
#include "task.h"
#include "theme.h"

#include <QHBoxLayout>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace Replay {
using ClientTheme::text;

struct PluginSettings::Private {
  explicit Private(PluginSettings *widget) : task(widget) {
    widget->setObjectName("replayPluginSettings");
    auto *outer = new QVBoxLayout(widget);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *card = new QFrame(widget);
    card->setProperty("surface", true);
    card->setObjectName("replayPluginCard");
    outer->addWidget(card);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *header = new QHBoxLayout;
    name = new QLabel(card);
    name->setObjectName("replayPluginName");
    name->setProperty("role", "sectionTitle");
    name->setWordWrap(true);
    header->addWidget(name, 1);
    pluginState = new QLabel(card);
    pluginState->setObjectName("replayPluginState");
    pluginState->setProperty("role", "replayBadge");
    header->addWidget(pluginState, 0, Qt::AlignTop);
    layout->addLayout(header);
    status = new QLabel(card);
    status->setObjectName("replayPluginDescription");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setProperty("muted", true);
    layout->addWidget(status);
    install = new QPushButton(card);
    install->setObjectName("downloadReplayPlugin");
    install->setProperty("primary", true);
    import = new QPushButton(card);
    import->setObjectName("importReplayPlugin");
    cancel = new QPushButton(card);
    cancel->hide();
    progressRow = new QWidget(card);
    progressRow->setObjectName("replayPluginProgressRow");
    auto *progressLayout = new QHBoxLayout(progressRow);
    progressLayout->setContentsMargins(0, 4, 0, 4);
    progressLayout->setSpacing(10);
    progress = new QProgressBar(progressRow);
    progress->setObjectName("replayPluginProgress");
    progress->setRange(0, 100);
    progress->setTextVisible(false);
    progressLabel = new QLabel(progressRow);
    progressLabel->setObjectName("replayPluginProgressLabel");
    progressLabel->setProperty("role", "caption");
    progressLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progressLabel->setMinimumWidth(progressLabel->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 8);
    progressLayout->addWidget(progress, 1);
    progressLayout->addWidget(progressLabel);
    progressRow->hide();
    layout->addWidget(progressRow);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addWidget(install);
    buttons->addWidget(import);
    buttons->addWidget(cancel);
    buttons->addStretch();
    layout->addLayout(buttons);
    QObject::connect(cancel, &QPushButton::clicked, widget, [this] { task.cancel(); });
    const auto start = [this](const QString &archive) {
      if (task.busy()) return;
      progress->setValue(0);
      progressLabel->setText(QStringLiteral("0%"));
      progressRow->show();
      cancel->show();
      install->hide();
      import->hide();
      status->setText(archive.isEmpty() ? text("正在下载转码插件…", "Downloading the transcoding plugin…") :
          text("正在检查插件包…", "Checking the plugin package…"));
      task.start([archive](const Canceled &c, const Progress &p) {
            if (archive.isEmpty()) FfmpegPlugin::install(FfmpegPlugin::root(), c, p);
            else FfmpegPlugin::importArchive(archive, FfmpegPlugin::root(), c, p);
          },
          [this, archive](const QString &error, bool canceled) {
            progressRow->hide();
            cancel->hide();
            install->show();
            import->show();
            refresh();
            if (canceled) {
              status->setText(status->text() + (archive.isEmpty()
                  ? text(" 操作已取消，再次下载会继续。", " Operation canceled. Downloading again will resume.")
                  : text(" 操作已取消，未安装插件包。", " Operation canceled. No plugin package was installed.")));
            } else if (!error.isEmpty()) {
              if (archive.isEmpty()) {
                status->setText(status->text() + '\n' + error);
              } else {
                qWarning().noquote() << "FFmpeg plugin import failed:" << error;
                status->setText(status->text() + '\n' + text(
                    "导入失败。请检查插件包是否完整、来自可信来源并适用于当前系统。"
                    "若仍无法导入，请重新获取插件包后再试。",
                    "Import failed. Check that the package is complete, comes from a trusted source, "
                    "and is compatible with this system. If it still fails, obtain the package again."));
              }
            }
          }, [this](int percent) {
            progress->setValue(percent);
            progressLabel->setText(QString::number(percent) + '%');
            if (percent >= 98 && percent < 100)
              status->setText(text("正在确认插件可以正常使用…", "Checking that the plugin works…"));
          });
    };
    QObject::connect(install, &QPushButton::clicked, widget, [start] { start({}); });
    QObject::connect(import, &QPushButton::clicked, widget, [widget, start] {
      const auto path = QFileDialog::getOpenFileName(widget, text("选择离线插件包", "Choose offline plugin package"),
          QString(), text("插件包 (*.tar)", "Plugin package (*.tar)"));
      if (!path.isEmpty()) {
        QMessageBox prompt(QMessageBox::Warning,
            text("导入离线插件包", "Import offline plugin package"),
            text("请确认插件包来自可信来源。安装前，客户端会检查插件包是否完整、"
                 "适用于当前系统并可以正常转码；检查未通过时不会安装。\n\n文件：%1",
                 "Confirm that the package comes from a trusted source. Before installation, "
                 "the client checks that it is complete, compatible with this system, and able "
                 "to transcode recordings. Nothing is installed if a check fails.\n\nFile: %1")
                .arg(QFileInfo(path).fileName()),
            QMessageBox::NoButton, widget);
        prompt.setObjectName("offlinePluginImportPrompt");
        prompt.setTextFormat(Qt::PlainText);
        auto *continueButton = prompt.addButton(text("继续导入", "Continue"), QMessageBox::AcceptRole);
        auto *cancelButton = prompt.addButton(text("取消", "Cancel"), QMessageBox::RejectRole);
        continueButton->setObjectName("continueOfflinePluginImport");
        cancelButton->setObjectName("cancelOfflinePluginImport");
        prompt.setDefaultButton(cancelButton);
        prompt.setEscapeButton(cancelButton);
        prompt.exec();
        if (prompt.clickedButton() == continueButton) start(path);
      }
    });
  }

  void refresh() {
    if (task.busy()) return;
    const bool supported = FfmpegPlugin::supported();
    const auto plugin = FfmpegPlugin::installed();
    const bool installed = bool(plugin);
    name->setText(text("录像转码插件（FFmpeg）", "Recording transcoding plugin (FFmpeg)"));
    install->setText(installed ? text("重新下载", "Download again") : text("下载", "Download"));
    install->setEnabled(supported);
    import->setText(text("离线导入", "Import offline"));
    import->setEnabled(supported);
    cancel->setText(text("取消", "Cancel"));
    QString label;
    if (!supported) label = text("用于将 RDP/VNC 协议录像转为 MP4；当前系统暂不支持下载。",
                                 "Converts RDP/VNC protocol recordings to MP4; unavailable for this platform.");
    else label = text("用于将 RDP/VNC 协议录像转为 MP4，离线播放无需此插件。",
                      "Converts RDP/VNC protocol recordings to MP4. The plugin is not needed for offline playback.");
    status->setText(label);
    pluginState->setText(!supported ? text("不可用", "Unavailable") :
        installed ? text("已安装 %1", "Installed %1").arg(plugin.installedVersion) : text("未安装", "Not installed"));
    pluginState->setProperty("tone", installed ? "success" : "neutral");
    pluginState->style()->unpolish(pluginState);
    pluginState->style()->polish(pluginState);
  }

  Task task;
  QLabel *name, *status, *progressLabel, *pluginState;
  QWidget *progressRow;
  QProgressBar *progress;
  QPushButton *install, *import, *cancel;
};

PluginSettings::PluginSettings(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>(this)) { retranslate(); }
PluginSettings::~PluginSettings() = default;
void PluginSettings::retranslate() { d->refresh(); }

} // namespace Replay
