#pragma once

#include "replaysource.h"
#include <QProcess>

namespace Replay {

using Progress = std::function<void(int)>; // -1 means indeterminate
struct FfmpegPlugin {
  QString ffmpeg;
  QString ffprobe;
  QString installedVersion;
  qint64 encoderSize = -1, probeSize = -1;
  QByteArray encoderDigest, probeDigest;
  explicit operator bool() const { return !ffmpeg.isEmpty() && !ffprobe.isEmpty(); }

  static QString version();
  static bool supported();
  static QString root();
  static FfmpegPlugin installed(const QString &directory = root());
  static FfmpegPlugin install(const QString &directory, const Canceled &canceled = {},
                              const Progress &progress = {});
  static FfmpegPlugin importArchive(const QString &archive, const QString &directory,
                                    const Canceled &canceled = {}, const Progress &progress = {});
  void verify(const Canceled &canceled = {}) const;
};

void waitForExit(QProcess &process, int timeoutMs, const Canceled &canceled);
void stopProcess(QProcess &process);

} // namespace Replay
