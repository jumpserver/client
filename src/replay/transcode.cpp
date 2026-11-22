#include "transcode.h"
#include "guacamole.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPainter>
#include <QRegularExpression>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <memory>

namespace Replay {
namespace {
constexpr int fps = 10;
constexpr int interval = 1000 / fps;
QString tr(const char *s) { return QCoreApplication::translate("OfflineReplay", s); }

void verifyVideo(const QString &probePath, const QString &path, const QSize &size,
                 int frames, const Canceled &canceled) {
  QProcess probe;
  probe.setWorkingDirectory(QFileInfo(probePath).absolutePath());
  probe.start(probePath, {"-v", "error", "-protocol_whitelist", "file",
      "-show_entries", "stream=codec_name,codec_type,width,height,pix_fmt,nb_frames,duration",
      "-of", "json", path}, QIODevice::ReadOnly);
  waitForExit(probe, 15000, canceled);
  const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value("streams").toArray();
  const auto info = streams.isEmpty() ? QJsonObject() : streams.at(0).toObject();
  const double duration = info.value("duration").toString().toDouble();
  if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() || streams.size() != 1 ||
      info.value("codec_name") != "h264" || info.value("codec_type") != "video" ||
      info.value("pix_fmt") != "yuv420p" || info.value("width").toInt() != size.width() ||
      info.value("height").toInt() != size.height() || info.value("nb_frames").toString().toInt() != frames ||
      std::abs(duration - double(frames) / fps) > 0.001)
    throw Error{tr("The exported video did not pass ffprobe validation.")};
}

class OutputGuard {
public:
  OutputGuard(QFile &file, const QString &path) : m_file(file), m_path(path) {}
  ~OutputGuard() { if (!m_committed) discard(); }
  void commit() {
    m_file.close();
    m_committed = true;
  }
  bool discard() {
    if (!m_committed) {
      m_file.resize(0);
      m_file.close();
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (!QFileInfo::exists(m_path) || QFile::remove(m_path)) return true;
      QThread::msleep(100);
    }
    return false;
  }
private:
  QFile &m_file;
  QString m_path;
  bool m_committed = false;
};

} // namespace

QSize outputSize(const QSize &largest, Resolution resolution) {
  QSize size = largest;
  QSize limit;
  switch (resolution) {
  case Resolution::Original: break;
  case Resolution::P1080: limit = {1920, 1080}; break;
  case Resolution::P720: limit = {1280, 720}; break;
  case Resolution::P360: limit = {640, 360}; break;
  }
  if (limit.isValid() && (size.width() > limit.width() || size.height() > limit.height()))
    size.scale(limit, Qt::KeepAspectRatio);
  // H.264 4:2:0 needs even dimensions. At most one pixel of padding per axis.
  return {qMax(2, (size.width() + 1) & ~1), qMax(2, (size.height() + 1) & ~1)};
}

QString outputFilename(const RecordingInfo &info, FilenameStyle style, const QString &customName) {
  const auto clean = [](QString name, int limit) {
    name.replace(QRegularExpression(QStringLiteral(R"([/\\:*?"<>|\p{Cc}\p{Cf}])")), "_");
    name = name.simplified();
    while (name.toUtf8().size() > limit) {
      name.chop(1);
      if (!name.isEmpty() && name.at(name.size() - 1).isHighSurrogate()) name.chop(1);
    }
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    while (name.startsWith('.') || name.startsWith(' ')) name.remove(0, 1);
    return name;
  };
  QString fallback = info.label;
  fallback.chop(fallback.endsWith(".replay.gz", Qt::CaseInsensitive) ? 10 :
                fallback.endsWith(".tar", Qt::CaseInsensitive) ? 4 : 0);
  QString name = customName.trimmed();
  if (name.endsWith(".mp4", Qt::CaseInsensitive)) name.chop(4);
  name = clean(name, 200);
  if (name.isEmpty() && style != FilenameStyle::Original) {
    const auto &m = info.metadata;
    QStringList fields;
    for (const auto &value : {m.user, m.asset, m.account}) {
      const auto part = clean(value, 45);
      if (!part.isEmpty()) fields << part;
    }
    name = fields.join('-');
    if (!name.isEmpty() && style == FilenameStyle::SessionWithId && !m.id.isEmpty())
      name += '(' + clean(m.id, 56) + ')';
  }
  if (name.isEmpty()) name = clean(info.metadata.id, 200);
  if (name.isEmpty()) name = clean(fallback, 200);
  if (name.isEmpty()) name = QStringLiteral("recording");
  static const QRegularExpression reserved(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³]) *(\.|$))"),
                                           QRegularExpression::CaseInsensitiveOption);
  if (reserved.match(name).hasMatch()) name.prepend('_');
  return name + ".mp4";
}

void transcode(const SourcePtr &source, const QString &destination, const QString &pluginRoot,
               const Canceled &canceled, const Progress &progress, Resolution resolution,
               QLockFile *heldPluginLock) {
  if (!source || source->kind != Kind::Guacamole) {
    throw Error{tr("Only RDP/VNC protocol recordings can be exported as MP4.")};
  }
  if (source->fragment) throw Error{tr("Open the complete offline TAR to export this recording.")};
  if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
    throw Error{tr("The output already exists. Choose another file name.")};
  const QSize size = outputSize(source->largestScreen, resolution);
  if (qint64(size.width()) * size.height() > 20 * 1024 * 1024)
    throw Error{tr("The original resolution is too large. Select a lower output resolution.")};
  std::unique_ptr<QLockFile> localPluginLock;
  if (!heldPluginLock) {
    localPluginLock = std::make_unique<QLockFile>(QDir(pluginRoot).filePath("install.lock"));
    localPluginLock->setStaleLockTime(0);
    if (!localPluginLock->tryLock(0))
      throw Error{tr("The FFmpeg plugin is busy. Try again after its current task finishes.")};
  }
  const auto tools = FfmpegPlugin::installed(pluginRoot);
  tools.verify(canceled);
  const QFileInfo output(destination);
  // Keep one direct output file so antivirus readers never block a final rename.
  // The held handle also reserves the path and lets failed exports be truncated.
  QFile directOutput(output.absoluteFilePath());
  if (!directOutput.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
      throw Error{tr("The output already exists. Choose another file name.")};
    throw Error{tr("Cannot save the verified video in the selected folder (%1).").arg(directOutput.error())};
  }
  const QString encodedPath = directOutput.fileName();
  OutputGuard outputGuard(directOutput, encodedPath);
  QProcess encoder;
  int frames = 0;
  try {
    frames = source->duration() / interval + 1;
    const QString frameSize = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
    encoder.setWorkingDirectory(QFileInfo(tools.ffmpeg).absolutePath());
    encoder.start(tools.ffmpeg, {"-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-protocol_whitelist", "pipe", "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", frameSize,
        "-framerate", QString::number(fps), "-i", "pipe:0", "-an", "-threads", "4", "-c:v", "libx264",
        "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", "-g", "20",
        "-movflags", "+faststart", "-progress", "pipe:1", "-f", "mp4", encodedPath});
    int reported = -1;
    QByteArray stats;
    auto drain = [&] {
      encoder.readAllStandardError();
      stats.append(encoder.readAllStandardOutput());
      int end;
      while ((end = stats.indexOf('\n')) >= 0) {
        const QByteArray line = stats.left(end);
        stats.remove(0, end + 1);
        if (line.startsWith("frame=")) {
          const int p = qBound(0, line.mid(6).trimmed().toInt() * 98 / frames, 98);
          if (p > reported) { reported = p; if (progress) progress(p); }
        }
      }
    };
    if (!encoder.waitForStarted(5000)) throw Error{tr("Cannot start FFmpeg: %1").arg(encoder.errorString())};
    if (progress) progress(0);
    Guacamole replay(source);
    QImage frame;
    const int stride = size.width() * 3;
    QByteArray packed(stride * size.height(), '\0');
    if (packed.isEmpty()) throw Error{tr("Cannot allocate the video canvas.")};
    QImage canvas(reinterpret_cast<uchar *>(packed.data()), size.width(), size.height(),
                  stride, QImage::Format_RGB888);
    if (canvas.isNull()) throw Error{tr("Cannot allocate the video canvas.")};
    for (int index = 0; index < frames; ++index) {
      checkCanceled(canceled);
      const QImage screen = replay.at(index + 1 == frames ? source->duration() : index * interval, canceled);
      if (index == 0 || screen.cacheKey() != frame.cacheKey()) {
        frame = screen;
        canvas.fill(Qt::black);
        if (!frame.isNull()) {
          const QSize fitted = frame.size().scaled(size, Qt::KeepAspectRatio);
          const QRect rect(QPoint((size.width() - fitted.width()) / 2, (size.height() - fitted.height()) / 2), fitted);
          QPainter p(&canvas);
          p.setRenderHint(QPainter::SmoothPixmapTransform);
          p.drawImage(rect, frame);
        }
      }
      QElapsedTimer idle;
      idle.start();
      // Feed at most one complete raw frame ahead. Waiting on this worker
      // thread also drains both process channels, keeping the GUI responsive.
      while (encoder.bytesToWrite() > 0 && encoder.state() == QProcess::Running) {
        checkCanceled(canceled);
        if (encoder.waitForBytesWritten(100)) idle.restart();
        drain();
        if (idle.elapsed() > 30000) throw Error{tr("FFmpeg stopped accepting frames.")};
      }
      drain();
      if (encoder.state() != QProcess::Running || encoder.write(packed) != packed.size())
        throw Error{tr("FFmpeg failed while encoding. Check the output folder and available disk space.")};
    }
    encoder.closeWriteChannel();
    QElapsedTimer finalize;
    finalize.start();
    while (encoder.state() != QProcess::NotRunning) {
      checkCanceled(canceled);
      encoder.waitForFinished(100);
      drain();
      if (finalize.elapsed() > 120000) throw Error{tr("FFmpeg did not finish the MP4 in time.")};
    }
    drain();
    if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0)
      throw Error{tr("FFmpeg could not finish the MP4. Check the output folder and available disk space.")};
    if (progress) progress(99);
    verifyVideo(tools.ffprobe, encodedPath, size, frames, canceled);
    checkCanceled(canceled);
    outputGuard.commit();
  } catch (const Error &error) {
    stopProcess(encoder);
    if (!outputGuard.discard())
      throw Error{tr("%1 The incomplete output file could not be removed. Delete it before trying again.")
                    .arg(error.message)};
    throw;
  } catch (...) {
    stopProcess(encoder);
    if (!outputGuard.discard())
      throw Error{tr("The incomplete output file could not be removed. Delete it before trying again.")};
    throw;
  }
  if (progress) progress(100);
}

} // namespace Replay
