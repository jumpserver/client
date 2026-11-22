#pragma once

#include "ffmpegplugin.h"

class QLockFile;

namespace Replay {

enum class Resolution { Original, P1080, P720, P360 };
enum class FilenameStyle { Original, Session, SessionWithId };
QSize outputSize(const QSize &source, Resolution resolution);
QString outputFilename(const RecordingInfo &info, FilenameStyle style, const QString &customName = {});
void transcode(const SourcePtr &source, const QString &destination, const QString &pluginRoot,
               const Canceled &canceled = {}, const Progress &progress = {},
               Resolution resolution = Resolution::P1080, QLockFile *heldPluginLock = nullptr);

} // namespace Replay
