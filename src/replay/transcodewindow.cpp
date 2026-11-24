#include "transcodewindow.h"
#include "presentation.h"
#include "task.h"
#include "transcode.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLockFile>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <vector>

namespace Replay {
namespace {
using ClientTheme::text;
using ClientTheme::ElidedLabel;
constexpr int widePlayLabelMinimum = 900;
constexpr int defaultConcurrency = 3;
constexpr int maxConcurrency = 6;

QStringList recordings(const QMimeData *mime) {
  QStringList paths;
  for (const auto &url : mime->urls()) {
    if (!url.isLocalFile() || !url.host().isEmpty()) continue;
    const auto path = url.toLocalFile();
    if (path.endsWith(".replay.gz", Qt::CaseInsensitive) || path.endsWith(".tar", Qt::CaseInsensitive))
      paths.append(path);
  }
  return paths;
}

QString unusedOutput(const QString &filename, const QDir &directory, const QSet<QString> &reserved) {
  const auto stem = filename.chopped(4);
  auto path = directory.filePath(filename);
  const auto unavailable = [&reserved](const QString &candidate) {
    return QFileInfo::exists(candidate) || QFileInfo(candidate).isSymLink() ||
        reserved.contains(QFileInfo(candidate).absoluteFilePath());
  };
  for (int i = 2; unavailable(path); ++i)
    path = directory.filePath(QStringLiteral("%1 (%2).mp4").arg(stem).arg(i));
  return path;
}

QPushButton *quietButton(QWidget *parent, const QString &icon = {}) {
  auto *button = new QPushButton(parent);
  button->setProperty("quiet", true);
  if (!icon.isEmpty()) button->setIcon(ClientTheme::icon(icon));
  return button;
}

QLabel *badge(QWidget *parent, const char *tone) {
  auto *label = new QLabel(parent);
  label->setProperty("role", "replayBadge");
  label->setProperty("tone", tone);
  return label;
}

QIcon primaryPlayIcon(QWidget *widget) {
  const auto source = widget->style()->standardIcon(QStyle::SP_MediaPlay).pixmap(16, 16);
  const auto tinted = [&source](const QColor &color) {
    QPixmap pixmap = source;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
    return pixmap;
  };
  QIcon result;
  result.addPixmap(tinted(Qt::white), QIcon::Normal);
  result.addPixmap(tinted(ClientTheme::colors().muted), QIcon::Disabled);
  return result;
}

ElidedLabel *caption(QWidget *parent) {
  auto *label = new ElidedLabel({}, parent);
  label->setProperty("role", "caption");
  label->setTextFormat(Qt::PlainText);
  return label;
}
} // namespace

struct TranscodeWindow::Private {
  enum class State { Reading, Queued, Running, Succeeded, Failed, Canceled };
  struct Card {
    QListWidgetItem *item = nullptr;
    QFrame *frame = nullptr;
    ElidedLabel *title = nullptr, *summary = nullptr, *output = nullptr;
    QLabel *state = nullptr, *elapsed = nullptr, *percent = nullptr, *icon = nullptr;
    QPushButton *details = nullptr, *rename = nullptr, *play = nullptr, *remove = nullptr;
    QProgressBar *progress = nullptr;
  };
  struct Job {
    QString input, output, error, inspectionError, customName;
    RecordingInfo info;
    QSize dimensions;
    int duration = -1, percent = -1;
    qint64 elapsed = -1;
    State state = State::Reading;
    Card card;
  };

  Private(TranscodeWindow *window, std::function<void()> openPluginSettings,
          std::function<void(const QString &)> openOutput)
      : w(window), playOutput(std::move(openOutput)), inspection(w) {
    for (int i = 0; i < maxConcurrency; ++i) workers.emplace_back(std::make_unique<Task>(w));
    w->setObjectName("replayTranscodeWindow");
    w->setProperty("replayPage", true);
    w->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    w->setAcceptDrops(true);
    w->resize(1080, 720);
    w->setMinimumSize(730, 520);
    auto *layout = new QVBoxLayout(w);
    layout->setContentsMargins(22, 18, 22, 18);
    auto *panel = new QFrame(w);
    panel->setProperty("surface", true);
    layout->addWidget(panel);
    auto *body = new QVBoxLayout(panel);
    body->setContentsMargins(18, 15, 18, 15);
    body->setSpacing(12);

    auto *header = new QHBoxLayout;
    headerIcon = new QLabel(panel);
    headerIcon->setPixmap(ClientTheme::icon("replay-transcode", ClientTheme::colors().accent).pixmap(16));
    title = new QLabel(panel);
    title->setProperty("role", "dialogTitle");
    outputOptions = quietButton(panel, "settings");
    outputOptions->setObjectName("replayOutputOptions");
    start = new QPushButton(panel);
    start->setObjectName("startReplayTranscode");
    start->setProperty("primary", true);
    start->setIcon(primaryPlayIcon(w));
    cancel = new QPushButton(panel);
    cancel->setObjectName("cancelReplayTranscode");
    header->addWidget(headerIcon);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(outputOptions);
    header->addWidget(start);
    header->addWidget(cancel);
    body->addLayout(header);
    description = new QLabel(panel);
    description->setProperty("muted", true);
    description->setWordWrap(true);
    body->addWidget(description);

    auto *badges = new QHBoxLayout;
    total = badge(panel, "accent");
    queued = badge(panel, "warning");
    succeeded = badge(panel, "success");
    failed = badge(panel, "error");
    canceled = badge(panel, "neutral");
    for (auto *label : {total, queued, succeeded, failed, canceled}) badges->addWidget(label);
    badges->addStretch();
    body->addLayout(badges);

    options = new QDialog(w);
    options->setObjectName("replayOutputSettingsDialog");
    options->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    options->setMinimumWidth(480);
    options->resize(520, 0);
    auto *optionLayout = new QVBoxLayout(options);
    optionLayout->setContentsMargins(24, 22, 24, 20);
    optionLayout->setSpacing(0);
    optionHeading = new QLabel(options);
    optionHeading->setProperty("role", "dialogTitle");
    optionLayout->addWidget(optionHeading);
    optionDescription = new QLabel(options);
    optionDescription->setProperty("role", "caption");
    optionDescription->setWordWrap(true);
    optionLayout->addSpacing(5);
    optionLayout->addWidget(optionDescription);
    optionLayout->addSpacing(22);
    auto *outputRow = new QHBoxLayout;
    outputRow->setSpacing(10);
    directoryLabel = new QLabel(options);
    directoryLabel->setProperty("role", "sectionTitle");
    optionLayout->addWidget(directoryLabel);
    optionLayout->addSpacing(8);
    directory = new QLineEdit(options);
    directory->setObjectName("replayOutputDirectory");
    directory->setReadOnly(true);
    auto defaultDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!QFileInfo(defaultDirectory).isDir()) defaultDirectory = QDir::homePath();
    directory->setText(QDir::toNativeSeparators(defaultDirectory));
    directory->setText(QSettings().value("replay/outputDirectory", directory->text()).toString());
    directoryLabel->setBuddy(directory);
    choose = new QPushButton(options);
    choose->setIcon(ClientTheme::icon("folder"));
    outputRow->addWidget(directory, 1);
    outputRow->addWidget(choose);
    optionLayout->addLayout(outputRow);
    optionLayout->addSpacing(3);
    directoryHint = new QLabel(options);
    directoryHint->setProperty("role", "caption");
    optionLayout->addWidget(directoryHint);
    optionLayout->addSpacing(14);
    filenameLabel = new QLabel(options);
    filenameLabel->setProperty("role", "sectionTitle");
    filename = new ClientTheme::ComboBox(options);
    filename->setObjectName("replayFilenameStyle");
    filenameLabel->setBuddy(filename);
    resolutionLabel = new QLabel(options);
    resolutionLabel->setProperty("role", "sectionTitle");
    resolution = new ClientTheme::ComboBox(options);
    resolution->setObjectName("replayOutputResolution");
    resolutionLabel->setBuddy(resolution);
    concurrencyLabel = new QLabel(options);
    concurrencyLabel->setProperty("role", "sectionTitle");
    concurrency = new QSpinBox(options);
    concurrency->setObjectName("replayTranscodeConcurrency");
    concurrency->setRange(1, maxConcurrency);
    concurrency->setValue(qBound(1, QSettings().value("replay/concurrency", defaultConcurrency).toInt(),
                                 maxConcurrency));
    concurrency->setFixedWidth(112);
    concurrencyLabel->setBuddy(concurrency);
    concurrencyHint = new QLabel(options);
    concurrencyHint->setProperty("role", "caption");
    concurrencyHint->setWordWrap(true);
    optionLayout->addWidget(filenameLabel);
    optionLayout->addSpacing(8);
    optionLayout->addWidget(filename);
    optionLayout->addSpacing(18);
    optionLayout->addWidget(resolutionLabel);
    optionLayout->addSpacing(8);
    optionLayout->addWidget(resolution);
    optionLayout->addSpacing(18);
    optionLayout->addWidget(concurrencyLabel);
    optionLayout->addSpacing(8);
    optionLayout->addWidget(concurrency, 0, Qt::AlignLeft);
    optionLayout->addSpacing(6);
    optionLayout->addWidget(concurrencyHint);
    optionError = new QLabel(options);
    optionError->setTextFormat(Qt::PlainText);
    optionError->setWordWrap(true);
    optionError->setProperty("role", "validation");
    optionLayout->addWidget(optionError);
    optionLayout->addSpacing(20);
    auto *divider = new QFrame(options);
    divider->setFrameShape(QFrame::HLine);
    divider->setObjectName("replaySettingsDivider");
    optionLayout->addWidget(divider);
    optionLayout->addSpacing(16);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    optionCancel = new QPushButton(options);
    optionCancel->setObjectName("cancelReplayOutputSettings");
    optionSave = new QPushButton(options);
    optionSave->setObjectName("saveReplayOutputSettings");
    optionSave->setProperty("primary", true);
    optionCancel->setMinimumWidth(86);
    optionSave->setMinimumWidth(86);
    buttons->addStretch();
    buttons->addWidget(optionCancel);
    buttons->addWidget(optionSave);
    optionLayout->addLayout(buttons);

    auto *queueHeader = new QHBoxLayout;
    queueTitle = new QLabel(panel);
    queueTitle->setProperty("role", "sectionTitle");
    add = quietButton(panel, "plus");
    add->setObjectName("addReplayTranscodeFiles");
    add->setShortcut(QKeySequence::Open);
    clear = quietButton(panel, "trash");
    queueHeader->addWidget(queueTitle);
    queueHeader->addStretch();
    queueHeader->addWidget(add);
    queueHeader->addWidget(clear);
    body->addLayout(queueHeader);

    pages = new QStackedWidget(panel);
    drop = new QWidget(pages);
    drop->setObjectName("replayDropZone");
    drop->setAttribute(Qt::WA_StyledBackground);
    auto *dropLayout = new QVBoxLayout(drop);
    dropIcon = new QLabel(drop);
    dropIcon->setPixmap(ClientTheme::icon("folder").pixmap(32));
    dropTitle = new QLabel(drop);
    dropTitle->setObjectName("replayTranscodeEmptyTitle");
    dropTitle->setProperty("role", "sectionTitle");
    dropDescription = new QLabel(drop);
    dropDescription->setObjectName("replayTranscodeEmptyDescription");
    dropDescription->setProperty("muted", true);
    dropDescription->setWordWrap(true);
    dropDescription->setAlignment(Qt::AlignCenter);
    dropAdd = new QPushButton(drop);
    dropAdd->setObjectName("addReplayTranscodeFromDropZone");
    dropAdd->setProperty("soft", true);
    installPlugin = new QPushButton(drop);
    installPlugin->setObjectName("openReplayPluginSettings");
    installPlugin->setProperty("primary", true);
    dropLayout->addStretch();
    dropLayout->addWidget(dropIcon, 0, Qt::AlignHCenter);
    dropLayout->addWidget(dropTitle, 0, Qt::AlignHCenter);
    dropLayout->addWidget(dropDescription);
    dropLayout->addWidget(dropAdd, 0, Qt::AlignHCenter);
    dropLayout->addWidget(installPlugin, 0, Qt::AlignHCenter);
    dropLayout->addStretch();
    pages->addWidget(drop);
    list = new QListWidget(pages);
    list->setObjectName("replayTranscodeQueue");
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setSpacing(3);
    list->setResizeMode(QListView::Adjust);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pages->addWidget(list);
    body->addWidget(pages, 1);

    status = new QLabel(panel);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setProperty("muted", true);
    body->addWidget(status);

    QObject::connect(outputOptions, &QPushButton::clicked, w, [this] {
      const auto oldDirectory = directory->text();
      const int oldFilename = filename->currentIndex(), oldResolution = resolution->currentIndex(),
                oldConcurrency = concurrency->value();
      optionError->clear();
      if (options->exec() == QDialog::Accepted) {
        QSettings settings;
        settings.setValue("replay/outputDirectory", directory->text());
        settings.setValue("replay/filenameStyle", filename->currentData());
        settings.setValue("replay/outputResolution", resolution->currentData());
        settings.setValue("replay/concurrency", concurrency->value());
      } else {
        directory->setText(oldDirectory);
        filename->setCurrentIndex(oldFilename);
        resolution->setCurrentIndex(oldResolution);
        concurrency->setValue(oldConcurrency);
      }
      for (auto &job : jobs) updateCard(job);
      refresh();
    });
    QObject::connect(optionCancel, &QPushButton::clicked, options, &QDialog::reject);
    QObject::connect(optionSave, &QPushButton::clicked, options, [this] {
      const QFileInfo folder(directory->text());
      if (!folder.isDir() || !folder.isWritable()) {
        optionError->setText(text("请选择有写入权限的输出目录。", "Choose a writable output folder."));
        return;
      }
      options->accept();
    });
    QObject::connect(choose, &QPushButton::clicked, w, [this] {
      const auto path = QFileDialog::getExistingDirectory(w, text("选择输出目录", "Choose output folder"), directory->text());
      if (!path.isEmpty()) directory->setText(QDir::toNativeSeparators(path));
    });
    QObject::connect(installPlugin, &QPushButton::clicked, w, [this, openPluginSettings] {
      openPluginSettings();
      refreshPlugin();
      refresh();
    });
    const auto pickFiles = [this] {
      const bool pendingBatch = batchActive && !stopping;
      if (pendingBatch) ++pendingPickers;
      const auto paths = QFileDialog::getOpenFileNames(w, text("添加录像", "Add recordings"),
          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
          text("离线录像 (*.tar *.replay.gz)", "Offline recordings (*.tar *.replay.gz)"));
      addPaths(paths);
      if (pendingBatch) {
        --pendingPickers;
        finishBatchIfIdle();
      }
    };
    QObject::connect(add, &QPushButton::clicked, w, pickFiles);
    QObject::connect(dropAdd, &QPushButton::clicked, w, pickFiles);
    QObject::connect(clear, &QPushButton::clicked, w, [this] {
      if (batchActive) return;
      jobs.clear();
      list->clear();
      refresh();
    });
    QObject::connect(start, &QPushButton::clicked, w, [this] { startBatch(); });
    QObject::connect(cancel, &QPushButton::clicked, w, [this] {
      stopping = true;
      inspection.cancel();
      for (auto &worker : workers) worker->cancel();
      for (auto &job : jobs) {
        if (job.state != State::Queued && job.state != State::Reading) continue;
        job.state = State::Canceled;
        updateCard(job);
      }
      finishBatchIfIdle();
      refresh();
    });
  }

  Job *findJob(QListWidgetItem *item) {
    for (auto &job : jobs) if (job.card.item == item) return &job;
    return nullptr;
  }

  void addCard(Job &job) {
    auto &c = job.card;
    c.item = new QListWidgetItem(list);
    c.frame = new QFrame(list);
    c.frame->setObjectName("replayTranscodeCard");
    c.frame->setProperty("replayCard", true);
    auto *body = new QVBoxLayout(c.frame);
    body->setContentsMargins(11, 8, 11, 8);
    body->setSpacing(5);
    auto *top = new QHBoxLayout;
    c.icon = new QLabel(c.frame);
    c.icon->setProperty("role", "replayIcon");
    c.icon->setPixmap(ClientTheme::icon("video").pixmap(16));
    c.title = new ElidedLabel({}, c.frame);
    c.title->setProperty("role", "sectionTitle");
    c.title->setTextFormat(Qt::PlainText);
    c.details = quietButton(c.frame, "info");
    c.details->setObjectName("replayQueueMetadata");
    c.details->setProperty("iconOnly", true);
    c.details->setFixedSize(28, 28);
    c.state = badge(c.frame, "neutral");
    c.elapsed = badge(c.frame, "neutral");
    c.remove = quietButton(c.frame, "close");
    c.remove->setObjectName("removeReplayTranscodeJob");
    c.remove->setProperty("iconOnly", true);
    c.remove->setFixedSize(28, 28);
    top->addWidget(c.icon);
    top->addWidget(c.title, 1);
    top->addWidget(c.details);
    top->addWidget(c.elapsed);
    top->addWidget(c.state);
    top->addWidget(c.remove);
    body->addLayout(top);
    c.summary = caption(c.frame);
    c.summary->setObjectName("replayQueueMetadataSummary");
    body->addWidget(c.summary);
    auto *outputRow = new QHBoxLayout;
    c.output = caption(c.frame);
    c.output->setObjectName("replayQueueOutput");
    c.output->setProperty("role", "replayOutput");
    c.rename = quietButton(c.frame, "edit");
    c.rename->setObjectName("renameReplayTranscodeOutput");
    c.play = quietButton(c.frame);
    c.play->setIcon(ClientTheme::icon("play"));
    c.play->setIconSize({16, 16});
    c.play->setObjectName("playReplayTranscodeOutput");
    c.rename->setProperty("iconOnly", true);
    c.rename->setFixedSize(28, 28);
    outputRow->addWidget(c.output, 1);
    outputRow->addWidget(c.rename);
    outputRow->addWidget(c.play);
    body->addLayout(outputRow);
    c.progress = new QProgressBar(c.frame);
    c.progress->setObjectName("replayQueueProgress");
    c.progress->setRange(0, 100);
    c.progress->setTextVisible(false);
    c.percent = new QLabel(c.frame);
    c.percent->setObjectName("replayQueuePercent");
    c.percent->setProperty("role", "caption");
    c.percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    c.percent->setFixedWidth(c.percent->fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 8);
    auto *progressRow = new QHBoxLayout;
    progressRow->addWidget(c.progress, 1);
    progressRow->addWidget(c.percent);
    body->addLayout(progressRow);
    list->setItemWidget(c.item, c.frame);

    QObject::connect(c.remove, &QPushButton::clicked, w, [this, item = c.item] {
      if (batchActive) return;
      for (int row = 0; row < jobs.size(); ++row) {
        if (jobs.at(row).card.item != item) continue;
        jobs.removeAt(row);
        delete list->takeItem(row);
        break;
      }
      refresh();
    });
    QObject::connect(c.details, &QPushButton::clicked, w, [this, item = c.item, button = c.details] {
      if (const auto *job = findJob(item))
        showDetails(button, detailFields(*job));
    });
    QObject::connect(c.rename, &QPushButton::clicked, w, [this, item = c.item] {
      auto *job = findJob(item);
      if (!job || batchActive) return;
      const auto hint = predictedName(*job);
      QInputDialog dialog(w);
      dialog.setWindowTitle(text("设置输出文件名", "Set output filename"));
      dialog.setLabelText(text("文件名（留空使用命名规则）", "Filename (leave blank to use the naming rule)"));
      dialog.setTextValue(job->customName.isEmpty() ? hint : job->customName);
      dialog.setOkButtonText(text("保存", "Save"));
      dialog.setCancelButtonText(text("取消", "Cancel"));
      const auto available = ClientTheme::dialogAvailableGeometry(&dialog, true);
      dialog.setMinimumWidth(qMin(560, qMax(320, available.width() - 48)));
      dialog.resize(dialog.minimumWidth(), dialog.sizeHint().height());
      if (dialog.exec() == QDialog::Accepted && (job = findJob(item))) {
        job->customName = dialog.textValue().left(256);
        updateCard(*job);
      }
    });
    QObject::connect(c.play, &QPushButton::clicked, w, [this, item = c.item] {
      if (const auto *job = findJob(item); job && job->state == State::Succeeded) playOutput(job->output);
    });
  }

  QString predictedName(const Job &job) const {
    return outputFilename(job.info, static_cast<FilenameStyle>(filename->currentData().toInt()), job.customName);
  }

  void addPaths(const QStringList &paths) {
    if (!pluginReady) return;
    if (stopping && batchActive) return;
    for (const auto &path : paths) {
      const QFileInfo file(path);
      if (!file.isFile()) continue;
      const auto input = file.absoluteFilePath();
      if (std::any_of(jobs.cbegin(), jobs.cend(), [&input](const Job &job) { return job.input == input; })) continue;
      Job job;
      job.input = input;
      job.info.label = file.fileName();
      jobs.append(job);
      addCard(jobs.last());
      updateCard(jobs.last());
    }
    inspectNext();
    refresh();
  }

  void inspectNext() {
    if (inspection.busy()) return;
    for (const auto &job : jobs) {
      if (job.state != State::Reading) continue;
      const auto path = job.input;
      const auto unsupported = text("只能转码完整的 RDP/VNC 协议录像。",
                                    "Only complete RDP/VNC protocol recordings can be transcoded.");
      auto info = std::make_shared<RecordingInfo>();
      inspection.start([path, info](const Canceled &stop, const Progress &) { *info = Replay::inspect(path, stop); },
          [this, path, info, unsupported](const QString &error, bool stopped) {
        for (auto &job : jobs) {
          if (job.input != path || job.state != State::Reading) continue;
          if (!info->label.isEmpty()) job.info = *info;
          job.inspectionError = !error.isEmpty() ? error :
              info->kind != Kind::Guacamole || info->fragment ? unsupported : QString();
          job.error = job.inspectionError;
          job.state = (stopped || (batchActive && stopping)) ? State::Canceled : job.error.isEmpty() ? State::Queued : State::Failed;
          updateCard(job);
          break;
        }
        inspectNext();
        if (batchActive) scheduleJobs();
        finishBatchIfIdle();
        refresh();
      });
      break;
    }
  }

  void startBatch() {
    if (batchActive || inspection.busy()) return;
    batchError.clear();
    refreshPlugin();
    const QFileInfo folder(directory->text());
    if (!pluginReady || !folder.isDir() || !folder.isWritable()) {
      refresh();
      return;
    }
    if (!acquirePluginLock()) return;
    stopping = false;
    batchActive = true;
    reservedOutputs.clear();
    for (auto &job : jobs) {
      if (job.state == State::Succeeded || !job.inspectionError.isEmpty()) continue;
      job.state = State::Queued;
      job.percent = -1;
      job.error.clear();
      job.output.clear();
      job.elapsed = -1;
      updateCard(job);
    }
    scheduleJobs();
    finishBatchIfIdle();
    refresh();
  }

  bool acquirePluginLock() {
    if (batchLock && batchLock->isLocked()) return true;
    auto lock = std::make_unique<QLockFile>(QDir(FfmpegPlugin::root()).filePath("install.lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
      batchError = text("转码插件正在执行其他任务，请稍后重试。",
                        "The transcoding plugin is busy. Try again shortly.");
      refresh();
      return false;
    }
    batchLock = std::move(lock);
    return true;
  }

  void releasePluginLock() {
    if (!batchLock) return;
    batchLock->unlock();
    batchLock.reset();
  }

  int configuredConcurrency() const {
    return qBound(1, QSettings().value("replay/concurrency", defaultConcurrency).toInt(), maxConcurrency);
  }

  int nextQueuedJob() const {
    for (int row = 0; row < jobs.size(); ++row)
      if (jobs.at(row).state == State::Queued) return row;
    return -1;
  }

  void scheduleJobs() {
    if (!batchActive) return;
    const int limit = configuredConcurrency();
    for (auto &worker : workers) {
      if (!batchActive || runningRows.size() >= limit) break;
      if (worker->busy()) continue;
      const int row = nextQueuedJob();
      if (row < 0) break;
      auto &job = jobs[row];
      if (stopping) {
        job.state = State::Canceled;
        updateCard(job);
        continue;
      }
      const auto folder = QDir(directory->text());
      const auto style = static_cast<FilenameStyle>(filename->currentData().toInt());
      const auto size = static_cast<Resolution>(resolution->currentData().toInt());
      const auto pluginRoot = FfmpegPlugin::root();
      const auto destination = unusedOutput(outputFilename(job.info, style, job.customName), folder, reservedOutputs);
      const auto unsupported = text("只能转码完整的 RDP/VNC 协议录像。",
                                    "Only complete RDP/VNC protocol recordings can be transcoded.");
      struct Result {
        QString path;
        RecordingInfo info;
        QSize dimensions;
        int duration = -1;
      };
      auto result = std::make_shared<Result>();
      result->path = destination;
      reservedOutputs.insert(QFileInfo(destination).absoluteFilePath());
      job.state = State::Running;
      job.output.clear();
      job.error.clear();
      job.dimensions = {};
      job.percent = -1;
      job.elapsed = -1;
      runningRows.insert(row);
      QElapsedTimer clock;
      clock.start();
      updateCard(job);
      const auto heldPluginLock = batchLock.get();
      worker->start([input = job.input, destination, pluginRoot, unsupported, size, heldPluginLock, result]
                    (const Canceled &stop, const Progress &progress) {
        const auto source = Replay::open(input, stop);
        if (source->kind != Kind::Guacamole || source->fragment) throw Error{unsupported};
        result->info = *source;
        result->duration = source->duration();
        result->dimensions = outputSize(source->largestScreen, size);
        Replay::transcode(source, destination, pluginRoot, stop, progress, size, heldPluginLock);
      }, [this, row, result, clock](const QString &error, bool stopped) {
        if (row < 0 || row >= jobs.size()) return;
        auto &finished = jobs[row];
        runningRows.remove(row);
        finished.state = stopped ? State::Canceled : error.isEmpty() ? State::Succeeded : State::Failed;
        finished.error = error;
        if (!result->info.label.isEmpty()) finished.info = result->info;
        finished.duration = result->duration;
        if (finished.state == State::Succeeded) {
          finished.output = result->path;
          finished.dimensions = result->dimensions;
          finished.elapsed = clock.elapsed();
        }
        reservedOutputs.remove(QFileInfo(result->path).absoluteFilePath());
        updateCard(finished);
        scheduleJobs();
        finishBatchIfIdle();
        refresh();
      }, [this, row](int percent) {
        if (row < 0 || row >= jobs.size() || jobs.at(row).state != State::Running) return;
        jobs[row].percent = percent;
        updateCard(jobs[row]);
        refreshCounts();
      });
    }
    refresh();
  }

  void finishBatchIfIdle() {
    if (!batchActive || !runningRows.isEmpty() || inspection.busy() || pendingPickers > 0) return;
    if (nextQueuedJob() >= 0) {
      if (!stopping) scheduleJobs();
      if (!runningRows.isEmpty() || inspection.busy()) return;
      if (nextQueuedJob() >= 0) return;
    }
    batchActive = false;
    stopping = false;
    reservedOutputs.clear();
    releasePluginLock();
    refresh();
  }

  QList<QPair<QString, QString>> detailFields(const Job &job) const {
    auto fields = metadataFields(job.info);
    if (job.info.partCount) fields.append({text("分段数", "Segments"), QString::number(job.info.partCount)});
    const qint64 length = job.duration >= 0 ? job.duration : job.info.metadata.durationMs;
    if (length >= 0) fields.append({text("时长", "Duration"), timeLabel(length)});
    if (job.dimensions.isValid()) fields.append({text("输出画面", "Output dimensions"),
        QStringLiteral("%1 × %2").arg(job.dimensions.width()).arg(job.dimensions.height())});
    return fields;
  }

  void updateCard(Job &job) {
    auto &c = job.card;
    const bool complete = job.state == State::Succeeded;
    const bool running = job.state == State::Running;
    const bool valid = job.state != State::Reading && job.inspectionError.isEmpty();
    c.title->setText(job.info.label);
    c.title->setToolTip(detailsTooltip({{text("文件", "File"), job.info.label}}));
    QStringList summary;
    const auto names = metadataSummary(job.info.metadata);
    if (!names.isEmpty()) summary << names;
    if (valid) summary << kindLabel(job.info.kind);
    if (!job.info.metadata.protocol.isEmpty()) summary << job.info.metadata.protocol.toUpper();
    if (job.info.metadata.started.isValid()) summary << localTime(job.info.metadata.started);
    const qint64 length = job.duration >= 0 ? job.duration : job.info.metadata.durationMs;
    if (length >= 0) summary << timeLabel(length);
    if (job.info.partCount > 1) summary << text("%1 段", "%1 segments").arg(job.info.partCount);
    if (job.dimensions.isValid()) summary << QStringLiteral("%1 × %2").arg(job.dimensions.width()).arg(job.dimensions.height());
    c.summary->setText(summary.join(QStringLiteral(" · ")));
    c.summary->setVisible(!summary.isEmpty());
    c.summary->setToolTip(detailsTooltip(detailFields(job)));
    c.details->setText({});
    c.details->setToolTip(text("查看录像详情", "View recording details"));
    c.details->setAccessibleName(c.details->toolTip());
    c.details->setVisible(valid || !names.isEmpty());
    c.rename->setText({});
    c.rename->setToolTip(text("重命名输出文件", "Rename output file"));
    c.rename->setAccessibleName(c.rename->toolTip());
    c.rename->setVisible(valid && !complete);
    c.rename->setEnabled(!batchActive);
    c.remove->setToolTip(text("移除", "Remove"));
    c.remove->setAccessibleName(c.remove->toolTip());
    c.remove->setEnabled(!batchActive);
    syncPlayButton(c.play);
    c.play->setVisible(complete);
    c.elapsed->setVisible(complete);
    if (complete) c.elapsed->setText(text("用时 %1", "Elapsed %1").arg(timeLabel(qMax<qint64>(1000, job.elapsed))));
    c.progress->setVisible(running);
    c.percent->setVisible(running);
    if (running) {
      c.progress->setRange(0, job.percent < 0 ? 0 : 100);
      c.progress->setValue(job.percent);
      c.percent->setText(job.percent < 0 ? QStringLiteral("…") : QString::number(job.percent) + '%');
    }
    QString label;
    const char *tone = "neutral";
    switch (job.state) {
    case State::Reading: label = text("读取元数据", "Reading details"); break;
    case State::Queued: label = text("等待", "Queued"); tone = "warning"; break;
    case State::Running:
      label = job.percent < 0 ? text("读取录像", "Reading recording") : text("转码中", "Transcoding"); tone = "accent"; break;
    case State::Succeeded: label = text("已完成", "Completed"); tone = "success"; break;
    case State::Failed: label = text("失败", "Failed"); tone = "error"; break;
    case State::Canceled: label = text("已取消", "Canceled"); break;
    }
    c.state->setText(label);
    c.state->setObjectName("replayQueueState");
    if (c.state->property("tone").toString() != QLatin1String(tone)) {
      c.state->setProperty("tone", tone);
      c.state->style()->unpolish(c.state);
      c.state->style()->polish(c.state);
    }
    c.output->setVisible(valid || !job.error.isEmpty());
    const auto line = !job.error.isEmpty() ? job.error : complete ? QDir::toNativeSeparators(job.output) : predictedName(job);
    c.output->setText((job.error.isEmpty() ? text("输出：", "Output: ") : text("错误：", "Error: ")) + line);
    c.output->setToolTip(QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped()));
    c.output->setProperty("outputPath", job.output);
    c.frame->layout()->activate();
    c.item->setSizeHint(c.frame->sizeHint());
  }

  void syncPlayButton(QPushButton *button) {
    const bool showLabel = w->width() >= widePlayLabelMinimum;
    const bool iconOnly = !showLabel;
    button->setText(showLabel ? text("播放", "Play") : QString());
    button->setToolTip(text("播放转码结果", "Play transcoded output"));
    button->setAccessibleName(button->toolTip());
    if (button->property("iconOnly").toBool() != iconOnly) {
      button->setProperty("iconOnly", iconOnly);
      button->style()->unpolish(button);
      button->style()->polish(button);
    }
    button->setFixedSize(showLabel ? ClientTheme::buttonSize(button, true).width() : 28, 28);
  }

  void syncPlayButtons() {
    for (auto &job : jobs) syncPlayButton(job.card.play);
  }

  void refreshPlugin() {
    const bool supported = FfmpegPlugin::supported();
    pluginReady = supported && bool(FfmpegPlugin::installed());
  }

  void updateEmptyState() {
    const bool supported = FfmpegPlugin::supported();
    if (!pluginReady) {
      dropIcon->setPixmap(ClientTheme::icon("plugin").pixmap(32));
      dropTitle->setText(!supported ? text("当前平台不支持转码", "Transcoding is unavailable on this platform") :
          text("未安装转码插件", "Transcoding plugin not installed"));
      dropDescription->setText(!supported ?
          text("当前系统没有可用的 FFmpeg 转码插件。", "No FFmpeg transcoding plugin is available for this system.") :
          text("安装 FFmpeg 转码插件后即可添加并转换 RDP/VNC 协议录像。",
               "Install the FFmpeg transcoding plugin to add and convert RDP/VNC protocol recordings."));
      dropAdd->hide();
      installPlugin->setVisible(supported);
    } else {
      dropIcon->setPixmap(ClientTheme::icon("folder").pixmap(32));
      dropTitle->setText(text("将录像拖到这里", "Drop recordings here"));
      dropDescription->setText(text("支持完整的 RDP/VNC 协议录像 TAR 和 .replay.gz，可一次添加多个文件。",
          "Add one or more complete RDP/VNC protocol recording TAR or .replay.gz files."));
      dropAdd->show();
      installPlugin->hide();
    }
  }

  void refreshCounts() {
    int done = 0, errors = 0, stopped = 0, pending = 0;
    qint64 progressSum = 0;
    for (const auto &job : jobs) {
      done += job.state == State::Succeeded;
      errors += job.state == State::Failed;
      stopped += job.state == State::Canceled;
      pending += job.state == State::Queued;
      if (job.state == State::Succeeded || job.state == State::Failed || job.state == State::Canceled)
        progressSum += 100;
      else if (job.state == State::Running)
        progressSum += qMax(0, job.percent);
    }
    if (batchActive && !jobs.isEmpty()) {
      const int progress = progressSum / jobs.size();
      total->setText(text("总进度 %1%", "Total progress %1%").arg(progress));
    } else {
      total->setText(text("共 %1 项", "%1 items").arg(jobs.size()));
    }
    total->setVisible(!jobs.isEmpty());
    queued->setVisible(pending > 0);
    queued->setText(text("等待 %1", "%1 queued").arg(pending));
    succeeded->setVisible(done > 0);
    succeeded->setText(text("成功 %1", "%1 completed").arg(done));
    failed->setVisible(errors > 0);
    failed->setText(text("失败 %1", "%1 failed").arg(errors));
    canceled->setVisible(stopped > 0);
    canceled->setText(text("已取消 %1", "%1 canceled").arg(stopped));
  }

  void refresh() {
    const bool running = batchActive;
    const QFileInfo folder(directory->text());
    const bool writable = folder.isDir() && folder.isWritable();
    const bool eligible = std::any_of(jobs.cbegin(), jobs.cend(), [](const Job &job) {
      return job.state != State::Succeeded && job.state != State::Reading && job.inspectionError.isEmpty();
    });
    start->setVisible(!running);
    start->setEnabled(!running && !inspection.busy() && pluginReady && writable && eligible);
    cancel->setVisible(running);
    cancel->setEnabled(running && !stopping);
    outputOptions->setEnabled(!running);
    add->setEnabled(pluginReady && (!stopping || !running));
    dropAdd->setEnabled(pluginReady);
    installPlugin->setEnabled(FfmpegPlugin::supported() && !running);
    choose->setEnabled(!running);
    filename->setEnabled(!running);
    resolution->setEnabled(!running);
    concurrency->setEnabled(!running);
    clear->setEnabled(!running && !jobs.isEmpty());
    updateEmptyState();
    pages->setCurrentIndex(jobs.isEmpty() ? 0 : 1);
    refreshCounts();
    for (auto &job : jobs) {
      job.card.remove->setEnabled(!running);
      job.card.rename->setEnabled(!running);
    }
    const auto warning = !writable ? text("请选择有写入权限的输出目录。", "Choose a writable output folder.") :
        !batchError.isEmpty() ? batchError :
        stopping && running ? text("正在取消…", "Canceling…") : QString();
    status->setText(warning);
    status->setVisible(!warning.isEmpty());
  }

  void retranslate() {
    start->setIcon(primaryPlayIcon(w));
    headerIcon->setPixmap(ClientTheme::icon("replay-transcode", ClientTheme::colors().accent).pixmap(16));
    dropIcon->setPixmap(ClientTheme::icon("folder").pixmap(32));
    w->setWindowTitle(text("录像转码", "Recording transcoding"));
    title->setText(w->windowTitle());
    description->setText(text("将 RDP/VNC 协议录像批量转换为 MP4，默认同时处理 3 个录像",
                              "Convert RDP/VNC protocol recordings to MP4; three recordings run at once by default"));
    outputOptions->setText(text("转码设置", "Transcode settings"));
    options->setWindowTitle(outputOptions->text());
    optionHeading->setText(text("输出设置", "Output settings"));
    optionDescription->setText(text("设置转码文件的保存位置、输出格式与并发数量",
                                    "Choose where to save transcoded recordings, their output format, and concurrency."));
    directoryHint->setText(text("转码完成的文件将保存在此目录", "Transcoded files will be saved here."));
    optionCancel->setText(text("取消", "Cancel"));
    optionSave->setText(text("保存", "Save"));
    start->setText(text("开始转码", "Start transcoding"));
    cancel->setText(text("取消转码", "Cancel transcoding"));
    directoryLabel->setText(text("输出目录", "Output folder"));
    choose->setText(text("选择…", "Choose…"));
    filenameLabel->setText(text("文件命名", "Filename style"));
    resolutionLabel->setText(text("输出分辨率", "Output resolution"));
    concurrencyLabel->setText(text("并发数量", "Concurrency"));
    concurrencyHint->setText(text("最多同时处理 %1 个录像，默认 %2 个。",
                                  "Process up to %1 recordings at once; the default is %2.")
                                .arg(maxConcurrency)
                                .arg(defaultConcurrency));
    const int nameIndex = filename->currentIndex() < 0 ? QSettings().value("replay/filenameStyle", 0).toInt() : filename->currentIndex();
    const int sizeIndex = resolution->currentIndex() < 0 ? QSettings().value("replay/outputResolution", 1).toInt() : resolution->currentIndex();
    filename->clear();
    filename->addItem(text("原始名称 / 会话 ID", "Original name / session ID"), int(FilenameStyle::Original));
    filename->addItem(text("用户-资产-账号", "User-asset-account"), int(FilenameStyle::Session));
    filename->addItem(text("用户-资产-账号（会话 ID）", "User-asset-account (session ID)"), int(FilenameStyle::SessionWithId));
    filename->setCurrentIndex(qBound(0, nameIndex, filename->count() - 1));
    resolution->clear();
    resolution->addItem(text("原始分辨率", "Original resolution"), int(Resolution::Original));
    resolution->addItem(QStringLiteral("1080p"), int(Resolution::P1080));
    resolution->addItem(QStringLiteral("720p"), int(Resolution::P720));
    resolution->addItem(QStringLiteral("360p"), int(Resolution::P360));
    resolution->setCurrentIndex(qBound(0, sizeIndex, resolution->count() - 1));
    queueTitle->setText(text("转码队列", "Transcode queue"));
    add->setText(text("添加录像…", "Add recordings…"));
    dropAdd->setText(add->text());
    clear->setText(text("清空", "Clear"));
    installPlugin->setText(text("前往安装", "Install plugin"));
    for (auto &job : jobs) {
      job.card.icon->setPixmap(ClientTheme::icon("video").pixmap(16));
      job.card.play->setIcon(ClientTheme::icon("play"));
      updateCard(job);
    }
    refreshPlugin();
    refresh();
  }

  TranscodeWindow *w;
  std::function<void(const QString &)> playOutput;
  std::unique_ptr<QLockFile> batchLock;
  std::vector<std::unique_ptr<Task>> workers;
  Task inspection;
  QVector<Job> jobs;
  QSet<int> runningRows;
  QSet<QString> reservedOutputs;
  int pendingPickers = 0;
  bool stopping = false, pluginReady = false, batchActive = false;
  QString batchError;
  QLabel *title, *description, *directoryLabel, *filenameLabel, *resolutionLabel, *concurrencyLabel,
      *concurrencyHint, *queueTitle, *optionError, *optionHeading, *optionDescription, *directoryHint,
      *headerIcon, *dropIcon,
      *dropTitle, *dropDescription, *status, *total, *queued, *succeeded, *failed, *canceled;
  QPushButton *start, *cancel, *outputOptions, *choose, *installPlugin, *add, *dropAdd, *clear,
      *optionSave, *optionCancel;
  QLineEdit *directory;
  QComboBox *filename, *resolution;
  QSpinBox *concurrency;
  QDialog *options;
  QListWidget *list;
  QStackedWidget *pages;
  QWidget *drop;
};

TranscodeWindow::TranscodeWindow(QWidget *parent, std::function<void()> openPluginSettings,
                                 std::function<void(const QString &)> openOutput)
    : QDialog(parent), d(std::make_unique<Private>(this, std::move(openPluginSettings), std::move(openOutput))) {
  retranslate();
}

TranscodeWindow::~TranscodeWindow() = default;

void TranscodeWindow::retranslate() { d->retranslate(); }

void TranscodeWindow::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  d->syncPlayButtons();
}

void TranscodeWindow::dragEnterEvent(QDragEnterEvent *event) {
  if (d->pluginReady && (!d->batchActive || !d->stopping) && !recordings(event->mimeData()).isEmpty()) {
    setDropHighlight(d->drop, true);
    event->acceptProposedAction();
  }
}

void TranscodeWindow::dragLeaveEvent(QDragLeaveEvent *event) {
  setDropHighlight(d->drop, false);
  event->accept();
}

void TranscodeWindow::dropEvent(QDropEvent *event) {
  setDropHighlight(d->drop, false);
  if (!d->pluginReady || (d->batchActive && d->stopping)) return;
  const auto paths = recordings(event->mimeData());
  if (paths.isEmpty()) return;
  d->addPaths(paths);
  event->acceptProposedAction();
}

} // namespace Replay
