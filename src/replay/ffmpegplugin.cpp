#include "ffmpegplugin.h"
#include "platform.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QOperatingSystemVersion>
#include <QPair>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QVersionNumber>

#include <algorithm>

namespace Replay {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("OfflineReplay", s); }
struct Asset { const char *name; qint64 size; const char *digest; };
struct Package { QString target; Asset encoder; Asset probe; };
constexpr qint64 maxBinarySize = 192LL * 1024 * 1024;
constexpr int metadataLimit = 4096;

// These are GitHub's published SHA-256 values for the actual binaries, not
// values supplied by a response at install time. Upgrading means reviewing
// the upstream release and replacing this table together with the tag.
Package package() {
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
  // The pinned arm64 executables both have LC_BUILD_VERSION minos 12.0.
  if (QOperatingSystemVersion::current().majorVersion() < 12) return {};
  return {"darwin-arm64",
    {"ffmpeg", 45568216, "a90e3db6a3fd35f6074b013f948b1aa45b31c6375489d39e572bea3f18336584"},
    {"ffprobe", 45528808, "bb2db6f5d8cef919da12fbf592119a987202a8c060a886f3cab091f9cab90b64"}};
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
  return {"darwin-x64",
    {"ffmpeg", 78862176, "ebdddc936f61e14049a2d4b549a412b8a40deeff6540e58a9f2a2da9e6b18894"},
    {"ffprobe", 78780408, "fa3add0ce901f7241abe0dfc0155d958fc834aca3f8ce61f87cc712ae669c1e0"}};
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
  return {"win32-x64",
    {"ffmpeg", 82797568, "04e1307997530f9cf2fe35cba2ca7e8875ca91da02f89d6c7243df819c94ad00"},
    {"ffprobe", 82668032, "3a7e2dc003dc2cd1472827e4c7c4f056ae1ae0ae7c5bbc580c99b49827351ba4"}};
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
  return {"linux-x64",
    {"ffmpeg", 79826272, "e7e7fb30477f717e6f55f9180a70386c62677ef8a4d4d1a5d948f4098aa3eb99"},
    {"ffprobe", 79665792, "4f231a1960d83e403d08f7971e271707bec278a9ae18e21b8b5b03186668450d"}};
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
  return {"linux-arm64",
    {"ffmpeg", 51134160, "6bb182d0d75d23028db82e9e4f723ca69b853d055698486e6984ddb2c06fb8ce"},
    {"ffprobe", 50994160, "d17ae9b4c297d48e2521ba14e417bb0537c6ff77c584cdbcd6bb0d8d0307a2e8"}};
#else
  return {};
#endif
}

QString executable(const Asset &asset) {
  QString name = QString::fromLatin1(asset.name);
#ifdef Q_OS_WIN
  name += ".exe";
#endif
  return name;
}

QString markerName(const Package &p) {
  return "current-" + FfmpegPlugin::version() + '-' + p.target;
}

QVersionNumber minimumVersion() {
  return QVersionNumber(6, 0, 0);
}

QString pinnedRuntimeVersion() {
  return QStringLiteral("6.0");
}

bool parseNumericVersion(const QString &version, QVersionNumber *number) {
  const auto match = QRegularExpression(QStringLiteral(R"(^(\d+)\.(\d+)(?:\.(\d+))?)"))
      .match(version.trimmed());
  if (!match.hasMatch()) return false;
  bool majorOk = false, minorOk = false, patchOk = true;
  const int major = match.captured(1).toInt(&majorOk);
  const int minor = match.captured(2).toInt(&minorOk);
  const int patch = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt(&patchOk);
  if (!majorOk || !minorOk || !patchOk) return false;
  *number = QVersionNumber(major, minor, patch);
  return true;
}

QString displayVersion(const QString &version) {
  const auto match = QRegularExpression(QStringLiteral(R"(^(\d+)\.(\d+)(?:\.(\d+))?)"))
      .match(version.trimmed());
  if (!match.hasMatch()) return {};
  return match.captured(3).isEmpty() || match.captured(3) == QStringLiteral("0")
      ? match.captured(1) + '.' + match.captured(2)
      : match.captured(1) + '.' + match.captured(2) + '.' + match.captured(3);
}

bool isExecutable(const QString &path, qint64 size) {
  const QFileInfo info(path);
  return info.isFile() && !info.isSymLink() && info.isExecutable() && info.size() == size;
}

void verifyFile(const QString &path, qint64 size, const QByteArray &digest, const Canceled &canceled) {
  QFile file(path);
  if (!isExecutable(path, size) || !file.open(QIODevice::ReadOnly))
    throw Error{tr("The FFmpeg plugin is incomplete. Download it again in Settings.")};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    checkCanceled(canceled);
    const auto chunk = file.read(64 * 1024);
    if (chunk.isEmpty()) throw Error{file.errorString()};
    hash.addData(chunk);
  }
  if (hash.result().toHex() != digest)
    throw Error{tr("FFmpeg checksum verification failed. Download it again in Settings.")};
}

QString parseVersion(const QByteArray &output, const QByteArray &tool) {
  const auto first = QString::fromLatin1(output).section('\n', 0, 0).trimmed();
  const auto prefix = QString::fromLatin1(tool) + QStringLiteral(" version ");
  if (!first.startsWith(prefix)) throw Error{tr("The FFmpeg plugin reported an invalid version.")};
  const auto token = first.mid(prefix.size()).section(' ', 0, 0);
  QVersionNumber version;
  if (!parseNumericVersion(token, &version) || QVersionNumber::compare(version, minimumVersion()) < 0)
    throw Error{tr("The FFmpeg plugin requires FFmpeg 6.0 or newer.")};
  return displayVersion(token);
}

bool validDigest(const QByteArray &digest) {
  return digest.size() == 64 &&
      std::all_of(digest.cbegin(), digest.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}

bool allowedUrl(const QUrl &url) {
  const QString host = url.host();
  return url.scheme() == "https" && url.userInfo().isEmpty() && url.port(443) == 443 &&
      (host == "github.com" || host == "release-assets.githubusercontent.com" ||
       host == "objects.githubusercontent.com");
}

bool fileMatches(const QString &path, const Asset &asset, const Canceled &canceled) {
  const QFileInfo info(path);
  if (info.isSymLink() || !info.isFile() || info.size() != asset.size) return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return false;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    checkCanceled(canceled);
    const auto chunk = file.read(64 * 1024);
    if (chunk.isEmpty()) return false;
    hash.addData(chunk);
  }
  return hash.result().toHex() == asset.digest;
}

void download(QNetworkAccessManager &manager, QUrl url, const QString &path, const Asset &asset,
              const Canceled &canceled, const std::function<void(qint64)> &received) {
  if (QFileInfo::exists(path)) {
    const QFileInfo existing(path);
    if (existing.isSymLink() || !existing.isFile() || existing.size() > asset.size) {
      if (!QFile::remove(path)) throw Error{tr("Cannot reset the interrupted FFmpeg download.")};
    } else if (existing.size() == asset.size) {
      if (fileMatches(path, asset, canceled)) {
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner))
          throw Error{tr("Cannot prepare the downloaded FFmpeg binary.")};
        received(asset.size);
        return;
      }
      QFile::remove(path);
    }
  }
  for (int hop = 0; hop < 5; ++hop) {
    checkCanceled(canceled);
    if (!allowedUrl(url)) throw Error{tr("The FFmpeg download was redirected to an untrusted host.")};
    const QSettings settings;
    QString proxyFailure;
    auto mode = settings.value("proxy/mode", "system").toString().trimmed();
    if (mode != "manual" && mode != "none") mode = QStringLiteral("system");
    auto proxies = resolveClientProxies(url, mode,
        settings.value("proxy/http").toString().trimmed(), &proxyFailure);
    if (!proxyFailure.isEmpty() || proxies.isEmpty())
      throw Error{proxyFailure.isEmpty() ? tr("Cannot resolve the configured proxy.") : proxyFailure};
    for (;;) {
      qint64 offset = QFileInfo(path).size();
      if (offset < 0) offset = 0;
      manager.setProxy(proxies.takeFirst());
      QNetworkRequest request(url);
      request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
      request.setRawHeader("User-Agent", "OSMClient-OfflineReplay");
      if (offset > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
      QFile file(path);
      if (!file.open(QIODevice::ReadWrite)) throw Error{file.errorString()};
      if (offset == 0) {
        if (!file.resize(0) || !file.seek(0)) throw Error{file.errorString()};
      } else if (!file.seek(offset)) throw Error{file.errorString()};
      std::unique_ptr<QNetworkReply> reply(manager.get(request));
      reply->setReadBufferSize(256 * 1024);
      QEventLoop loop;
      QTimer poll;
      QElapsedTimer elapsed, idle;
      elapsed.start(); idle.start();
      qint64 downloaded = 0;
      bool rangeChecked = false;
      QString failure;
      auto drain = [&] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && status != 206) {
          reply->readAll();
          return;
        }
        if (!rangeChecked) {
          rangeChecked = true;
          if (status == 206) {
            const auto contentRange = reply->rawHeader("Content-Range");
            const auto match = QRegularExpression(QStringLiteral(R"(^bytes (\d+)-(\d+)/(\d+|\*)$)"))
                .match(QString::fromLatin1(contentRange).trimmed());
            bool ok = false;
            const qint64 start = match.captured(1).toLongLong(&ok);
            if (!match.hasMatch() || !ok || start != file.pos()) {
              failure = tr("The FFmpeg server returned an invalid resume range.");
            } else if (match.captured(3) != QStringLiteral("*")) {
              bool totalOk = false;
              const qint64 total = match.captured(3).toLongLong(&totalOk);
              if (!totalOk || total != asset.size)
                failure = tr("The FFmpeg server returned an unexpected file size.");
            }
          } else if (file.pos() > 0) {
            if (!file.resize(0) || !file.seek(0))
              failure = file.errorString();
            else
              offset = 0;
          }
        }
        if (!failure.isEmpty()) {
          reply->readAll();
          if (!reply->isFinished()) reply->abort();
          return;
        }
        const auto data = reply->readAll();
        if (data.isEmpty() || !failure.isEmpty()) return;
        downloaded += data.size();
        idle.restart();
        if (file.pos() + data.size() > asset.size) failure = tr("The FFmpeg download exceeded its expected size.");
        else if (file.write(data) != data.size()) failure = file.errorString();
        else received(file.pos());
        if (!failure.isEmpty() && !reply->isFinished()) reply->abort();
      };
      QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, drain);
      QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
      QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (canceled && canceled()) reply->abort();
        else if (idle.elapsed() > 45000 || elapsed.elapsed() > 600000) {
          failure = tr("The FFmpeg download timed out.");
          reply->abort();
        }
      });
      poll.start(100);
      if (!reply->isFinished()) loop.exec();
      checkCanceled(canceled);
      drain();
      if (!failure.isEmpty()) throw Error{failure};
      const auto error = reply->error();
      const bool proxyError = error >= QNetworkReply::ProxyConnectionRefusedError &&
          error <= QNetworkReply::UnknownProxyError && error != QNetworkReply::ProxyAuthenticationRequiredError;
      if (proxyError && downloaded == 0 && offset == 0 && !proxies.isEmpty()) continue;
      if (error != QNetworkReply::NoError)
        throw Error{tr("Cannot download FFmpeg: %1. Retry to continue.").arg(reply->errorString().section("?", 0, 0))};
      const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
      if (!redirect.isEmpty()) { url = url.resolved(redirect); break; }
      if (!file.flush()) throw Error{file.errorString()};
      file.close();
      if (QFileInfo(path).size() != asset.size)
        throw Error{tr("The FFmpeg download is incomplete. Try again to continue.")};
      if (!fileMatches(path, asset, canceled)) {
        QFile::remove(path);
        throw Error{tr("The downloaded FFmpeg binary failed checksum verification.")};
      }
      if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner))
        throw Error{tr("Cannot prepare the downloaded FFmpeg binary.")};
      received(asset.size);
      return;
    }
  }
  throw Error{tr("Too many redirects while downloading FFmpeg.")};
}

QString smoke(const FfmpegPlugin &tools, const Canceled &canceled) {
  QProcess encoder;
  encoder.start(tools.ffmpeg, {"-hide_banner", "-encoders"}, QIODevice::ReadOnly);
  waitForExit(encoder, 10000, canceled);
  const QByteArray encoders = encoder.readAllStandardOutput();
  if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() || !encoders.contains(" libx264 "))
    throw Error{tr("The FFmpeg plugin cannot run or does not provide the H.264 encoder.")};

  QProcess version;
  version.start(tools.ffmpeg, {"-hide_banner", "-version"}, QIODevice::ReadOnly);
  waitForExit(version, 10000, canceled);
  const auto encoderOutput = version.readAllStandardOutput();
  if (version.exitStatus() != QProcess::NormalExit || version.exitCode())
    throw Error{tr("The FFmpeg plugin cannot run on this system.")};
  const auto encoderVersion = parseVersion(encoderOutput, "ffmpeg");

  QProcess probe;
  probe.start(tools.ffprobe, {"-hide_banner", "-version"}, QIODevice::ReadOnly);
  waitForExit(probe, 10000, canceled);
  const auto probeOutput = probe.readAllStandardOutput();
  if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode())
    throw Error{tr("The plugin's ffprobe cannot run on this system.")};
  if (parseVersion(probeOutput, "ffprobe") != encoderVersion)
    throw Error{tr("The FFmpeg and ffprobe versions do not match.")};
  return encoderVersion;
}

bool writeMetadata(const QDir &stage, const Package &p, const FfmpegPlugin &tools) {
  QJsonObject ffmpeg{{"size", double(tools.encoderSize)}, {"sha256", QString::fromLatin1(tools.encoderDigest)}};
  QJsonObject ffprobe{{"size", double(tools.probeSize)}, {"sha256", QString::fromLatin1(tools.probeDigest)}};
  QJsonObject root{{"schema", 1}, {"version", tools.installedVersion}, {"platform", p.target},
      {"ffmpeg", ffmpeg}, {"ffprobe", ffprobe}};
  QSaveFile file(stage.filePath("plugin.json"));
  if (!file.open(QIODevice::WriteOnly)) return false;
  const auto data = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (file.write(data) != data.size()) return false;
  return file.commit();
}

bool readAsset(const QJsonObject &root, const QString &key, qint64 *size, QByteArray *digest) {
  const auto asset = root.value(key).toObject();
  const double value = asset.value("size").toDouble(-1);
  const auto hash = asset.value("sha256").toString().toLatin1();
  if (!asset.value("size").isDouble() || value <= 0 || value > maxBinarySize || value != qint64(value) ||
      !validDigest(hash)) return false;
  *size = qint64(value);
  *digest = hash;
  return true;
}

bool readMetadata(const QDir &install, FfmpegPlugin *tools) {
  QFile file(install.filePath("plugin.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > metadataLimit) return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  const auto root = document.object();
  const auto storedVersion = root.value("version").toString();
  QVersionNumber version;
  if (root.value("schema").toInt() != 1 || root.value("platform").toString() != package().target ||
      !parseNumericVersion(storedVersion, &version) ||
      QVersionNumber::compare(version, minimumVersion()) < 0) return false;
  qint64 encoderSize = -1, probeSize = -1;
  QByteArray encoderDigest, probeDigest;
  if (!readAsset(root, "ffmpeg", &encoderSize, &encoderDigest) ||
      !readAsset(root, "ffprobe", &probeSize, &probeDigest)) return false;
  const auto p = package();
  const QString ffmpeg = install.filePath(executable(p.encoder));
  const QString ffprobe = install.filePath(executable(p.probe));
  if (!isExecutable(ffmpeg, encoderSize) || !isExecutable(ffprobe, probeSize)) return false;
  *tools = FfmpegPlugin{ffmpeg, ffprobe, displayVersion(storedVersion), encoderSize, probeSize,
      encoderDigest, probeDigest};
  return true;
}

FfmpegPlugin activate(QTemporaryDir &stage, const QDir &root, const Package &p,
                      const FfmpegPlugin &tools, const Canceled &canceled, const Progress &progress) {
  checkCanceled(canceled);
  const auto old = FfmpegPlugin::installed(root.absolutePath());
  if (!writeMetadata(QDir(stage.path()), p, tools)) throw Error{tr("Cannot write FFmpeg plugin metadata.")};
  QSaveFile marker(root.filePath("current"));
  if (!marker.open(QIODevice::WriteOnly)) throw Error{marker.errorString()};
  writeAll(marker, QFileInfo(stage.path()).fileName().toLatin1());
  if (!marker.commit()) throw Error{marker.errorString()};
  stage.setAutoRemove(false);
  if (old) {
    QDir previous(QFileInfo(old.ffmpeg).absolutePath());
    if (previous.absolutePath() != QDir(stage.path()).absolutePath()) previous.removeRecursively();
  }
  for (const auto &markerName : root.entryList({"current-*"}, QDir::Files)) QFile::remove(root.filePath(markerName));
  if (progress) progress(100);
  return tools;
}
} // namespace

QString FfmpegPlugin::version() { return QStringLiteral("b6.1.1"); }
bool FfmpegPlugin::supported() { return !package().target.isEmpty(); }
QString FfmpegPlugin::root() {
  return QDir(ConfigStore::configDirectory()).absoluteFilePath("replay-plugins");
}

FfmpegPlugin FfmpegPlugin::installed(const QString &directory) {
  const auto p = package();
  if (p.target.isEmpty()) return {};
  const QDir root(directory);
  const QStringList markers{QStringLiteral("current"), markerName(p)};
  for (const auto &name : markers) {
    QFile marker(root.filePath(name));
    if (QFileInfo(marker).isSymLink() || !marker.open(QIODevice::ReadOnly) || marker.size() > 80) continue;
    const QString dir = QString::fromLatin1(marker.readAll()).trimmed();
    if (!dir.startsWith("ffmpeg-") || dir.size() != 13) continue;
    bool validName = true;
    for (QChar c : dir.mid(7)) validName = validName && c.isLetterOrNumber() && c.unicode() < 128;
    if (!validName) continue;
    const QFileInfo folder(root.filePath(dir));
    if (!folder.isDir() || folder.isSymLink()) continue;
    const QDir install(folder.absoluteFilePath());
    FfmpegPlugin tools;
    if (readMetadata(install, &tools)) return tools;
    if (name == markerName(p)) {
      const FfmpegPlugin legacy{install.filePath(executable(p.encoder)),
          install.filePath(executable(p.probe)), version(),
          p.encoder.size, p.probe.size, QByteArray(p.encoder.digest), QByteArray(p.probe.digest)};
      if (isExecutable(legacy.ffmpeg, legacy.encoderSize) && isExecutable(legacy.ffprobe, legacy.probeSize))
        return legacy;
    }
  }
  return {};
}

void FfmpegPlugin::verify(const Canceled &canceled) const {
  if (!*this || encoderSize <= 0 || probeSize <= 0 ||
      !validDigest(encoderDigest) || !validDigest(probeDigest))
    throw Error{tr("Download the FFmpeg plugin in Settings first.")};
  verifyFile(ffmpeg, encoderSize, encoderDigest, canceled);
  verifyFile(ffprobe, probeSize, probeDigest, canceled);
}

FfmpegPlugin FfmpegPlugin::install(const QString &directory, const Canceled &canceled, const Progress &progress) {
  const auto p = package();
  if (p.target.isEmpty()) throw Error{tr("No FFmpeg download is available for this platform.")};
  const QDir root(directory);
  if (!QDir().mkpath(root.absolutePath())) throw Error{tr("Cannot create the FFmpeg plugin directory.")};
  QLockFile lock(root.filePath("install.lock"));
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) throw Error{tr("An FFmpeg download is already in progress.")};
  QDir cache(root.absoluteFilePath(".download-" + version() + '-' + p.target));
  if (!QDir().mkpath(cache.absolutePath())) throw Error{tr("Cannot create the FFmpeg download directory.")};
  FfmpegPlugin cached{cache.filePath(executable(p.encoder)), cache.filePath(executable(p.probe))};
  cached.encoderSize = p.encoder.size;
  cached.probeSize = p.probe.size;
  cached.encoderDigest = QByteArray(p.encoder.digest);
  cached.probeDigest = QByteArray(p.probe.digest);
  const QString base = "https://github.com/eugeneware/ffmpeg-static/releases/download/" + version() + '/';
  const qint64 total = p.encoder.size + p.probe.size;
  QNetworkAccessManager manager;
  if (progress) progress(0);
  download(manager, QUrl(base + p.encoder.name + '-' + p.target), cached.ffmpeg, p.encoder, canceled,
           [&](qint64 n) { if (progress) progress(int(98 * n / total)); });
  download(manager, QUrl(base + p.probe.name + '-' + p.target), cached.ffprobe, p.probe, canceled,
           [&](qint64 n) { if (progress) progress(int(98 * (p.encoder.size + n) / total)); });
  cached.installedVersion = smoke(cached, canceled);
  QTemporaryDir stage(root.filePath("ffmpeg-XXXXXX"));
  if (!stage.isValid()) throw Error{tr("Cannot stage the FFmpeg plugin locally.")};
  FfmpegPlugin tools{stage.filePath(executable(p.encoder)), stage.filePath(executable(p.probe)),
      cached.installedVersion, p.encoder.size, p.probe.size,
      QByteArray(p.encoder.digest), QByteArray(p.probe.digest)};
  const QList<QPair<QString, QString>> files{
      {cached.ffmpeg, tools.ffmpeg}, {cached.ffprobe, tools.ffprobe}};
  for (const auto &path : files) {
    if (!QFile::copy(path.first, path.second) ||
        !QFile::setPermissions(path.second, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner))
      throw Error{tr("Cannot stage the downloaded FFmpeg plugin.")};
  }
  const auto result = activate(stage, root, p, tools, canceled, progress);
  cache.removeRecursively();
  return result;
}

FfmpegPlugin FfmpegPlugin::importArchive(const QString &archive, const QString &directory,
                                         const Canceled &canceled, const Progress &progress) {
  const auto p = package();
  if (p.target.isEmpty()) throw Error{tr("No FFmpeg plugin is available for this platform.")};
  const QDir root(directory);
  if (!QDir().mkpath(root.absolutePath())) throw Error{tr("Cannot create the FFmpeg plugin directory.")};
  QLockFile lock(root.filePath("install.lock"));
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) throw Error{tr("The FFmpeg plugin is busy. Try again later.")};
  QTemporaryDir stage(root.filePath("ffmpeg-XXXXXX"));
  if (!stage.isValid()) throw Error{tr("Cannot stage the FFmpeg plugin locally.")};
  const QString encoder = executable(p.encoder), probe = executable(p.probe);
  FfmpegPlugin tools{stage.filePath(encoder), stage.filePath(probe)};
  if (progress) progress(0);
  const auto archiveSize = qMax<qint64>(1, QFileInfo(archive).size());
  extractPluginTar(archive, stage.path(), {encoder, probe, QStringLiteral("SHA256SUMS")}, canceled,
      [&](qint64 n) { if (progress) progress(int(qMin<qint64>(90, n * 90 / archiveSize))); });
  QFile sums(stage.filePath("SHA256SUMS"));
  if (!sums.open(QIODevice::ReadOnly)) throw Error{sums.errorString()};
  QHash<QByteArray, QByteArray> entries;
  for (const auto &raw : sums.readAll().split('\n')) {
    const auto line = raw.trimmed();
    if (line.isEmpty()) continue;
    const auto match = QRegularExpression(QStringLiteral(R"(^([0-9a-fA-F]{64})\s+\*?([^\s]+)$)"))
        .match(QString::fromLatin1(line));
    const auto name = match.captured(2).toLatin1();
    if (!match.hasMatch() || entries.contains(name)) throw Error{tr("Invalid plugin checksum file.")};
    entries.insert(name, match.captured(1).toLatin1().toLower());
  }
  if (entries.size() != 2 || !entries.contains(encoder.toLatin1()) || !entries.contains(probe.toLatin1()))
    throw Error{tr("The plugin checksum file does not match this platform.")};
  tools.encoderSize = QFileInfo(tools.ffmpeg).size();
  tools.probeSize = QFileInfo(tools.ffprobe).size();
  if (tools.encoderSize <= 0 || tools.encoderSize > maxBinarySize ||
      tools.probeSize <= 0 || tools.probeSize > maxBinarySize)
    throw Error{tr("The plugin contains an invalid binary size.")};
  tools.encoderDigest = entries.value(encoder.toLatin1());
  tools.probeDigest = entries.value(probe.toLatin1());
  for (const auto &path : {tools.ffmpeg, tools.ffprobe}) {
    QFile file(path);
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
      throw Error{file.errorString()};
  }
  verifyFile(tools.ffmpeg, tools.encoderSize, tools.encoderDigest, canceled);
  verifyFile(tools.ffprobe, tools.probeSize, tools.probeDigest, canceled);
  tools.installedVersion = smoke(tools, canceled);
  if (tools.installedVersion == pinnedRuntimeVersion() &&
      (tools.encoderSize != p.encoder.size || tools.probeSize != p.probe.size ||
       tools.encoderDigest != p.encoder.digest || tools.probeDigest != p.probe.digest))
    throw Error{tr("The plugin labeled as FFmpeg 6.0 does not match the trusted build.")};
  if (progress) progress(98);
  return activate(stage, root, p, tools, canceled, progress);
}

void stopProcess(QProcess &process) {
  if (process.state() == QProcess::NotRunning) return;
  process.terminate();
  if (!process.waitForFinished(300)) {
    process.kill();
    process.waitForFinished(3000);
  }
}

void waitForExit(QProcess &process, int timeoutMs, const Canceled &canceled) {
  QElapsedTimer elapsed;
  elapsed.start();
  try {
    while (process.state() != QProcess::NotRunning) {
      checkCanceled(canceled);
      process.waitForFinished(100);
      if (elapsed.elapsed() > timeoutMs) throw Error{tr("The FFmpeg process timed out.")};
    }
    checkCanceled(canceled);
    if (process.error() == QProcess::FailedToStart) throw Error{process.errorString()};
  } catch (...) {
    stopProcess(process);
    throw;
  }
}

} // namespace Replay
