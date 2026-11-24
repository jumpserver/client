#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>
#include <functional>
#include <memory>

namespace Replay {

// These errors are displayed as plain text, including messages from local files.
struct Error { QString message; };
using Canceled = std::function<bool()>;
void checkCanceled(const Canceled &canceled);
void writeAll(QIODevice &destination, const QByteArray &data);
void inflateGzip(QIODevice &input, qint64 bytes, QIODevice &output, qint64 limit,
                 const Canceled &canceled = {});

struct Instruction {
  QByteArray opcode;
  QList<QByteArray> args;
};

class InstructionReader {
public:
  explicit InstructionReader(QFile &file, qint64 offset = 0);
  bool next(Instruction &instruction);
  qint64 offset() const { return m_offset; }

private:
  int peek();
  int take();
  QByteArray element();
  QFile &m_file;
  QByteArray m_block;
  int m_pos = 0;
  qint64 m_offset = 0;
};

enum class Kind { Guacamole, Mp4, Cast };
struct Metadata {
  QString id, user, asset, account, protocol, remoteAddress;
  QDateTime started, ended;
  qint64 durationMs = -1;
};
struct RecordingInfo {
  Kind kind = Kind::Guacamole;
  bool fragment = false;
  QString label;
  Metadata metadata;
  int partCount = 0;
};
struct Segment {
  int index = 0;
  int start = 0, end = 0; // positions on the decoded, continuous timeline
  QDateTime started;
};
struct Frame {
  int ms;
  qint64 end;
};
struct Source : RecordingInfo {
  QString path;
  std::unique_ptr<QTemporaryDir> workspace;
  QVector<Frame> frames;
  QVector<Segment> segments;
  QSize largestScreen;
  int duration() const { return frames.isEmpty() ? 0 : frames.constLast().ms; }
};
using SourcePtr = std::shared_ptr<const Source>;

SourcePtr open(const QString &path, const Canceled &canceled = {});
void extractPluginTar(const QString &path, const QString &directory, const QStringList &names,
                      const Canceled &canceled = {}, const std::function<void(qint64)> &progress = {});
// Read archive headers and metadata without extracting or decoding the video.
RecordingInfo inspect(const QString &path, const Canceled &canceled = {});
int number(const QByteArray &value, int min, int max);

} // namespace Replay
