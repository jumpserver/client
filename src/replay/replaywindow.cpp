#include "replaywindow.h"
#include "castplayer.h"
#include "guacamole.h"
#include "presentation.h"
#include "task.h"
#include "theme.h"

#include <QComboBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QMediaPlayer>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVideoWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif

// Qt 5 on Windows: QVideoWidget renders through the WMF EVR video window, a
// native child HWND whose display rect is only refreshed on resize. Inside the
// replay stage it stays covered by the Qt-painted background (white in light
// theme, dark in dark theme) until a layout change, such as hiding the
// playlist, resizes it. Receive frames through a video surface and paint them
// with the same Canvas used for Guacamole playback, so no native child window
// is involved. Define CLIENT_REPLAY_SURFACE_VIDEO to exercise this path on
// other Qt 5 platforms; set OSM_REPLAY_NATIVE_VIDEO=1 to fall back at runtime.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && QT_VERSION >= QT_VERSION_CHECK(5, 15, 0) && (defined(Q_OS_WIN) || defined(CLIENT_REPLAY_SURFACE_VIDEO))
#define CLIENT_REPLAY_SURFACE_VIDEO_ENABLED 1
#include <QAbstractVideoSurface>
#include <QPointer>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#endif

namespace Replay {
namespace {
using ClientTheme::text;

class Canvas : public QWidget {
public:
  using QWidget::QWidget;
  void setFrame(QImage image) { m_image = std::move(image); update(); }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 20, 24));
    if (!m_image.isNull()) {
      const QSize size = m_image.size().scaled(QWidget::size(), Qt::KeepAspectRatio);
      p.setRenderHint(QPainter::SmoothPixmapTransform);
      p.drawImage(QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size), m_image);
    }
  }
private:
  QImage m_image;
};

#ifdef CLIENT_REPLAY_SURFACE_VIDEO_ENABLED
class FrameSurface : public QAbstractVideoSurface {
public:
  FrameSurface(Canvas *target, QObject *parent) : QAbstractVideoSurface(parent), m_target(target) {}
  QList<QVideoFrame::PixelFormat> supportedPixelFormats(
      QAbstractVideoBuffer::HandleType type) const override {
    if (type != QAbstractVideoBuffer::NoHandle) return {};
    return {QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32,
            QVideoFrame::Format_ARGB32_Premultiplied, QVideoFrame::Format_RGB24,
            QVideoFrame::Format_RGB565, QVideoFrame::Format_RGB555,
            QVideoFrame::Format_NV12, QVideoFrame::Format_YUV420P,
            QVideoFrame::Format_YV12, QVideoFrame::Format_YUYV, QVideoFrame::Format_UYVY};
  }
  bool start(const QVideoSurfaceFormat &format) override {
    m_bottomUp = format.scanLineDirection() == QVideoSurfaceFormat::BottomToTop;
    return QAbstractVideoSurface::start(format);
  }
  bool present(const QVideoFrame &frame) override {
    // QVideoFrame::image() maps the frame and returns a deep copy, so the
    // image stays valid on the GUI thread after the backend recycles the frame.
    QImage image = frame.image();
    if (image.isNull()) return true;
    if (m_bottomUp) image = image.mirrored();
    QPointer<Canvas> target = m_target;
    if (!target) return false;
    QMetaObject::invokeMethod(target, [target, image] {
      if (target) target->setFrame(image);
    }, Qt::QueuedConnection);
    return true;
  }
private:
  QPointer<Canvas> m_target;
  bool m_bottomUp = false;
};
#endif

class PlayOverlay : public QWidget {
public:
  explicit PlayOverlay(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setObjectName("replayPlayOverlay");
    hide();
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center = rect().center();
    painter.setPen(QPen(QColor(255, 255, 255, 110), 1));
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawEllipse(center, 32, 32);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    QPolygonF triangle;
    triangle << center + QPointF(-8, -13) << center + QPointF(14, 0) << center + QPointF(-8, 13);
    painter.drawPolygon(triangle);
  }
};

class LoadingOverlay : public QWidget {
public:
  explicit LoadingOverlay(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setObjectName("replayLoadingOverlay");
    m_timer.setInterval(40);
    connect(&m_timer, &QTimer::timeout, this, [this] {
      m_angle = (m_angle + 12) % 360;
      update();
    });
    hide();
  }

protected:
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    m_angle = 0;
    m_timer.start();
  }
  void hideEvent(QHideEvent *event) override {
    m_timer.stop();
    QWidget::hideEvent(event);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 15, 20, 145));
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF center(width() / 2.0, height() / 2.0 - 10);
    const QRectF ring(center.x() - 18, center.y() - 18, 36, 36);
    painter.setPen(QPen(QColor(255, 255, 255, 42), 3));
    painter.drawEllipse(ring);
    painter.setPen(QPen(QColor("#52b7f3"), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, -m_angle * 16, 105 * 16);
    QFont labelFont = font();
    if (labelFont.pointSizeF() <= 0) labelFont.setPixelSize(13);
    painter.setFont(labelFont);
    painter.setPen(QColor("#f4f7fb"));
    painter.drawText(QRectF(0, center.y() + 29, width(), 28),
                     Qt::AlignHCenter | Qt::AlignTop,
                     text("正在加载…", "Loading…"));
  }

private:
  QTimer m_timer;
  int m_angle = 0;
};

void setLocalMedia(QMediaPlayer &player, const QUrl &url) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  player.setMedia(url);
#else
  player.setSource(url);
#endif
}

QIcon controlIcon(const QIcon &source) {
  auto pixmap = source.pixmap(20, 20);
  QPainter painter(&pixmap);
  painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
  painter.fillRect(pixmap.rect(), QColor("#f2f2f2"));
  QIcon icon(pixmap);
  painter.end();
  QPixmap disabled = pixmap;
  QPainter muted(&disabled);
  muted.setCompositionMode(QPainter::CompositionMode_SourceIn);
  muted.fillRect(disabled.rect(), QColor("#777b83"));
  muted.end();
  icon.addPixmap(disabled, QIcon::Disabled);
  return icon;
}

constexpr int kSeekLoadingMinimumMs = 260;
} // namespace

struct Window::Private {
  explicit Private(Window *window) : w(window), importer(w), renderer(w) {
    w->setObjectName("offlineReplayWindow");
    w->setProperty("replayPage", true);
    w->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    w->setAcceptDrops(true);
    w->resize(1250, 740);
    w->setMinimumSize(760, 520);
    auto *layout = new QVBoxLayout(w);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    auto *stage = new QFrame(w);
    stage->setObjectName("replayStage");
    auto *stageLayout = new QVBoxLayout(stage);
    stageLayout->setContentsMargins(7, 7, 7, 7);
    screens = new QStackedWidget(stage);
    auto *drop = new QWidget(screens);
    drop->setObjectName("replayDropZone");
    drop->setAttribute(Qt::WA_StyledBackground);
    auto *dropLayout = new QVBoxLayout(drop);
    dropLayout->setContentsMargins(20, 20, 20, 20);
    auto *dropIcon = new QLabel(drop);
    dropIcon->setObjectName("replayDropIcon");
    dropIcon->setText(QStringLiteral("▶"));
    dropIcon->setAlignment(Qt::AlignCenter);
    dropIcon->setFixedSize(48, 48);
    dropTitle = new QLabel(drop);
    dropTitle->setObjectName("replayDropTitle");
    dropTitle->setAlignment(Qt::AlignCenter);
    dropTitle->setWordWrap(true);
    dropDescription = new QLabel(drop);
    dropDescription->setObjectName("replayDropDescription");
    dropDescription->setAlignment(Qt::AlignCenter);
    dropDescription->setWordWrap(true);
    dropOpen = new QPushButton(drop);
    dropOpen->setProperty("primary", true);
    dropLayout->setSpacing(10);
    dropLayout->addStretch();
    dropLayout->addWidget(dropIcon, 0, Qt::AlignHCenter);
    dropLayout->addWidget(dropTitle);
    dropLayout->addWidget(dropDescription);
    dropLayout->addWidget(dropOpen, 0, Qt::AlignHCenter);
    dropLayout->addStretch();
    canvas = new Canvas(screens);
    canvas->setObjectName("replayCanvas");
    loadingOverlay = new LoadingOverlay(canvas);
    canvasOverlay = new PlayOverlay(canvas);
    dropZone = drop;
    screens->addWidget(dropZone);
    screens->addWidget(canvas);
    stageLayout->addWidget(screens, 1);
    body->addWidget(stage, 3);
    auto *controls = new QHBoxLayout;
    play = new QPushButton(w);
    play->setObjectName("toggleReplayPlayback");
    play->setProperty("replayControl", true);
    play->setFixedSize(36, 36);
    play->setShortcut(Qt::Key_Space);
    seek = new QSlider(Qt::Horizontal, w);
    seek->setObjectName("replayPosition");
    time = new QLabel(w);
    time->setObjectName("replayTime");
    speed = new QComboBox(w);
    speed->setObjectName("replaySpeed");
    auto *speedOptions = new QListView(speed);
    speedOptions->setMinimumWidth(90);
    speedOptions->setTextElideMode(Qt::ElideNone);
    speed->setView(speedOptions);
    speed->setFixedHeight(30);
    int widestSpeedText = 0;
    for (double rate : {0.5, 1.0, 1.5, 2.0, 2.5, 3.0}) {
      const QString option = QString::number(rate) + "×";
      speed->addItem(option, rate);
      widestSpeedText = qMax(widestSpeedText, speed->fontMetrics().horizontalAdvance(option));
    }
    speed->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    speed->setFixedWidth(qMax(66, widestSpeedText + 34));
    speed->setCurrentIndex(1);
    controls->addWidget(play);
    controls->addWidget(seek, 1);
    controls->addWidget(time);
    controls->addWidget(speed);
    playlistToggle = new QPushButton(w);
    playlistToggle->setObjectName("toggleReplayPlaylist");
    playlistToggle->setFixedWidth(18);
    playlistToggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    playlistToggle->setCursor(Qt::PointingHandCursor);
    playlistToggle->setIconSize(QSize(14, 14));
    stageLayout->addLayout(controls);
    body->addWidget(playlistToggle);

    auto *playlist = new QFrame(w);
    playlist->setObjectName("replayPlaylistPanel");
    playlist->setProperty("surface", true);
    playlist->setMinimumWidth(270);
    playlist->setMaximumWidth(420);
    playlist->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *playlistLayout = new QVBoxLayout(playlist);
    playlistLayout->setContentsMargins(12, 12, 12, 12);
    auto *playlistToolbar = new QHBoxLayout;
    playlistTitle = new QLabel(playlist);
    playlistTitle->setProperty("role", "sectionTitle");
    count = new QLabel(playlist);
    count->setProperty("role", "replayBadge");
    count->setProperty("tone", "accent");
    add = new QPushButton(playlist);
    add->setObjectName("openReplayFiles");
    add->setShortcut(QKeySequence::Open);
    add->setProperty("quiet", true);
    playlistToolbar->addWidget(playlistTitle, 1);
    playlistToolbar->addWidget(count);
    playlistToolbar->addWidget(add);
    playlistLayout->addLayout(playlistToolbar);
    list = new QListWidget(playlist);
    list->setObjectName("replayFiles");
    list->setUniformItemSizes(true);
    list->setTextElideMode(Qt::ElideMiddle);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    playlistPages = new QStackedWidget(playlist);
    emptyPlaylist = new QLabel(playlistPages);
    emptyPlaylist->setObjectName("replayEmptyPlaylist");
    emptyPlaylist->setAlignment(Qt::AlignCenter);
    emptyPlaylist->setWordWrap(true);
    emptyPlaylist->setAttribute(Qt::WA_StyledBackground);
    emptyPlaylist->setProperty("muted", true);
    playlistPages->addWidget(emptyPlaylist);
    playlistPages->addWidget(list);
    playlistLayout->addWidget(playlistPages, 1);

    recordDetails = new QFrame(playlist);
    recordDetails->setProperty("replayCard", true);
    auto *detailsLayout = new QVBoxLayout(recordDetails);
    detailsLayout->setContentsMargins(11, 8, 11, 8);
    assetLabel = new ClientTheme::ElidedLabel({}, recordDetails);
    assetLabel->setObjectName("replayMetadataTitle");
    assetLabel->setProperty("role", "sectionTitle");
    assetLabel->setTextFormat(Qt::PlainText);
    detailsLayout->addWidget(assetLabel);
    recordSummary = new ClientTheme::ElidedLabel({}, recordDetails);
    recordSummary->setObjectName("replayMetadataSummary");
    recordSummary->setProperty("role", "caption");
    recordSummary->setTextFormat(Qt::PlainText);
    detailsLayout->addWidget(recordSummary);
    recordTime = new ClientTheme::ElidedLabel({}, recordDetails);
    recordTime->setProperty("role", "caption");
    recordTime->setTextFormat(Qt::PlainText);
    detailsLayout->addWidget(recordTime);
    info = new QPushButton(recordDetails);
    info->setObjectName("replayMetadataButton");
    info->setProperty("soft", true);
    detailsLayout->addWidget(info, 0, Qt::AlignLeft);
    segments = new ClientTheme::ComboBox(recordDetails);
    segments->setObjectName("replaySegments");
    segments->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    detailsLayout->addWidget(segments);
    segmentTime = new ClientTheme::ElidedLabel({}, recordDetails);
    segmentTime->setObjectName("replaySegmentTime");
    segmentTime->setProperty("role", "caption");
    segmentTime->setTextFormat(Qt::PlainText);
    detailsLayout->addWidget(segmentTime);
    playlistLayout->addWidget(recordDetails);
    body->addWidget(playlist, 1);
    QObject::connect(playlistToggle, &QPushButton::clicked, w, [this, playlist] {
      playlist->setVisible(playlist->isHidden());
      refreshControls();
    });
    layout->addLayout(body, 1);

    auto *statusRow = new QHBoxLayout;
    status = new QLabel(w);
    status->setObjectName("replayStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    external = new QPushButton(w);
    external->hide();
    statusRow->addWidget(status, 1);
    statusRow->addWidget(external);
    layout->addLayout(statusRow);

    timer = new QTimer(w);
    timer->setInterval(35);
    QObject::connect(timer, &QTimer::timeout, w, [this] {
      if (!playing || renderer.busy()) return;
      const qint64 next = qMin<qint64>(duration, origin + qint64(clock.elapsed() * rate()));
      requestFrame(int(next));
    });
    const auto pickFiles = [this] {
      const auto paths = QFileDialog::getOpenFileNames(w, text("打开离线录像", "Open offline recordings"),
          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
          text("离线录像 (*.tar *.replay.gz *.part.gz *.mp4 *.cast *.cast.gz)",
               "Offline recordings (*.tar *.replay.gz *.part.gz *.mp4 *.cast *.cast.gz)"));
      addPaths(paths);
    };
    QObject::connect(add, &QPushButton::clicked, w, pickFiles);
    QObject::connect(dropOpen, &QPushButton::clicked, w, pickFiles);
    QObject::connect(list, &QWidget::customContextMenuRequested, w, [this](const QPoint &point) {
      auto *item = list->itemAt(point);
      if (!item) return;
      QMenu menu(list);
      menu.setObjectName("replayPlaylistContextMenu");
      auto *action = menu.addAction(text("移除", "Remove"));
      action->setObjectName("removeReplayContextAction");
      if (menu.exec(list->viewport()->mapToGlobal(point)) == action && list->row(item) >= 0) {
        delete list->takeItem(list->row(item));
        refreshControls();
      }
    });
    QObject::connect(list, &QListWidget::currentItemChanged, w, [this](QListWidgetItem *item) {
      load(item ? item->data(Qt::UserRole).toString() : QString());
    });
    QObject::connect(segments, QOverload<int>::of(&QComboBox::activated), w, [this](int row) {
      if (!source || row < 0 || row >= source->segments.size()) return;
      resumeAfterSeek = playing || resumeAfterSeek;
      seekTo(source->segments.at(row).start);
    });
    QObject::connect(info, &QPushButton::clicked, w, [this] {
      if (!source) return;
      auto fields = metadataFields(*source);
      const qint64 length = duration > 0 ? duration : source->metadata.durationMs;
      if (length >= 0) fields.append({text("时长", "Duration"), timeLabel(length)});
      if (!source->largestScreen.isEmpty()) fields.append({text("画面", "Screen"),
          QStringLiteral("%1 × %2").arg(source->largestScreen.width()).arg(source->largestScreen.height())});
      showDetails(info, fields);
    });
    QObject::connect(play, &QPushButton::clicked, w, [this] { toggle(); });
    QObject::connect(seek, &QSlider::sliderPressed, w, [this] {
      resumeAfterDrag = source && !isMp4() && (playing || resumeAfterSeek);
      resumeAfterSeek = false;
      if (!isMp4()) pause();
    });
    QObject::connect(seek, &QSlider::valueChanged, w, [this](int ms) {
      if (!updating && !seek->isSliderDown()) seekTo(ms);
    });
    QObject::connect(seek, &QSlider::sliderReleased, w, [this] {
      resumeAfterSeek = resumeAfterDrag && source && !isMp4();
      resumeAfterDrag = false;
      seekTo(seek->value());
    });
    QObject::connect(speed, QOverload<int>::of(&QComboBox::currentIndexChanged), w, [this] {
      if (isMp4()) { ensureMedia(); player->setPlaybackRate(rate()); }
      else if (playing) { origin = position; clock.restart(); }
    });
    QObject::connect(external, &QPushButton::clicked, w, [this] {
      if (isMp4()) QDesktopServices::openUrl(QUrl::fromLocalFile(source->path));
    });
  }

  void retranslate() {
    w->setWindowTitle(text("离线播放", "Offline playback"));
    playlistTitle->setText(text("播放列表", "Playlist"));
    add->setText(text("添加…", "Add…"));
    dropTitle->setText(text("将录像拖到这里", "Drop recordings here"));
    dropDescription->setText(text("支持 RDP/VNC 协议录像、MP4 和终端录像",
                                  "RDP/VNC protocol recordings, MP4 and terminal recordings"));
    dropOpen->setText(text("打开录像…", "Open recordings…"));
    emptyPlaylist->setText(text("播放列表为空\n添加本地录像后可在这里切换。", "The playlist is empty.\nAdd recordings to switch between them here."));
    info->setText(text("录像详情", "Recording details"));
    segments->setAccessibleName(text("选择录像分段", "Choose recording segment"));
    updateDetails();
    external->setText(text("用系统播放器打开", "Open in system player"));
    seek->setAccessibleName(text("播放进度", "Playback position"));
    speed->setAccessibleName(text("播放速度", "Playback speed"));
    refreshControls();
    setPosition(position);
  }

  void addPaths(const QStringList &paths) {
    QListWidgetItem *first = nullptr;
    for (const QString &path : paths) {
      const QFileInfo info(path);
      if (!info.isFile()) continue;
      const auto absolute = info.absoluteFilePath();
      QListWidgetItem *item = nullptr;
      for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == absolute) { item = list->item(i); break; }
      if (!item) {
        if (list->count() >= 100) break;
        item = new QListWidgetItem(info.fileName(), list);
        item->setData(Qt::UserRole, absolute);
        item->setToolTip(info.fileName().toHtmlEscaped());
        item->setSizeHint(QSize(0, 36));
      }
      if (!first) first = item;
    }
    if (first) list->setCurrentItem(first);
    refreshControls();
  }

  double rate() const { return speed->currentData().toDouble(); }
  bool isMp4() const { return source && source->kind == Kind::Mp4; }
  void pause() { playing = false; timer->stop(); refreshControls(); }

  void ensureMedia() {
    if (player) return;
#ifdef CLIENT_REPLAY_SURFACE_VIDEO_ENABLED
    const bool nativeVideo = qEnvironmentVariableIntValue("OSM_REPLAY_NATIVE_VIDEO") == 1;
    Canvas *videoCanvas = nativeVideo ? nullptr : new Canvas(screens);
    if (videoCanvas) {
      videoCanvas->setObjectName("replayVideoCanvas");
      video = videoCanvas;
    } else {
      video = new QVideoWidget(screens);
    }
#else
    video = new QVideoWidget(screens);
#endif
    videoOverlay = new PlayOverlay(video);
    video->installEventFilter(w);
    screens->addWidget(video);
    player = new QMediaPlayer(w);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    player->setAudioOutput(new QAudioOutput(player));
    QObject::connect(player, &QMediaPlayer::playbackStateChanged, w, [this](QMediaPlayer::PlaybackState state) {
      if (isMp4()) { playing = state == QMediaPlayer::PlayingState; refreshControls(); }
    });
    QObject::connect(player, &QMediaPlayer::errorOccurred, w, [this] { mediaError(); });
#else
    QObject::connect(player, &QMediaPlayer::stateChanged, w, [this](QMediaPlayer::State state) {
      if (isMp4()) { playing = state == QMediaPlayer::PlayingState; refreshControls(); }
    });
    QObject::connect(player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), w, [this] { mediaError(); });
#endif
#ifdef CLIENT_REPLAY_SURFACE_VIDEO_ENABLED
    if (videoCanvas) player->setVideoOutput(new FrameSurface(videoCanvas, player));
    else player->setVideoOutput(static_cast<QVideoWidget *>(video));
#else
    player->setVideoOutput(static_cast<QVideoWidget *>(video));
#endif
    QObject::connect(player, &QMediaPlayer::positionChanged, w, [this](qint64 ms) { if (isMp4()) setPosition(ms); });
    QObject::connect(player, &QMediaPlayer::durationChanged, w, [this](qint64 ms) { if (isMp4()) setDuration(ms); });
    QObject::connect(player, &QMediaPlayer::seekableChanged, w, [this] { refreshControls(); });
    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, w, [this](QMediaPlayer::MediaStatus s) {
      if (!isMp4()) return;
      if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia) refreshControls();
      else if (s == QMediaPlayer::InvalidMedia) mediaError();
    });
  }

  void updateDetails() {
    recordDetails->setVisible(bool(source));
    segments->clear();
    segments->hide();
    segmentTime->hide();
    if (!source) return;
    const auto &m = source->metadata;
    assetLabel->setText(m.asset.isEmpty() ? source->label : m.asset);
    QStringList summary{kindLabel(source->kind)};
    for (const auto &value : {m.user, m.account, m.protocol.toUpper()}) if (!value.isEmpty()) summary << value;
    recordSummary->setText(summary.join(QStringLiteral(" · ")));
    for (const auto &segment : source->segments) {
      segments->addItem(text("分段 %1 / %2 · %3", "Segment %1 / %2 · %3")
          .arg(segment.index + 1).arg(source->partCount).arg(timeLabel(segment.start)));
    }
    segments->setVisible(source->partCount > 1 && !source->segments.isEmpty());
    segmentTime->setVisible(!segments->isHidden());
    displayedSegment = -1;
    updateRecordTime();
    updateSegment();
  }

  void updateRecordTime() {
    if (!source) return;
    QStringList values;
    if (source->metadata.started.isValid()) values << localTime(source->metadata.started);
    const qint64 length = duration > 0 ? duration : source->metadata.durationMs;
    if (length >= 0) values << timeLabel(length);
    recordTime->setText(values.join(QStringLiteral(" · ")));
    recordTime->setVisible(!values.isEmpty());
  }

  void updateSegment() {
    if (!source || source->segments.isEmpty() || segments->isHidden()) return;
    const auto &parts = source->segments;
    auto next = std::upper_bound(parts.cbegin(), parts.cend(), position,
        [](int ms, const Segment &segment) { return ms < segment.start; });
    const int row = next == parts.cbegin() ? 0 : int(next - parts.cbegin()) - 1;
    segments->setCurrentIndex(row);
    if (displayedSegment == row) return;
    displayedSegment = row;
    const auto &part = parts.at(row);
    const auto started = part.started.isValid() ? part.started : source->metadata.started.isValid()
        ? source->metadata.started.addMSecs(part.start) : QDateTime();
    const auto range = timeLabel(part.start) + QStringLiteral(" – ") + timeLabel(part.end);
    segmentTime->setText(started.isValid() ? localTime(started) + QStringLiteral(" · ") + range : range);
  }

  void setPosition(qint64 ms) {
    position = int(qBound<qint64>(0, ms, duration));
    time->setText(timeLabel(position) + " / " + timeLabel(duration));
    updateSegment();
    if (!seek->isSliderDown()) {
      updating = true;
      seek->setValue(position);
      updating = false;
    }
  }
  void setDuration(qint64 ms) {
    duration = int(qBound<qint64>(0, ms, INT_MAX));
    updating = true;
    seek->setRange(0, duration);
    updating = false;
    setPosition(position);
    updateRecordTime();
    refreshControls();
  }
  void refreshControls() {
    const auto label = playing ? text("暂停", "Pause") : text("播放", "Play");
    play->setIcon(controlIcon(w->style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay)));
    play->setToolTip(label);
    play->setAccessibleName(label);
    const bool shown = !w->findChild<QFrame *>("replayPlaylistPanel")->isHidden();
    playlistToggle->setIcon(ClientTheme::icon(shown ? "chevron-right" : "chevron-left"));
    playlistToggle->setToolTip(shown ? text("收起播放列表", "Hide playlist") : text("展开播放列表", "Show playlist"));
    playlistToggle->setAccessibleName(playlistToggle->toolTip());
    const bool seeking = source && !isMp4() && pendingSeek >= 0;
    play->setEnabled(source && !playbackError && !seeking);
    const bool paused = source && !playbackError && !playing && !seeking;
    canvasOverlay->setVisible(paused && !isMp4());
    if (videoOverlay) videoOverlay->setVisible(paused && isMp4());
    loadingOverlay->setVisible(seeking);
    if (seeking) loadingOverlay->raise();
    if (paused) {
      canvasOverlay->raise();
      if (videoOverlay) videoOverlay->raise();
    }
    seek->setEnabled(source && duration > 0 && !playbackError &&
                     (!isMp4() || (player && player->isSeekable())));
    speed->setEnabled(source && !playbackError);
    count->setText(QString::number(list->count()));
    playlistPages->setCurrentWidget(list->count() ? static_cast<QWidget *>(list) : emptyPlaylist);
  }

  void seekTo(int ms) {
    const int target = qBound(0, ms, duration);
    // Keep the selected segment and timeline position responsive while a
    // multi-part recording rebuilds its display state in the background.
    setPosition(target);
    if (isMp4()) { ensureMedia(); player->setPosition(target); return; }
    if (source) {
      resumeAfterSeek = playing || resumeAfterSeek;
      pendingSeek = target;
      seekLoadingClock.restart();
      w->setProperty("replaySeekPending", true);
      pause();
      requestFrame(target, ++seekVersion);
    }
  }
  void toggle() {
    resumeAfterDrag = false;
    resumeAfterSeek = false;
    if (!source || playbackError) return;
    if (isMp4()) {
      ensureMedia();
      if (playing) player->pause();
      else { if (duration > 0 && position == duration) player->setPosition(0); player->play(); }
      return;
    }
    if (playing) { pause(); return; }
    origin = position >= duration ? 0 : position;
    playing = true;
    clock.restart();
    timer->start();
    requestFrame(origin);
    refreshControls();
  }

  void mediaError() {
    if (!isMp4()) return;
    playbackError = true;
    external->show();
    status->setText(text("当前系统无法内嵌播放这个 MP4。", "The system media backend cannot play this MP4."));
    refreshControls();
  }

  void load(const QString &path) {
    resumeAfterDrag = false;
    resumeAfterSeek = false;
    const auto oldSource = std::move(source);
    source.reset();
    // Release the media backend before dropping the source's extraction dir.
    if (player) {
      player->stop();
      setLocalMedia(*player, {});
    }
    guacamole.reset();
    cast.reset();
    requested = -1;
    pendingPath = path;
    pendingSeek = -1;
    w->setProperty("replaySeekPending", false);
    ++generation;
    importer.cancel();
    renderer.cancel();
    playbackError = false;
    pause();
    setDuration(0);
    updateDetails();
    screens->setCurrentWidget(dropZone);
    canvas->setFrame({});
    // Drop the previous MP4's last frame when the video is painted by Canvas.
    if (auto *videoCanvas = dynamic_cast<Canvas *>(video)) videoCanvas->setFrame({});
    external->hide();
    status->setText(path.isEmpty() ? QString() : text("正在读取录像…", "Reading recording…"));
    startImport();
  }

  struct Imported {
    SourcePtr source;
    std::shared_ptr<Guacamole> guac;
    std::shared_ptr<CastPlayer> cast;
    QImage preview;
  };
  void startImport() {
    if (importer.busy() || pendingPath.isEmpty()) return;
    const auto path = pendingPath;
    pendingPath.clear();
    const int id = generation;
    auto result = std::make_shared<Imported>();
    importer.start([path, result](const Canceled &c, const Progress &) {
      result->source = Replay::open(path, c);
      if (result->source->kind == Kind::Guacamole) {
        result->guac = std::make_shared<Guacamole>(result->source);
        result->preview = result->guac->at(0, c);
      } else if (result->source->kind == Kind::Cast) {
        result->cast = std::make_shared<CastPlayer>(result->source->path, c);
        result->preview = result->cast->at(0, c);
      }
    }, [this, result, id](const QString &error, bool canceled) {
      if (id != generation || canceled) { startImport(); return; }
      if (!error.isEmpty()) { status->setText(error); return; }
      source = result->source;
      if (auto *item = list->currentItem()) {
        const auto &asset = source->metadata.asset;
        item->setText(asset.isEmpty() ? source->label : asset);
        item->setToolTip(detailsTooltip(metadataFields(*source)));
      }
      guacamole = result->guac;
      cast = result->cast;
      status->clear();
      if (source->fragment)
        status->setText(text("当前仅播放首个分片；请打开完整 TAR 播放整段录像。",
                             "Playing the first fragment only. Open the complete TAR to play the full recording."));
      if (isMp4()) {
        ensureMedia();
        screens->setCurrentWidget(video);
        setLocalMedia(*player, QUrl::fromLocalFile(source->path));
        player->setPlaybackRate(rate());
      } else {
        screens->setCurrentWidget(canvas);
        canvas->setFrame(result->preview);
        setDuration(cast ? cast->duration() : source->duration());
      }
      updateDetails();
      refreshControls();
    });
  }

  void finishSeek(int at) {
    pendingSeek = -1;
    w->setProperty("replaySeekPending", false);
    const bool shouldResume = resumeAfterSeek && !playbackError && at < duration;
    resumeAfterSeek = false;
    if (shouldResume) {
      origin = at;
      playing = true;
      clock.restart();
      timer->start();
    }
    refreshControls();
  }

  void requestFrame(int at, int requestedVersion = -1) {
    if (!guacamole && !cast) return;
    requested = at;
    requestedSeekVersion = requestedVersion;
    if (renderer.busy()) return;
    const int activeVersion = requestedSeekVersion;
    requested = -1;
    requestedSeekVersion = -1;
    const int id = generation;
    auto image = std::make_shared<QImage>();
    renderer.start([g = guacamole, t = cast, at, image](const Canceled &c, const Progress &) {
      *image = g ? g->at(at, c) : t->at(at, c);
    }, [this, id, image, at, activeVersion](const QString &error, bool canceled) {
      if (id == generation && !canceled) {
        const bool currentRequest = activeVersion < 0 ? pendingSeek < 0 : activeVersion == seekVersion;
        if (!error.isEmpty()) {
          if (currentRequest && at == pendingSeek) {
            pendingSeek = -1;
            w->setProperty("replaySeekPending", false);
          }
          if (currentRequest) {
            resumeAfterSeek = false;
            playbackError = true;
            pause();
            status->setText(error);
          }
        } else if (requested < 0 && currentRequest) {
          canvas->setFrame(*image);
          setPosition(at);
          if (at == pendingSeek) {
            const qint64 remaining = kSeekLoadingMinimumMs - seekLoadingClock.elapsed();
            if (remaining > 0) {
              QTimer::singleShot(int(remaining), w, [this, id, at, activeVersion] {
                if (id == generation && activeVersion == seekVersion && pendingSeek == at)
                  finishSeek(at);
              });
            } else {
              finishSeek(at);
            }
          } else {
            refreshControls();
          }
        }
        if (playing && at >= duration) pause();
      }
      if (requested >= 0) requestFrame(requested, requestedSeekVersion);
    });
  }

  Window *w;
  Task importer, renderer;
  QListWidget *list;
  Canvas *canvas;
  LoadingOverlay *loadingOverlay;
  PlayOverlay *canvasOverlay, *videoOverlay = nullptr;
  QWidget *dropZone;
  QStackedWidget *screens, *playlistPages;
  QWidget *video = nullptr;
  QMediaPlayer *player = nullptr;
  QSlider *seek;
  QComboBox *speed;
  QLabel *dropTitle, *dropDescription, *playlistTitle, *emptyPlaylist, *status, *time;
  QLabel *count;
  QFrame *recordDetails;
  ClientTheme::ElidedLabel *assetLabel, *recordSummary, *recordTime, *segmentTime;
  QComboBox *segments;
  QPushButton *add, *dropOpen, *play, *external, *info, *playlistToggle;
  QTimer *timer;
  QElapsedTimer clock;
  QElapsedTimer seekLoadingClock;
  QString pendingPath;
  SourcePtr source;
  std::shared_ptr<Guacamole> guacamole;
  std::shared_ptr<CastPlayer> cast;
  int generation = 0, duration = 0, position = 0, origin = 0, requested = -1, pendingSeek = -1,
      requestedSeekVersion = -1, seekVersion = 0, displayedSegment = -1;
  bool updating = false, playing = false, playbackError = false;
  bool resumeAfterDrag = false, resumeAfterSeek = false;
};

Window::Window(QWidget *parent) : QDialog(parent), d(std::make_unique<Private>(this)) {
  d->canvas->installEventFilter(this);
  d->retranslate();
}
Window::~Window() {
  d->timer->stop();
  if (d->player) {
    disconnect(d->player, nullptr, this, nullptr);
    setLocalMedia(*d->player, {});
  }
}
bool Window::eventFilter(QObject *watched, QEvent *event) {
  if (watched == d->canvas || watched == d->video) {
    auto *widget = static_cast<QWidget *>(watched);
    auto *overlay = watched == d->canvas ? d->canvasOverlay : d->videoOverlay;
    if (!overlay) return QDialog::eventFilter(watched, event);
    if (event->type() == QEvent::Resize) {
      overlay->setGeometry(widget->rect());
      if (watched == d->canvas) d->loadingOverlay->setGeometry(widget->rect());
    }
    if (event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton && widget->rect().contains(static_cast<QMouseEvent *>(event)->pos())) {
      if (watched == d->canvas && d->pendingSeek >= 0) return true;
      d->toggle();
      return true;
    }
  }
  return QDialog::eventFilter(watched, event);
}
void Window::retranslate() { d->retranslate(); }
void Window::openFiles(const QStringList &paths) { d->addPaths(paths); }
void Window::dragEnterEvent(QDragEnterEvent *event) {
  for (const auto &url : event->mimeData()->urls())
    if (url.isLocalFile() && url.host().isEmpty()) {
      setDropHighlight(d->dropZone, true);
      event->acceptProposedAction();
      return;
    }
}
void Window::dragLeaveEvent(QDragLeaveEvent *event) {
  setDropHighlight(d->dropZone, false);
  event->accept();
}
void Window::dropEvent(QDropEvent *event) {
  setDropHighlight(d->dropZone, false);
  QStringList paths;
  for (const auto &url : event->mimeData()->urls())
    if (url.isLocalFile() && url.host().isEmpty()) paths.append(url.toLocalFile());
  if (!paths.isEmpty()) { openFiles(paths); event->acceptProposedAction(); }
}
void Window::closeEvent(QCloseEvent *event) {
  if (d->list->currentItem()) d->list->setCurrentRow(-1);
  else d->load({});
  hide();
  event->accept();
}

} // namespace Replay
