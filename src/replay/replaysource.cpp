#include "replaysource.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace Replay {
namespace {
constexpr int blockSize = 64 * 1024;
constexpr qint64 gib = 1024LL * 1024 * 1024;
constexpr qint64 archiveLimit = 32 * gib;
constexpr qint64 guacLimit = 16 * gib;
constexpr qint64 castLimit = 32 * 1024 * 1024;
constexpr int maxDuration = 24 * 60 * 60 * 1000;

QString tr(const char *s) { return QCoreApplication::translate("OfflineReplay", s); }
[[noreturn]] void badArchive() { throw Error{tr("Invalid or incomplete replay archive.")}; }
[[noreturn]] void badInstruction() { throw Error{tr("Invalid or truncated RDP/VNC recording instruction.")}; }

QByteArray readExact(QIODevice &file, qint64 bytes) {
  const QByteArray data = file.read(bytes);
  if (data.size() != bytes) badArchive();
  return data;
}

quint64 octal(const QByteArray &value) {
  const QByteArray field = value.split('\0').constFirst().trimmed();
  if (field.isEmpty()) return 0;
  for (char c : field) if (c < '0' || c > '7') badArchive();
  bool ok;
  const quint64 v = field.toULongLong(&ok, 8);
  if (!ok) badArchive();
  return v;
}

bool safeName(const QString &name) {
  if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
      name.contains('\\') || name.contains(':') || name.size() > 512 ||
      name.endsWith('.') || name.endsWith(' ')) return false;
  for (QChar c : name) if (c.category() == QChar::Other_Control) return false;
  return true;
}

struct Member {
  qint64 offset = 0;
  qint64 size = 0;
};

// QMediaPlayer receives only self-contained ISO media files. In particular,
// a reference movie or a playlist renamed to .mp4 must not make playback
// depend on an external file or URL.
void checkMp4(QFile &file, const Member &member, const Canceled &canceled) {
  auto bad = [] { throw Error{tr("Only self-contained local MP4 video is supported.")}; };
  int elements = 0;
  bool ftyp = false, moov = false, mdat = false;
  auto bytes = [&](qint64 at, int count) {
    if (!file.seek(at)) bad();
    auto data = file.read(count);
    if (data.size() != count) bad();
    return data;
  };
  std::function<void(qint64, qint64, int)> walk = [&](qint64 at, qint64 end, int depth) {
    while (at < end) {
      if (++elements > 100000 || end - at < 8) bad();
      if (!(elements % 128)) checkCanceled(canceled);
      const auto head = bytes(at, 8);
      const QByteArray type = head.mid(4);
      quint64 size = qFromBigEndian<quint32>(head.constData());
      int header = 8;
      if (size == 1) {
        if (end - at < 16) bad();
        size = qFromBigEndian<quint64>(bytes(at + 8, 8).constData());
        header = 16;
      } else if (size == 0) size = end - at;
      if (size < quint64(header) || size > quint64(end - at)) bad();
      const qint64 body = at + header, next = at + qint64(size);
      if (type == "rmra" || type == "rmda" || depth > 6) bad();
      if (depth == 0) {
        ftyp |= type == "ftyp" && next - body >= 8;
        moov |= type == "moov";
        mdat |= type == "mdat" && next > body;
      }
      if (type == "dref") {
        if (next - body < 8) bad();
        const auto full = bytes(body, 8);
        const auto count = qFromBigEndian<quint32>(full.constData() + 4);
        qint64 entry = body + 8;
        if (count > 64) bad();
        for (quint32 i = 0; i < count; ++i) {
          if (next - entry < 12) bad();
          const auto data = bytes(entry, 12);
          if (qFromBigEndian<quint32>(data.constData()) != 12 || data.mid(4, 4) != "url " ||
              qFromBigEndian<quint32>(data.constData() + 8) != 1) bad();
          entry += 12;
        }
        if (entry != next) bad();
      } else if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "dinf") {
        walk(body, next, depth + 1);
      }
      at = next;
    }
  };
  walk(member.offset, member.offset + member.size, 0);
  if (!ftyp || !moov || !mdat) bad();
}

// JumpServer emits plain tar with regular files at its root. Python tarfile
// emits PAX records for fractional mtimes/long UTF-8 names. Never use a tar
// entry as an output path; record offsets and write only generated names.
QHash<QString, Member> members(QFile &file, const Canceled &canceled) {
  QHash<QString, Member> found;
  QSet<QString> names;
  QString paxPath;
  bool pendingPax = false;
  const QByteArray zeros(512, '\0');
  for (int entries = 0; entries < 20000; ++entries) {
    checkCanceled(canceled);
    const QByteArray header = readExact(file, 512);
    if (header == zeros) {
      if (pendingPax || found.isEmpty() || readExact(file, 512) != zeros) badArchive();
      return found;
    }
    quint64 checksum = 0;
    for (int i = 0; i < header.size(); ++i)
      checksum += (i >= 148 && i < 156) ? 32 : quint8(header.at(i));
    if (checksum != octal(header.mid(148, 8))) badArchive();
    const quint64 n = octal(header.mid(124, 12));
    const qint64 start = file.pos();
    if (n > quint64(archiveLimit) || (n + 511) / 512 * 512 > quint64(file.size() - start))
      badArchive();
    const qint64 next = start + qint64((n + 511) / 512 * 512);
    const char type = header.at(156);
    if (type == 'x') {
      if (pendingPax || n > 64 * 1024) badArchive();
      pendingPax = true;
      const QByteArray data = readExact(file, n);
      int offset = 0;
      while (offset < data.size()) {
        const int space = data.indexOf(' ', offset);
        if (space <= offset || space - offset > 7) badArchive();
        const int length = number(data.mid(offset, space - offset), 1, data.size());
        if (length > data.size() - offset || offset + length <= space + 2 ||
            data.at(offset + length - 1) != '\n') badArchive();
        const QByteArray field = data.mid(space + 1, offset + length - space - 2);
        const int eq = field.indexOf('=');
        if (eq < 1) badArchive();
        const QByteArray key = field.left(eq);
        if (key == "path") {
          if (!paxPath.isEmpty()) badArchive();
          paxPath = QString::fromUtf8(field.mid(eq + 1));
          if (!safeName(paxPath)) badArchive();
        } else if (key == "size" || key == "linkpath" || key.startsWith("GNU.sparse")) {
          badArchive();
        }
        offset += length;
      }
    } else if (type == '0' || type == '\0') {
      const QByteArray prefix = header.mid(345, 155).split('\0').constFirst();
      QString name = QString::fromUtf8(header.left(100).split('\0').constFirst());
      if (!prefix.isEmpty()) name.prepend(QString::fromUtf8(prefix) + '/');
      if (!paxPath.isEmpty()) name = paxPath;
      if (!safeName(name) || names.contains(name.toCaseFolded()) || !n) badArchive();
      names.insert(name.toCaseFolded());
      found.insert(name, {start, qint64(n)});
      paxPath.clear();
      pendingPax = false;
    } else {
      badArchive();
    }
    if (!file.seek(next)) badArchive();
  }
  badArchive();
}

struct Part {
  Member member;
  qint64 start = -1;
  qint64 duration = -1;
  qint64 end = 0; // end offset in the normalized instruction file
};

qint64 timestamp(const QJsonValue &value) {
  const double n = value.toDouble(-1);
  return n >= 0 && n < 9007199254740992.0 && std::floor(n) == n ? qint64(n) : -1;
}

QJsonObject readMetadata(QFile &file, const Member &meta) {
  if (meta.size > 1024 * 1024 || !file.seek(meta.offset)) return {};
  return QJsonDocument::fromJson(readExact(file, meta.size)).object();
}

Metadata sessionMetadata(const QJsonObject &obj) {
  const auto field = [&obj](const char *key) { return obj.value(QLatin1String(key)).toString().left(512).simplified(); };
  const auto date = [&field](const char *key) {
    QString value = field(key);
    value.replace('/', '-');
    if (value.size() > 10 && value.at(10) == ' ') value[10] = 'T';
    value.remove(' ');
    return QDateTime::fromString(value, Qt::ISODate);
  };
  Metadata metadata{field("id"), field("user"), field("asset"), field("account"), field("protocol"),
                    field("remote_addr"), date("date_start"), date("date_end")};
  const auto duration = field("duration");
  const auto time = QTime::fromString(duration, QStringLiteral("hh:mm:ss"));
  if (time.isValid()) metadata.durationMs = time.msecsSinceStartOfDay();
  else if (metadata.started.isValid() && metadata.ended.isValid()) {
    const auto ms = metadata.started.msecsTo(metadata.ended);
    if (ms >= 0 && ms <= maxDuration) metadata.durationMs = ms;
  }
  return metadata;
}

QVector<Part> partList(const QJsonObject &obj, const QString &manifest, const QHash<QString, Member> &entries) {
  const QString id = manifest.left(manifest.size() - int(sizeof(".replay.json") - 1));
  const auto list = obj.value("files").toArray();
  if (obj.value("id").toString() != id || list.isEmpty()) badArchive();
  const QRegularExpression partPattern("^" + QRegularExpression::escape(id) + R"(\.(0|[1-9][0-9]*)\.part\.gz$)");
  QMap<int, Part> ordered;
  for (const QJsonValue &entry : list) {
    const QJsonObject fields = entry.toObject();
    const auto name = fields.value("name").toString();
    const auto match = partPattern.match(name);
    bool ok = false;
    const int index = match.captured(1).toInt(&ok);
    if (!ok || !entries.contains(name) || ordered.contains(index)) badArchive();
    ordered.insert(index, {entries.value(name), timestamp(fields.value("start")),
                           timestamp(fields.value("duration")), 0});
  }
  QVector<Part> result;
  for (auto it = ordered.cbegin(); it != ordered.cend(); ++it) {
    if (it.key() != result.size()) badArchive();
    result.append(it.value());
  }
  if (entries.size() != result.size() + 1) badArchive();
  return result;
}

struct Input {
  RecordingInfo info;
  QVector<Part> parts;
  Member payload;
  QString media;
};

Input readInput(QFile &file, const Canceled &canceled) {
  Input input;
  auto &info = input.info;
  info.label = QFileInfo(file.fileName()).fileName();
  const auto name = info.label.toLower();
  input.media = name;
  input.payload = {0, file.size()};
  if (name.endsWith(".tar")) {
    const auto entries = members(file, canceled);
    QString manifest;
    Member sidecar;
    input.media.clear();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
      const QString item = it.key().toLower();
      if (item.endsWith(".replay.json")) {
        if (!manifest.isEmpty()) badArchive();
        manifest = it.key();
      } else if (item.endsWith(".replay.gz") || item.endsWith(".mp4") ||
                 item.endsWith(".cast.gz") || item.endsWith(".cast")) {
        if (!input.media.isEmpty()) badArchive();
        input.media = item;
        input.payload = it.value();
      } else if (item.endsWith(".json")) {
        sidecar = it.value();
      } else if (!item.endsWith(".part.gz")) {
        badArchive();
      }
    }
    if (!manifest.isEmpty()) {
      const auto obj = readMetadata(file, entries.value(manifest));
      if (input.media.isEmpty())
        input.parts = partList(obj, manifest, entries);
      else if (!obj.value("files").toArray().isEmpty() || entries.size() != 2)
        badArchive();
      info.metadata = sessionMetadata(obj);
    } else {
      if (input.media.isEmpty()) throw Error{tr("This archive does not contain a supported offline recording.")};
      if (entries.size() > 2) badArchive();
      for (const QString &entry : entries.keys())
        if (entry.toLower().endsWith(".part.gz")) badArchive();
      if (sidecar.size) info.metadata = sessionMetadata(readMetadata(file, sidecar));
    }
  }
  if (input.media.endsWith(".replay.gz")) input.parts.append({input.payload});
  if (name.endsWith(".0.part.gz")) {
    info.fragment = true;
    input.parts.append({input.payload});
  }
  if (input.media.endsWith(".cast.gz") || input.media.endsWith(".cast")) info.kind = Kind::Cast;
  else if (input.media.endsWith(".mp4")) info.kind = Kind::Mp4;
  else if (input.parts.isEmpty())
    throw Error{tr("Unsupported recording. For RDP/VNC protocol fragments, open the complete offline TAR.")};
  info.partCount = input.parts.size();
  return input;
}

void openInput(QFile &file) {
  const QFileInfo info(file.fileName());
  if (!info.isFile() || info.size() <= 0 || info.size() > archiveLimit)
    throw Error{tr("The selected replay is not a supported local file or exceeds the size limit.")};
  if (!file.open(QIODevice::ReadOnly)) throw Error{file.errorString()};
}

void scanGuacamole(Source &source, const QVector<Part> &parts, const Canceled &canceled) {
  QFile file(source.path);
  if (!file.open(QIODevice::ReadOnly)) throw Error{file.errorString()};
  InstructionReader reader(file);
  Instruction instruction;
  int part = 0;
  int syncPart = -1;
  qint64 firstRaw = -1, prevRaw = -1, lastInPart = -1, origin = 0;
  qint64 prevStart = -1, prevFirstMs = 0, prevDuration = -1;
  int seen = 0;
  for (;;) {
    if (!reader.next(instruction)) break;
    // A command can span two read blocks (or even two gzip parts); use the
    // part that contains its terminator to associate a sync with a manifest.
    while (part + 1 < parts.size() && reader.offset() > parts.at(part).end) ++part;
    if ((++seen % 256) == 0) checkCanceled(canceled);
    const auto &args = instruction.args;
    if (instruction.opcode == "size" && args.size() == 3 && args[0] == "0") {
      const QSize size(number(args[1], 0, 8192), number(args[2], 0, 8192));
      if (!size.isEmpty()) source.largestScreen = source.largestScreen.expandedTo(size);
    }
    if (instruction.opcode != "sync") continue;
    if (source.fragment && source.frames.isEmpty() && source.largestScreen.isEmpty())
      throw Error{tr("The fragment does not contain its initial screen. Open the complete offline TAR.")};
    bool ok = false;
    const qint64 raw = args.isEmpty() ? -1 : args[0].toLongLong(&ok);
    if (!ok || raw < 0 || raw > 9007199254740991LL || source.frames.size() >= 1000000) badInstruction();
    if (syncPart != part) {
      const auto &current = parts.at(part);
      if (firstRaw < 0) {
        firstRaw = raw;
        origin = raw;
      } else if (raw < prevRaw) {
        // Some producers reset the clock for each part. Only bridge a reset
        // using consistent manifest timing; never silently drop a time gap.
        const qint64 startDelta = current.start - prevStart;
        const int last = source.duration();
        if (prevStart < 0 || current.start < 0 || startDelta < 0 ||
            prevDuration < 0 || qAbs(prevDuration - (last - prevFirstMs)) > 1000 ||
            prevFirstMs + startDelta < last || prevFirstMs + startDelta > maxDuration)
          throw Error{tr("Cannot reconcile the RDP/VNC recording part timestamps.")};
        origin = raw - (prevFirstMs + startDelta);
      }
      prevStart = current.start;
      prevDuration = current.duration;
      prevFirstMs = raw - origin;
      lastInPart = -1;
      syncPart = part;
    }
    const qint64 ms = raw - origin;
    if (raw < lastInPart || ms < source.duration() || ms > maxDuration)
      throw Error{tr("The RDP/VNC protocol recording timeline is invalid or exceeds 24 hours.")};
    if (source.segments.isEmpty() || source.segments.constLast().index != part) {
      const auto start = parts.at(part).start;
      // Small values are offsets, not wall clock times.
      const auto started = start >= 100000000000LL && start <= 253402300799999LL
          ? QDateTime::fromMSecsSinceEpoch(start, Qt::UTC) : QDateTime();
      source.segments.append({part, int(ms), int(ms), started});
    }
    source.segments.last().end = int(ms);
    source.frames.append({int(ms), reader.offset()});
    prevRaw = lastInPart = raw;
  }
  if (source.frames.isEmpty() || source.largestScreen.isEmpty())
    throw Error{tr("No display frames were found in the RDP/VNC protocol recording.")};
}
} // namespace

void checkCanceled(const Canceled &canceled) {
  if (canceled && canceled()) throw Error{tr("Canceled.")};
}

void writeAll(QIODevice &destination, const QByteArray &data) {
  qint64 offset = 0;
  while (offset < data.size()) {
    const auto written = destination.write(data.constData() + offset, data.size() - offset);
    if (written <= 0) throw Error{tr("Cannot write the local replay file: %1").arg(destination.errorString())};
    offset += written;
  }
}

void inflateGzip(QIODevice &input, qint64 bytes, QIODevice &output, qint64 limit, const Canceled &canceled) {
  z_stream z{};
  if (inflateInit2(&z, 15 + 16) != Z_OK) throw Error{tr("Cannot start gzip decompression.")};
  struct End { z_stream *stream; ~End() { inflateEnd(stream); } } end{&z};
  QByteArray compressed;
  QByteArray buffer(blockSize, '\0');
  qint64 produced = 0;
  for (;;) {
    checkCanceled(canceled);
    if (!z.avail_in && bytes) {
      compressed = input.read(qMin<qint64>(blockSize, bytes));
      if (compressed.isEmpty()) throw Error{tr("Truncated gzip data.")};
      bytes -= compressed.size();
      z.next_in = reinterpret_cast<Bytef *>(compressed.data());
      z.avail_in = uInt(compressed.size());
    }
    z.next_out = reinterpret_cast<Bytef *>(buffer.data());
    z.avail_out = uInt(buffer.size());
    const int status = inflate(&z, Z_NO_FLUSH);
    const int count = buffer.size() - int(z.avail_out);
    produced += count;
    if (produced > limit) throw Error{tr("The decompressed file exceeds the size limit.")};
    if (count) writeAll(output, QByteArray::fromRawData(buffer.constData(), count));
    if (status == Z_STREAM_END) {
      if (!z.avail_in && !bytes) return;
      if (inflateReset(&z) != Z_OK) throw Error{tr("Invalid gzip member.")};
    } else if (status != Z_OK) {
      throw Error{tr("Invalid or truncated gzip data.")};
    }
  }
}

InstructionReader::InstructionReader(QFile &file, qint64 offset) : m_file(file), m_offset(offset) {
  if (!m_file.seek(offset)) throw Error{m_file.errorString()};
}

int InstructionReader::peek() {
  if (m_pos == m_block.size()) {
    m_block = m_file.read(blockSize);
    m_pos = 0;
    if (m_block.isEmpty()) {
      if (m_file.error() != QFileDevice::NoError) throw Error{m_file.errorString()};
      return -1;
    }
  }
  return quint8(m_block.at(m_pos));
}

int InstructionReader::take() {
  const int c = peek();
  if (c >= 0) { ++m_pos; ++m_offset; }
  return c;
}

QByteArray InstructionReader::element() {
  int length = 0;
  int digits = 0;
  for (int c = take(); c != '.'; c = take()) {
    if (c < '0' || c > '9' || ++digits > 8) badInstruction();
    length = length * 10 + c - '0';
    if (length > 8 * 1024 * 1024) badInstruction();
  }
  if (!digits) badInstruction();
  QByteArray bytes;
  bytes.reserve(length);
  while (length > 0) {
    if (peek() < 0) badInstruction();
    const auto available = qMin(length, int(m_block.size()) - m_pos);
    const char *start = m_block.constData() + m_pos;
    int ascii = 0;
    while (ascii < available && quint8(start[ascii]) < 0x80) ++ascii;
    if (ascii) {
      bytes.append(start, ascii);
      m_pos += ascii;
      m_offset += ascii;
      length -= ascii;
      continue;
    }
    const int first = take();
    const int extra = first >= 0xc2 && first <= 0xdf ? 1 : first <= 0xef && first >= 0xe0 ? 2 :
                      first >= 0xf0 && first <= 0xf4 ? 3 : -1;
    if (extra < 0) badInstruction();
    bytes.append(char(first));
    for (int i = 0; i < extra; ++i) {
      const int c = take();
      if (c < 0x80 || c > 0xbf || (!i && ((first == 0xe0 && c < 0xa0) ||
          (first == 0xed && c >= 0xa0) || (first == 0xf0 && c < 0x90) ||
          (first == 0xf4 && c >= 0x90)))) badInstruction();
      bytes.append(char(c));
    }
    --length;
  }
  return bytes;
}

bool InstructionReader::next(Instruction &instruction) {
  while (peek() == '\r' || peek() == '\n') take();
  if (peek() < 0) return false;
  instruction.opcode = element();
  if (instruction.opcode.isEmpty() || instruction.opcode.size() > 32) badInstruction();
  instruction.args.clear();
  qint64 bytes = 0;
  for (int sep = take(); sep != ';'; sep = take()) {
    if (sep != ',' || instruction.args.size() >= 32) badInstruction();
    instruction.args.append(element());
    bytes += instruction.args.constLast().size();
    if (bytes > 16 * 1024 * 1024) badInstruction();
  }
  return true;
}

int number(const QByteArray &value, int min, int max) {
  bool ok;
  const int n = value.toInt(&ok);
  if (!ok || n < min || n > max) badInstruction();
  return n;
}

SourcePtr open(const QString &path, const Canceled &canceled) {
  QFile file(QFileInfo(path).absoluteFilePath());
  openInput(file);
  auto input = readInput(file, canceled);
  const auto &media = input.media;
  const auto &payload = input.payload;
  auto &parts = input.parts;
  auto source = std::make_shared<Source>();
  static_cast<RecordingInfo &>(*source) = input.info;
  source->path = file.fileName();
  if (source->kind == Kind::Mp4) checkMp4(file, payload, canceled);
  if (source->label.endsWith(".mp4", Qt::CaseInsensitive)) return source;
  if (source->label.endsWith(".cast", Qt::CaseInsensitive)) {
    if (file.size() > castLimit) throw Error{tr("The terminal recording exceeds the size limit.")};
    return source;
  }

  source->workspace = std::make_unique<QTemporaryDir>(QDir::temp().filePath("osm-replay-XXXXXX"));
  if (!source->workspace->isValid()) throw Error{tr("Cannot create a private replay directory.")};
  const QString suffix = source->kind == Kind::Mp4 ? ".mp4" : source->kind == Kind::Cast ? ".cast" : ".guac";
  source->path = source->workspace->filePath("recording" + suffix);
  QFile output(source->path);
  if (!output.open(QIODevice::WriteOnly)) throw Error{output.errorString()};
  if (source->kind == Kind::Guacamole) {
    for (auto &part : parts) {
      if (!file.seek(part.member.offset)) badArchive();
      inflateGzip(file, part.member.size, output, guacLimit - output.pos(), canceled);
      part.end = output.pos();
    }
  } else {
    if (!file.seek(payload.offset)) badArchive();
    if (media.endsWith(".gz")) inflateGzip(file, payload.size, output, castLimit, canceled);
    else {
      if (source->kind == Kind::Cast && payload.size > castLimit)
        throw Error{tr("The terminal recording exceeds the size limit.")};
      for (qint64 left = payload.size; left > 0;) {
        checkCanceled(canceled);
        const auto data = readExact(file, qMin<qint64>(blockSize, left));
        writeAll(output, data);
        left -= data.size();
      }
    }
  }
  if (!output.flush()) throw Error{output.errorString()};
  output.close();
  if (source->kind == Kind::Guacamole) scanGuacamole(*source, parts, canceled);
  checkCanceled(canceled);
  return source;
}

RecordingInfo inspect(const QString &path, const Canceled &canceled) {
  QFile file(QFileInfo(path).absoluteFilePath());
  openInput(file);
  return readInput(file, canceled).info;
}

void extractPluginTar(const QString &path, const QString &directory, const QStringList &names,
                      const Canceled &canceled, const std::function<void(qint64)> &progress) {
  QFile file(QFileInfo(path).absoluteFilePath());
  openInput(file);
  if (file.size() > 256LL * 1024 * 1024) throw Error{tr("The plugin archive exceeds the size limit.")};
  const auto entries = members(file, canceled);
  if (entries.size() != names.size()) throw Error{tr("The plugin archive must contain exactly the required files.")};
  qint64 copied = 0;
  for (const auto &name : names) {
    if (!entries.contains(name)) throw Error{tr("The plugin archive is missing a required file.")};
    const auto entry = entries.value(name);
    if (name == "SHA256SUMS" && entry.size > 4096) throw Error{tr("The plugin checksum file is too large.")};
    if (!file.seek(entry.offset)) throw Error{file.errorString()};
    QFile output(QDir(directory).filePath(name));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Error{output.errorString()};
    for (qint64 left = entry.size; left > 0;) {
      checkCanceled(canceled);
      const auto bytes = readExact(file, qMin<qint64>(blockSize, left));
      writeAll(output, bytes);
      left -= bytes.size();
      copied += bytes.size();
      if (progress) progress(copied);
    }
  }
}

} // namespace Replay
