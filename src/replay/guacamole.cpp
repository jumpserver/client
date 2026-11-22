#include "guacamole.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace Replay {
namespace {
constexpr qint64 mib = 1024 * 1024;
constexpr qint64 surfaceLimit = 160 * mib;
constexpr qint64 checkpointBudget = 24 * mib;
constexpr int checkpointLimit = 2;
constexpr int dimLimit = 8192;
QString tr(const char *s) { return QCoreApplication::translate("OfflineReplay", s); }
[[noreturn]] void invalid() { throw Error{tr("Invalid RDP/VNC recording drawing data.")}; }
[[noreturn]] void tooLarge() { throw Error{tr("The RDP/VNC recording renderer exceeded its resource limit.")}; }
qint64 area(const QSize &size) { return qint64(size.width()) * size.height(); }

struct Graphics {
  QPainterPath clip;
  bool clipped = false;
  QTransform transform;
};
struct Layer {
  QImage image;
  int parent = 0;
  int z = 0;
  int order = 0;
  int alpha = 255;
  QPoint position;
  QTransform presentation;
  bool fixedSize = false;
  Graphics graphics;
  QVector<Graphics> stack;
  QPainterPath path;
  bool pathClosed = false;
};
struct ImageStream {
  int layer = 0;
  int mask = 14;
  QPoint position;
  QByteArray format;
  QByteArray bytes;
};

QPainter::CompositionMode composition(int mask) {
  switch (mask) {
    case 1: return QPainter::CompositionMode_DestinationIn;
    case 2: return QPainter::CompositionMode_DestinationOut;
    case 4: return QPainter::CompositionMode_SourceIn;
    case 6: return QPainter::CompositionMode_SourceAtop;
    case 8: return QPainter::CompositionMode_SourceOut;
    case 9: return QPainter::CompositionMode_DestinationAtop;
    case 10: return QPainter::CompositionMode_Xor;
    case 11: return QPainter::CompositionMode_DestinationOver;
    case 12: return QPainter::CompositionMode_Source;
    case 14: return QPainter::CompositionMode_SourceOver;
    case 15: return QPainter::CompositionMode_Plus;
    default: throw Error{tr("Unsupported RDP/VNC recording composition mask: %1").arg(mask)};
  }
}

QByteArray decode64(const QByteArray &encoded) {
  for (const char c : encoded) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '/' || c == '+' || c == '=')) invalid();
  }
  if (encoded.size() % 4 == 1) invalid();
  const int padding = encoded.indexOf('=');
  if (padding >= 0 && (encoded.size() % 4 || encoded.size() - padding > 2 ||
      (encoded.size() - padding == 2 && encoded.at(padding + 1) != '='))) invalid();
  return QByteArray::fromBase64(encoded);
}

QByteArray imageFormat(const QByteArray &mime) {
  if (mime == "image/png") return "png";
  if (mime == "image/jpeg") return "jpeg";
  if (mime == "image/webp") return "webp";
  throw Error{tr("The RDP/VNC protocol recording uses an unsupported image format.")};
}

struct Display {
  QMap<int, Layer> layers;
  QMap<int, ImageStream> streams;
  QImage cursor;
  QPoint hotspot, mouse;
  bool cursorVisible = false;
  qint64 surfaceBytes = 0, pendingBytes = 0;
  int serial = 0;

  qint64 estimatedBytes() const {
    // Checkpoints are copy-on-write. Once the live display changes, its image
    // surfaces and path data can detach, so account for the full state here.
    qint64 bytes = surfaceBytes + pendingBytes + cursor.sizeInBytes();
    for (auto it = layers.cbegin(); it != layers.cend(); ++it) {
      const Layer &value = it.value();
      bytes += sizeof(Layer);
      bytes += value.path.elementCount() * qint64(sizeof(QPainterPath::Element));
      bytes += value.graphics.clip.elementCount() * qint64(sizeof(QPainterPath::Element));
      for (const Graphics &graphics : value.stack)
        bytes += sizeof(Graphics) + graphics.clip.elementCount() * qint64(sizeof(QPainterPath::Element));
    }
    for (const ImageStream &stream : streams)
      bytes += sizeof(ImageStream) + stream.format.capacity() + stream.bytes.capacity();
    return bytes;
  }

  Layer &layer(int id) {
    auto it = layers.find(id);
    if (it != layers.end()) return it.value();
    if (layers.size() >= 2048) tooLarge();
    Layer added;
    added.order = ++serial;
    return layers.insert(id, added).value();
  }

  void resize(Layer &target, const QSize &size) {
    if (size == target.image.size()) return;
    if (size.width() > dimLimit || size.height() > dimLimit || area(size) > 20 * mib) tooLarge();
    const qint64 bytes = size.isEmpty() ? 0 : 4 * area(size);
    if (surfaceBytes - target.image.sizeInBytes() + bytes > surfaceLimit) tooLarge();
    QImage image;
    if (bytes) {
      image = QImage(size, QImage::Format_ARGB32_Premultiplied);
      if (image.isNull()) tooLarge();
      image.fill(Qt::transparent);
      QPainter p(&image);
      p.setCompositionMode(QPainter::CompositionMode_Source);
      p.drawImage(0, 0, target.image);
    }
    surfaceBytes += bytes - target.image.sizeInBytes();
    target.image = image;
    target.graphics = {};
    target.stack.clear();
    target.path = {};
    target.pathClosed = false;
  }

  void fit(Layer &target, int id, const QRect &rect) {
    if (target.fixedSize || id == 0 || rect.right() < 0 || rect.bottom() < 0) return;
    resize(target, target.image.size().expandedTo({rect.right() + 1, rect.bottom() + 1}));
  }

  void paintImage(const ImageStream &stream) {
    QBuffer buffer;
    buffer.setData(stream.bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, stream.format);
    reader.setAutoDetectImageFormat(false);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > dimLimit || size.height() > dimLimit ||
        area(size) > 20 * mib) throw Error{tr("Cannot decode a RDP/VNC recording %1 image; check the Qt image plugin.")
                                           .arg(QString::fromLatin1(stream.format))};
    const QImage image = reader.read();
    if (image.isNull()) invalid();
    Layer &target = layer(stream.layer);
    fit(target, stream.layer, QRect(stream.position, size));
    if (target.image.isNull()) return;
    QPainter p(&target.image);
    configure(p, target, stream.mask);
    p.drawImage(stream.position, image);
  }

  static void configure(QPainter &p, const Layer &target, int mask) {
    p.setCompositionMode(composition(mask));
    if (target.graphics.clipped) p.setClipPath(target.graphics.clip);
    p.setWorldTransform(target.graphics.transform);
  }

  void dispose(int id) {
    if (id == 0) return;
    auto it = layers.find(id);
    if (it == layers.end()) return;
    surfaceBytes -= it.value().image.sizeInBytes();
    layers.erase(it);
    // Children of a disposed layer are no longer in the visible tree.
    if (id > 0) for (Layer &l : layers) if (l.parent == id) l.parent = -1;
  }

  void draw(const Instruction &instruction) {
    const auto &op = instruction.opcode;
    const auto &a = instruction.args;
    auto need = [&](int n) { if (a.size() != n) invalid(); };
    auto i = [&](int p, int lo = -1000000, int hi = 1000000) {
      if (p >= a.size()) invalid();
      return number(a[p], lo, hi);
    };
    auto id = [&](int p) { return i(p); };
    auto point = [&](int p) { return QPoint(i(p), i(p + 1)); };
    auto size = [&](int p) { return QSize(i(p, 0, dimLimit), i(p + 1, 0, dimLimit)); };
    auto matrix = [&](int p) {
      qreal v[6];
      for (int j = 0; j < 6; ++j) {
        bool ok;
        v[j] = a[p + j].toDouble(&ok);
        if (!ok || !std::isfinite(v[j]) || qAbs(v[j]) > 1000000) invalid();
      }
      return QTransform(v[0], v[1], v[2], v[3], v[4], v[5]);
    };
    auto beginPath = [](Layer &l) {
      if (l.pathClosed) { l.path = {}; l.pathClosed = false; }
      if (l.path.elementCount() > 65536) tooLarge();
    };

    if (op == "size") {
      need(3);
      Layer &target = layer(id(0));
      resize(target, size(1));
      target.fixedSize = id(0) >= 0;
    } else if (op == "img") {
      need(6);
      const int stream = id(0);
      if (streams.contains(stream) || streams.size() >= 32) invalid();
      streams.insert(stream, {id(2), i(1, 0, 15), point(4), imageFormat(a[3]), {}});
    } else if (op == "blob") {
      need(2);
      auto stream = streams.find(id(0));
      if (stream == streams.end()) return; // audio, clipboard, or other ignored streams
      const QByteArray data = decode64(a[1]);
      pendingBytes += data.size();
      if (pendingBytes > 32 * mib) tooLarge();
      stream->bytes.append(data);
    } else if (op == "end") {
      need(1);
      auto stream = streams.find(id(0));
      if (stream == streams.end()) return;
      const ImageStream complete = stream.value();
      pendingBytes -= complete.bytes.size();
      streams.erase(stream);
      paintImage(complete);
    } else if (op == "png" || op == "jpeg" || op == "webp") {
      need(5);
      paintImage({id(1), i(0, 0, 15), point(2), op, decode64(a[4])});
    } else if (op == "copy" || op == "transfer") {
      need(9);
      const int mode = i(5, 0, 15);
      if (op == "transfer" && mode == 5) return;
      const int src = id(0), dst = id(6);
      const QImage from = layer(src).image;
      const QRect requested(point(1), size(3));
      const QRect rect = requested.intersected(from.rect());
      if (rect.isEmpty()) return;
      const QPoint at = point(7) + rect.topLeft() - requested.topLeft();
      Layer &target = layer(dst);
      fit(target, dst, QRect(at, rect.size()));
      if (target.image.isNull()) return;
      if (op == "copy" || mode == 3) {
        QPainter p(&target.image);
        if (op == "copy") configure(p, target, mode);
        else p.setCompositionMode(QPainter::CompositionMode_Source);
        if (src != dst) p.drawImage(QRect(at, rect.size()), from, rect);
        else p.drawImage(at, from.copy(rect));
      } else {
        const QRect clipped = QRect(at, rect.size()).intersected(target.image.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
          for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor s = from.pixelColor(rect.x() + x - at.x(), rect.y() + y - at.y());
            const QColor d = target.image.pixelColor(x, y);
            auto transfer = [mode](int src, int dst) {
              return (((mode & 1) ? src & dst : 0) | ((mode & 2) ? src & ~dst : 0) |
                      ((mode & 4) ? ~src & dst : 0) | ((mode & 8) ? ~src & ~dst : 0)) & 255;
            };
            target.image.setPixelColor(x, y, QColor(transfer(s.red(), d.red()), transfer(s.green(), d.green()),
                transfer(s.blue(), d.blue()), mode == 12 ? s.alpha() : d.alpha()));
          }
        }
      }
    } else if (op == "cursor") {
      need(7);
      const auto &from = layer(id(2)).image;
      const QSize dimensions(i(5, 0, 512), i(6, 0, 512));
      cursor = dimensions.isEmpty() ? QImage() : from.copy(QRect(point(3), dimensions));
      hotspot = point(0);
    } else if (op == "mouse") {
      if (a.size() < 2) invalid();
      mouse = point(0);
      cursorVisible = true;
    } else if (op == "move") {
      need(5);
      const int child = id(0), parent = id(1);
      if (child <= 0 || parent < 0) return;
      layer(parent);
      int depth = 0;
      for (int at = parent; at > 0; at = layers.value(at).parent)
        if (at == child || ++depth > 32) invalid();
      Layer &target = layer(child);
      target.parent = parent;
      target.position = point(2);
      target.z = i(4);
    } else if (op == "shade") {
      need(2);
      layer(id(0)).alpha = i(1, 0, 255);
    } else if (op == "distort") {
      need(7);
      layer(id(0)).presentation = matrix(1);
    } else if (op == "dispose") {
      need(1);
      dispose(id(0));
    } else if (op == "rect" || op == "start" || op == "line" || op == "curve" || op == "arc") {
      const int n = op == "rect" ? 5 : (op == "curve" || op == "arc") ? 7 : 3;
      need(n);
      const int index = id(0);
      Layer &target = layer(index);
      if (op == "rect") fit(target, index, QRect(point(1), size(3)));
      beginPath(target);
      if (op == "rect") target.path.addRect(QRect(point(1), size(3)));
      else if (op == "start") target.path.moveTo(point(1));
      else if (op == "line") target.path.lineTo(point(1));
      else if (op == "curve") target.path.cubicTo(point(1), point(3), point(5));
      else {
        bool startOk, endOk;
        const qreal start = a[4].toDouble(&startOk), end = a[5].toDouble(&endOk);
        if (!startOk || !endOk || !std::isfinite(start) || !std::isfinite(end) ||
            qAbs(start) > 1000000 || qAbs(end) > 1000000) invalid();
        const bool counter = i(6, 0, 1) != 0;
        const int radius = i(3, 0, dimLimit);
        const QPoint center = point(1);
        constexpr qreal degrees = 57.29577951308232, full = 6.283185307179586;
        qreal sweep = counter ? start - end : end - start;
        if (sweep >= full) sweep = full;
        else if (sweep < 0) sweep = std::fmod(std::fmod(sweep, full) + full, full);
        const QRectF bounds(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
        if (target.path.isEmpty()) target.path.arcMoveTo(bounds, -start * degrees);
        target.path.arcTo(bounds, -start * degrees, (counter ? sweep : -sweep) * degrees);
      }
    } else if (op == "close") {
      need(1);
      layer(id(0)).path.closeSubpath();
      layer(id(0)).pathClosed = true;
    } else if (op == "clip") {
      need(1);
      auto &target = layer(id(0));
      const auto clip = target.graphics.transform.map(target.path);
      auto &g = target.graphics;
      g.clip = g.clipped ? g.clip.intersected(clip) : clip;
      g.clipped = true;
      target.pathClosed = true;
    } else if (op == "cfill" || op == "cstroke" || op == "lfill" || op == "lstroke") {
      const bool stroke = op.endsWith("stroke"), pattern = op.startsWith('l');
      need(pattern ? (stroke ? 6 : 3) : (stroke ? 9 : 6));
      const int color = stroke ? 5 : 2;
      const QBrush brush = pattern ? QBrush(layer(id(color)).image) :
          QBrush(QColor(i(color, 0, 255), i(color + 1, 0, 255), i(color + 2, 0, 255), i(color + 3, 0, 255)));
      auto &target = layer(id(1));
      if (!target.image.isNull()) {
        QPainter p(&target.image);
        configure(p, target, i(0, 0, 15));
        if (stroke) {
          static const Qt::PenCapStyle caps[] = {Qt::FlatCap, Qt::RoundCap, Qt::SquareCap};
          static const Qt::PenJoinStyle joins[] = {Qt::BevelJoin, Qt::MiterJoin, Qt::RoundJoin};
          p.strokePath(target.path, QPen(brush, i(4, 1, dimLimit), Qt::SolidLine,
                                         caps[i(2, 0, 2)], joins[i(3, 0, 2)]));
        } else p.fillPath(target.path, brush);
      }
      target.pathClosed = true;
    } else if (op == "push" || op == "pop" || op == "reset" || op == "identity" || op == "transform") {
      need(op == "transform" ? 7 : 1);
      auto &target = layer(id(0));
      if (op == "reset") {
        target.graphics = {};
        target.stack.clear();
        target.path = {};
        target.pathClosed = false;
      } else if (op == "push") {
        if (target.stack.size() >= 32) tooLarge();
        target.stack.append(target.graphics);
      } else if (op == "pop") {
        if (!target.stack.isEmpty()) target.graphics = target.stack.takeLast();
      } else if (op == "identity") target.graphics.transform = {};
      else target.graphics.transform = matrix(1) * target.graphics.transform;
    } else {
      static const QSet<QByteArray> ignored{ "ack", "args", "audio", "body", "clipboard", "connect",
        "disconnect", "file", "filesystem", "get", "key", "log", "msg", "name", "nop", "pipe",
        "put", "ready", "required", "set", "sync", "touch", "undefine" };
      if (!ignored.contains(op))
        throw Error{tr("Unsupported RDP/VNC recording instruction: %1").arg(QString::fromLatin1(op))};
    }
  }

  void paintTree(QPainter &p, int id, int depth) const {
    const auto it = layers.constFind(id);
    if (it == layers.cend()) return;
    if (depth > 32) tooLarge();
    const Layer &current = it.value();
    p.save();
    p.translate(current.position);
    p.setTransform(current.presentation, true);
    p.setOpacity(p.opacity() * current.alpha / 255.0);
    p.drawImage(0, 0, current.image);
    QVector<int> children;
    for (auto i = layers.cbegin(); i != layers.cend(); ++i)
      if (i.key() > 0 && i.value().parent == id) children.append(i.key());
    std::sort(children.begin(), children.end(), [&](int a, int b) {
      const Layer &left = layers.constFind(a).value(), &right = layers.constFind(b).value();
      return left.z != right.z ? left.z < right.z : left.order < right.order;
    });
    for (int child : children) paintTree(p, child, depth + 1);
    p.restore();
  }

  QImage frame() const {
    const auto main = layers.constFind(0);
    if (main == layers.cend() || main->image.isNull()) return {};
    QImage result(main->image.size(), QImage::Format_RGB32);
    if (result.isNull()) tooLarge();
    result.fill(Qt::black);
    QPainter p(&result);
    paintTree(p, 0, 0);
    if (cursorVisible && !cursor.isNull()) p.drawImage(mouse - hotspot, cursor);
    return result;
  }
};
} // namespace

struct Guacamole::Private {
  explicit Private(SourcePtr s) : source(std::move(s)) {}
  struct Checkpoint { int next; int ms; qint64 bytes; Display display; };
  SourcePtr source;
  Display display;
  QVector<Checkpoint> checkpoints;
  qint64 checkpointBytes = 0;
  int frame = 0;
  int lastCheckpoint = -30000;
  int imageFrame = -1;
  QImage image;

  void addCheckpoint(int at, int ms) {
    const qint64 bytes = display.estimatedBytes();
    if (bytes > checkpointBudget) return;
    while (!checkpoints.isEmpty() &&
           (checkpoints.size() >= checkpointLimit || checkpointBytes + bytes > checkpointBudget)) {
      checkpointBytes -= checkpoints.first().bytes;
      checkpoints.removeFirst();
    }
    checkpoints.append({at, ms, bytes, display});
    checkpointBytes += bytes;
  }
};

Guacamole::Guacamole(SourcePtr source) : d(std::make_unique<Private>(std::move(source))) {}
Guacamole::~Guacamole() = default;

QImage Guacamole::at(int ms, const Canceled &canceled) {
  const auto &frames = d->source->frames;
  const int end = int(std::upper_bound(frames.cbegin(), frames.cend(), ms,
      [](int t, const Frame &f) { return t < f.ms; }) - frames.cbegin());
  if (end == d->imageFrame) return d->image;
  if (end < d->frame) {
    d->display = {};
    d->frame = 0;
    d->lastCheckpoint = frames.isEmpty() ? -30000 : frames.first().ms - 30000;
    for (auto i = d->checkpoints.crbegin(); i != d->checkpoints.crend(); ++i) {
      if (i->next <= end) {
        d->display = i->display;
        d->frame = i->next;
        d->lastCheckpoint = i->ms;
        break;
      }
    }
  }
  Instruction instruction;
  int instructions = 0;
  // The player can render on successive worker threads. Keep only plain
  // display state between calls, and open the local reader on this worker.
  QFile file(d->source->path);
  std::unique_ptr<InstructionReader> reader;
  if (d->frame < end) {
    if (!file.open(QIODevice::ReadOnly)) throw Error{file.errorString()};
    reader = std::make_unique<InstructionReader>(file, d->frame ? frames.at(d->frame - 1).end : 0);
  }
  while (d->frame < end) {
    checkCanceled(canceled);
    const auto &frame = frames.at(d->frame);
    while (reader->offset() < frame.end) {
      if (!(++instructions % 128)) checkCanceled(canceled);
      if (!reader->next(instruction) || reader->offset() > frame.end) invalid();
      d->display.draw(instruction);
    }
    ++d->frame;
    if (d->display.streams.isEmpty() && d->display.surfaceBytes <= 16 * mib &&
        frame.ms - d->lastCheckpoint >= 30000) {
      d->addCheckpoint(d->frame, frame.ms);
      d->lastCheckpoint = frame.ms;
    }
  }
  d->image = d->display.frame();
  d->imageFrame = end;
  return d->image;
}

} // namespace Replay
