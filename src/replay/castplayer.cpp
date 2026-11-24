#include "castplayer.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace Replay {
namespace {
struct Event { int ms; QString type, data; };
const QColor foreground(229, 231, 235), background(18, 20, 24);
[[noreturn]] void invalid() {
  throw Error{QCoreApplication::translate("OfflineReplay", "Invalid or unsupported asciinema recording.")};
}

QColor ansi(int n) {
  static const QRgb basic[] = {0xff171717, 0xffcc5555, 0xff62b76f, 0xffdbc66d, 0xff6096e3, 0xffbc78d2, 0xff58b6bd, 0xffd2d6dc,
                               0xff656974, 0xfff57a7a, 0xff8cdb99, 0xfff6e193, 0xff90bafb, 0xffe4a2f5, 0xff83e0e6, 0xfff5f5f5};
  if (n < 16) return QColor::fromRgb(basic[qBound(0, n, 15)]);
  if (n >= 232) { const int gray = 8 + (qMin(n, 255) - 232) * 10; return {gray, gray, gray}; }
  n -= 16;
  const auto rgb = [](int v) { return v ? 55 + v * 40 : 0; };
  return {rgb(n / 36), rgb(n / 6 % 6), rgb(n % 6)};
}

struct Cell {
  QString text;
  QColor fg = foreground, bg = background;
  bool bold = false, reverse = false, continuation = false;
};

int cellWidth(uint c) {
  if (QChar::combiningClass(c) || c == 0xfe0e || c == 0xfe0f || c == 0x200d) return 0;
  return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
         (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
         (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff60) ||
         (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd) ? 2 : 1;
}

class Terminal {
public:
  QSize size;
  QVector<Cell> cells, normal;
  QPoint cursor, saved, normalCursor;
  Cell style;
  QString arguments;
  enum State { Text, Escape, Csi, String, StringEscape, Charset } state = Text;
  bool cursorVisible = true, alternate = false;
  int top = 0, bottom = 0;

  void reset(QSize dimensions) {
    *this = Terminal();
    resize(dimensions);
  }

  void resize(const QSize &dimensions) {
    if (dimensions.width() < 1 || dimensions.width() > 320 || dimensions.height() < 1 || dimensions.height() > 160) invalid();
    auto resizeCells = [&](QVector<Cell> &grid) {
      if (grid.isEmpty() && grid.size() != size.width() * size.height()) return;
      QVector<Cell> copy(dimensions.width() * dimensions.height());
      for (int y = 0; y < qMin(size.height(), dimensions.height()); ++y)
        for (int x = 0; x < qMin(size.width(), dimensions.width()); ++x)
          copy[y * dimensions.width() + x] = grid.at(y * size.width() + x);
      grid.swap(copy);
    };
    if (cells.isEmpty()) cells.resize(dimensions.width() * dimensions.height());
    else resizeCells(cells);
    if (alternate) resizeCells(normal);
    size = dimensions;
    top = 0; bottom = size.height() - 1;
    setCursor(cursor.x(), cursor.y());
  }

  void accept(const QString &input, const Canceled &canceled) {
    for (int i = 0; i < input.size(); ++i) {
      if (i % 2048 == 0) checkCanceled(canceled);
      QChar c = input.at(i);
      if (state == String || state == StringEscape) {
        if (c == QChar(7) || (state == StringEscape && c == '\\')) state = Text;
        else state = c == QChar(27) ? StringEscape : String;
        continue;
      }
      if (c == QChar(27)) { state = Escape; continue; }
      if (state == Charset) { state = Text; continue; }
      if (state == Csi) {
        if (c >= '@' && c <= '~') { command(c, arguments); state = Text; }
        else if (arguments.size() < 128) arguments.append(c);
        continue;
      }
      if (state == Escape) {
        state = Text;
        if (c == '[') { arguments.clear(); state = Csi; }
        else if (c == ']' || c == 'P' || c == '^' || c == '_' || c == 'X') state = String;
        else if (c == '(' || c == ')' || c == '*' || c == '+') state = Charset;
        else if (c == '7') saved = cursor;
        else if (c == '8') setCursor(saved.x(), saved.y());
        else if (c == 'D' || c == 'E') { if (c == 'E') cursor.setX(0); lineFeed(); }
        else if (c == 'M') { if (cursor.y() == top) scroll(top, bottom, -1); else setCursor(cursor.x(), cursor.y() - 1); }
        else if (c == 'c') reset(size);
        continue;
      }
      if (c == '\r') cursor.setX(0);
      else if (c == '\n' || c == QChar(11) || c == QChar(12)) lineFeed();
      else if (c == '\b') cursor.setX(qMax(0, cursor.x() - 1));
      else if (c == '\t') cursor.setX(qMin(size.width() - 1, (cursor.x() / 8 + 1) * 8));
      else if (c.unicode() >= 32 && c != QChar(127)) {
        QString text(c);
        uint cp = c.unicode();
        if (c.isHighSurrogate() && i + 1 < input.size() && input.at(i + 1).isLowSurrogate()) {
          const QChar low = input.at(++i);
          cp = QChar::surrogateToUcs4(c, low);
          text.append(low);
        }
        put(text, cellWidth(cp));
      }
    }
  }

  QImage paint() const {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(16);
    const QFontMetrics fm(font);
    const int width = fm.horizontalAdvance(QLatin1Char('M')), height = fm.height() + 2;
    QImage image(size.width() * width + 20, size.height() * height + 16, QImage::Format_RGB32);
    if (image.isNull()) invalid();
    image.fill(background);
    QPainter painter(&image);
    for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
      const Cell &cell = cells.at(y * size.width() + x);
      const QRect rect(10 + x * width, 8 + y * height, width, height);
      const QColor bg = cell.reverse ? cell.fg : cell.bg;
      if (bg != background) painter.fillRect(rect, bg);
    }
    for (int y = 0; y < size.height(); ++y) for (int x = 0; x < size.width(); ++x) {
      const Cell &cell = cells.at(y * size.width() + x);
      if (cell.continuation || cell.text.isEmpty()) continue;
      font.setBold(cell.bold);
      painter.setFont(font);
      painter.setPen(cell.reverse ? cell.bg : cell.fg);
      painter.drawText(QPoint(10 + x * width, 8 + y * height + fm.ascent()), cell.text);
    }
    if (cursorVisible && cursor.x() < size.width())
      painter.fillRect(10 + cursor.x() * width, 8 + cursor.y() * height + height - 2, width, 2, foreground);
    return image;
  }

private:
  void put(const QString &text, int width) {
    if (!width) {
      if (cursor.x() > 0) {
        int offset = cursor.y() * size.width() + cursor.x() - 1;
        if (cells.at(offset).continuation && cursor.x() >= 2) --offset;
        if (cells[offset].text.size() < 16) cells[offset].text += text;
      }
      return;
    }
    width = qMin(width, size.width());
    if (cursor.x() + width > size.width()) { cursor.setX(0); lineFeed(); }
    const int at = cursor.y() * size.width() + cursor.x();
    if (cells.at(at).continuation && cursor.x() > 0) cells[at - 1] = Cell();
    if (cursor.x() + width < size.width() && cells.at(at + width).continuation) cells[at + width] = Cell();
    cells[at] = style; cells[at].text = text;
    if (width == 2) { cells[at + 1] = style; cells[at + 1].continuation = true; }
    cursor.rx() += width;
  }

  void setCursor(int x, int y) { cursor = {qBound(0, x, size.width() - 1), qBound(0, y, size.height() - 1)}; }
  void lineFeed() {
    if (cursor.y() == bottom) scroll(top, bottom, 1);
    else cursor.setY(qMin(size.height() - 1, cursor.y() + 1));
  }
  void clear(int first, int last) {
    Cell blank;
    blank.bg = style.bg;
    std::fill(cells.begin() + first, cells.begin() + last, blank);
  }
  void scroll(int start, int end, int n) {
    const int cols = size.width();
    n = qBound(-(end - start + 1), n, end - start + 1);
    auto base = cells.begin();
    if (n > 0) {
      std::move(base + (start + n) * cols, base + (end + 1) * cols, base + start * cols);
      clear((end + 1 - n) * cols, (end + 1) * cols);
    } else if (n < 0) {
      std::move_backward(base + start * cols, base + (end + 1 + n) * cols, base + (end + 1) * cols);
      clear(start * cols, (start - n) * cols);
    }
  }

  void sgr(const QVector<int> &args) {
    for (int i = 0; i < args.size(); ++i) {
      const int n = args.at(i);
      if (n == 0) style = {};
      else if (n == 1 || n == 22) style.bold = n == 1;
      else if (n == 7 || n == 27) style.reverse = n == 7;
      else if (n == 39) style.fg = foreground;
      else if (n == 49) style.bg = background;
      else if (n >= 30 && n <= 37) style.fg = ansi(n - 30);
      else if (n >= 40 && n <= 47) style.bg = ansi(n - 40);
      else if (n >= 90 && n <= 97) style.fg = ansi(n - 82);
      else if (n >= 100 && n <= 107) style.bg = ansi(n - 92);
      else if ((n == 38 || n == 48) && i + 2 < args.size()) {
        QColor &color = n == 38 ? style.fg : style.bg;
        if (args.at(i + 1) == 5) { color = ansi(args.at(i + 2)); i += 2; }
        else if (args.at(i + 1) == 2 && i + 4 < args.size()) {
          color = QColor(qBound(0, args.at(i + 2), 255), qBound(0, args.at(i + 3), 255), qBound(0, args.at(i + 4), 255));
          i += 4;
        }
      }
    }
  }

  void command(QChar op, QString raw) {
    const bool special = raw.startsWith('?');
    if (special) raw.remove(0, 1);
    QVector<int> a;
    for (const auto &v : raw.split(';')) a.append(qBound(0, v.toInt(), 10000));
    auto p = [&](int j, int fallback = 1) { return a.value(j) > 0 ? a.value(j) : fallback; };
    const int x = qMin(cursor.x(), size.width() - 1), y = cursor.y(), cols = size.width(), j = y * cols + x;
    if (special) {
      if (op == 'h' || op == 'l') for (int mode : a) {
        const bool enabled = op == 'h';
        if (mode == 25) cursorVisible = enabled;
        if (mode == 47 || mode == 1047 || mode == 1049) {
          if (enabled && !alternate) {
            normal = cells; normalCursor = cursor; cells = QVector<Cell>(cells.size()); cursor = {}; alternate = true;
            top = 0; bottom = size.height() - 1;
          } else if (!enabled && alternate) {
            cells.swap(normal); normal.clear(); setCursor(normalCursor.x(), normalCursor.y()); alternate = false;
            top = 0; bottom = size.height() - 1;
          }
        }
      }
      return;
    }
    switch (op.unicode()) {
      case 'A': setCursor(x, y - p(0)); break;
      case 'B': case 'e': setCursor(x, y + p(0)); break;
      case 'C': case 'a': setCursor(x + p(0), y); break;
      case 'D': setCursor(x - p(0), y); break;
      case 'E': setCursor(0, y + p(0)); break;
      case 'F': setCursor(0, y - p(0)); break;
      case 'G': case '`': setCursor(p(0) - 1, y); break;
      case 'd': setCursor(x, p(0) - 1); break;
      case 'H': case 'f': setCursor(p(1) - 1, p(0) - 1); break;
      case 's': saved = cursor; break;
      case 'u': setCursor(saved.x(), saved.y()); break;
      case 'J':
        if (a[0] == 0) clear(j, cells.size());
        else if (a[0] == 1) clear(0, j + 1);
        else if (a[0] == 2) clear(0, cells.size());
        break;
      case 'K':
        if (a[0] == 0) clear(j, (y + 1) * cols);
        else if (a[0] == 1) clear(y * cols, j + 1);
        else if (a[0] == 2) clear(y * cols, (y + 1) * cols);
        break;
      case '@': case 'P': case 'X': {
        const int n = qMin(p(0), cols - x), end = (y + 1) * cols;
        auto start = cells.begin();
        if (op == '@') { std::move_backward(start + j, start + end - n, start + end); clear(j, j + n); }
        else if (op == 'P') { std::move(start + j + n, start + end, start + j); clear(end - n, end); }
        else clear(j, j + n);
        break;
      }
      case 'L': case 'M': if (y >= top && y <= bottom) scroll(y, bottom, op == 'L' ? -p(0) : p(0)); break;
      case 'S': scroll(top, bottom, p(0)); break;
      case 'T': scroll(top, bottom, -p(0)); break;
      case 'r': {
        const int first = p(0) - 1, last = p(1, size.height()) - 1;
        if (first < last && last < size.height()) { top = first; bottom = last; setCursor(0, 0); }
        break;
      }
      case 'm': sgr(a); break;
    }
  }
};
} // namespace

struct CastPlayer::Private {
  QVector<Event> events;
  int next = 0;
  int imageAt = -1;
  QSize initial;
  Terminal terminal;
  QImage image;
};

CastPlayer::CastPlayer(const QString &path, const Canceled &canceled) : d(std::make_unique<Private>()) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) throw Error{file.errorString()};
  const auto header = QJsonDocument::fromJson(file.readLine(1024 * 1024)).object();
  const int version = header.value("version").toInt();
  if (version != 2 && version != 3) invalid();
  const auto term = version == 3 ? header.value("term").toObject() : header;
  d->initial = {term.value(version == 3 ? "cols" : "width").toInt(),
                term.value(version == 3 ? "rows" : "height").toInt()};
  d->terminal.reset(d->initial);
  double last = 0;
  while (!file.atEnd()) {
    checkCanceled(canceled);
    const auto line = file.readLine(1024 * 1024);
    if (line.trimmed().isEmpty()) continue;
    const auto event = QJsonDocument::fromJson(line).array();
    if (event.size() != 3 || !event.at(0).isDouble() || !event.at(1).isString() || !event.at(2).isString() ||
        d->events.size() >= 250000) invalid();
    const double seconds = event.at(0).toDouble() + (version == 3 ? last : 0);
    if (!std::isfinite(seconds) || seconds < last || seconds > 86400) invalid();
    d->events.append({int(std::round(seconds * 1000)), event.at(1).toString(), event.at(2).toString()});
    last = seconds;
  }
  if (file.error() != QFileDevice::NoError || d->events.isEmpty()) invalid();
}

CastPlayer::~CastPlayer() = default;
int CastPlayer::duration() const { return d->events.constLast().ms; }
QImage CastPlayer::at(int ms, const Canceled &canceled) {
  const int end = int(std::upper_bound(d->events.cbegin(), d->events.cend(), ms,
      [](int t, const Event &e) { return t < e.ms; }) - d->events.cbegin());
  if (end == d->imageAt) return d->image;
  if (end < d->next) { d->terminal.reset(d->initial); d->next = 0; }
  while (d->next < end) {
    checkCanceled(canceled);
    const auto &event = d->events.at(d->next++);
    if (event.type == "o") d->terminal.accept(event.data, canceled);
    else if (event.type == "r") {
      const auto dims = event.data.split('x');
      if (dims.size() != 2) invalid();
      d->terminal.resize({dims[0].toInt(), dims[1].toInt()});
    }
  }
  d->image = d->terminal.paint();
  d->imageAt = end;
  return d->image;
}

} // namespace Replay
