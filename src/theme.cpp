#include "theme.h"
#include "native_theme.h"
#include <QApplication>
#include <QFontDatabase>
#include <QHash>
#include <QIconEngine>
#include <QLocale>
#include <QListView>
#include <QPainter>
#include <QFontMetrics>
#include <QScreen>
#include <QSettings>
#include <QResizeEvent>
#include <QStyle>
#include <QSvgRenderer>
#include <QStyledItemDelegate>
#include <QTimer>

QRect ClientTheme::dialogAvailableGeometry(const QWidget *dialog, bool initialPlacement) {
  const QWidget *owner = dialog->parentWidget() ? dialog->parentWidget()->window() : nullptr;
  QScreen *screen = initialPlacement && owner ? owner->screen() : dialog->screen();
  return screen->availableGeometry();
}

void ClientTheme::positionDialog(QWidget *dialog, bool initialPlacement) {
  const QWidget *owner = dialog->parentWidget() ? dialog->parentWidget()->window() : nullptr;
  const QRect available = dialogAvailableGeometry(dialog, initialPlacement);
  QRect frame = dialog->frameGeometry();
  if (initialPlacement)
    frame.moveCenter((owner ? owner->frameGeometry() : available).center() - QPoint(0, 24));
  const int left = qBound(available.left() + 16, frame.left(),
                         qMax(available.left() + 16, available.right() - frame.width() - 15));
  const int top = qBound(available.top() + 16, frame.top(),
                        qMax(available.top() + 16, available.bottom() - frame.height() - 15));
  if (QPoint(left, top) != dialog->frameGeometry().topLeft()) dialog->move(left, top);
}

namespace {
class SystemPaletteObserver : public QObject {
public:
  explicit SystemPaletteObserver(QObject *parent) : QObject(parent), systemPalette(qApp->palette()) {}
  QPalette systemPalette;
  QPalette appliedPalette;
  bool applying = false;
  bool queued = false;
protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::Show) {
      auto *widget = qobject_cast<QWidget *>(watched);
      if (widget && widget->isWindow())
        applyNativeWindowTheme(widget, QSettings().value("appearance/theme", "system").toString(),
                               ClientTheme::colors().background.lightness() < 128);
    }
    if (event->type() == QEvent::ApplicationPaletteChange && !applying &&
        qApp->palette() != appliedPalette) {
      systemPalette = qApp->palette();
      if (!queued) {
        queued = true;
        QTimer::singleShot(0, this, [this] {
          queued = false;
          ClientTheme::apply();
        });
      }
    }
    return false;
  }
};
SystemPaletteObserver *paletteObserver() {
  static SystemPaletteObserver *observer = nullptr;
  if (!observer) {
    observer = new SystemPaletteObserver(qApp);
    qApp->installEventFilter(observer);
  }
  return observer;
}
class ComboItemDelegate : public QStyledItemDelegate {
public:
  explicit ComboItemDelegate(QComboBox *combo) : QStyledItemDelegate(combo), m_combo(combo) {}
  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    const QSize base = QStyledItemDelegate::sizeHint(option, index);
    return {base.width() + 44, qMax(34, option.fontMetrics.height() + 16)};
  }
  void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QStyleOptionViewItem item(option);
    initStyleOption(&item, index);
    const auto c = ClientTheme::colors();
    const bool enabled = item.state & QStyle::State_Enabled;
    const bool hovered = item.state & QStyle::State_Selected;
    const bool selected = index.row() == m_combo->currentIndex();
    const QRect row = item.rect.adjusted(3, 1, -3, -1);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    if (hovered || selected) {
      p->setBrush(selected ? c.softAccent : c.hover);
      p->drawRoundedRect(row, 6, 6);
    }
    QRect label = row.adjusted(10, 0, -30, 0);
    if (!item.icon.isNull()) {
      item.icon.paint(p, QRect(label.left(), row.center().y() - 8, 16, 16));
      label.setLeft(label.left() + 24);
    }
    p->setFont(item.font);
    p->setPen(!enabled ? c.muted : selected ? c.accent : c.text);
    p->drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(item.font).elidedText(item.text, Qt::ElideRight, label.width()));
    if (selected) {
      const QPoint center(row.right() - 15, row.center().y());
      p->setPen(QPen(enabled ? c.accent : c.muted, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p->drawLine(center + QPoint(-5, 0), center + QPoint(-1, 4));
      p->drawLine(center + QPoint(-1, 4), center + QPoint(6, -4));
    }
    p->restore();
  }
private:
  QComboBox *m_combo;
};
QColor blend(const QColor &a, const QColor &b, qreal amount) {
  return QColor::fromRgbF(a.redF() * (1-amount) + b.redF() * amount,
                         a.greenF() * (1-amount) + b.greenF() * amount,
                         a.blueF() * (1-amount) + b.blueF() * amount);
}
ClientTheme::Colors paletteColors(bool dark) {
  const QSettings settings;
  ClientTheme::Colors c;
  c.background = dark ? "#242426" : "#fafafa";
  c.panel = dark ? "#2c2c2f" : "#ffffff";
  c.sidebar = dark ? "#1e1e20" : "#f0f1f2";
  c.text = dark ? "#ededf0" : "#30343b";
  c.muted = dark ? "#999ba4" : "#848891";
  c.border = dark ? "#3c3c41" : "#e4e6ea";
  c.hover = dark ? "#333338" : "#e8e9ec";
  c.selected = dark ? "#3b3b42" : "#dddfe3";
  c.accent = QColor(settings.value(dark ? "appearance/accentDark" : "appearance/accentLight",
                                   dark ? "#34d399" : "#1ab394").toString());
  if (!c.accent.isValid()) c.accent = QColor("#1ab394");
  c.softAccent = blend(c.panel, c.accent, dark ? 0.14 : 0.09);
  return c;
}

// Scalable line icons follow the current palette without a platform icon font.
class LineIconEngine : public QIconEngine {
public:
  explicit LineIconEngine(QString name) : m_name(std::move(name)) {}
  QIconEngine *clone() const override { return new LineIconEngine(m_name); }
  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
    static const QHash<QString, QString> shapes{
      {"terminal", "<rect x='3' y='4' width='18' height='16' rx='3'/><path d='m7 9 3 3-3 3m6 0h4'/>"},
      {"windows", "<path d='m3 5 18-2v18L3 19Zm0 7h18M11 4v16'/>"},
      {"database", "<ellipse cx='12' cy='5' rx='8' ry='3'/><path d='M4 5v14c0 4 16 4 16 0V5M4 12c0 4 16 4 16 0'/>"},
      {"network", "<rect x='3' y='3' width='12' height='15' rx='2'/><rect x='11' y='8' width='10' height='13' rx='2'/>"},
      {"browser", "<rect x='3' y='4' width='18' height='16' rx='3'/><path d='M3 9h18M7 6.5h.01M10 6.5h.01'/>"},
      {"box", "<path d='m12 3 9 5v9l-9 5-9-5V8Zm0 9 9-4M3 8l9 4v10M7.5 5.5l9 5'/>"},
      {"star", "<path d='m12 3 2.8 5.7 6.2.9-4.5 4.4 1.1 6.2-5.6-3-5.6 3 1.1-6.2L3 9.6l6.2-.9Z'/>"},
      {"search", "<circle cx='10.5' cy='10.5' r='6.5'/><path d='m16 16 4 4'/>"},
      {"sidebar", "<rect x='3' y='4' width='18' height='16' rx='3'/><path d='M9 4v16M6 8h.01M6 11h.01'/>"},
      {"refresh", "<path d='M20 7v5h-5M4 17v-5h5M5.6 7a7 7 0 0 1 12.2-1L20 9M4 15l2.2 3a7 7 0 0 0 12.2-1'/>"},
      {"sort", "<path d='M8 4v16m-4-4 4 4 4-4M14 5h7M14 10h5M14 15h3'/>"},
      {"grid", "<rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/><rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/>"},
      {"list", "<path d='M9 5h12M9 12h12M9 19h12M3 5h1M3 12h1M3 19h1'/>"},
      {"settings", "<path d='m9 3-.5 3-2 1-2.8-1L2 9l2.3 2v2L2 15l1.7 3 2.8-1 2 1 .5 3h6l.5-3 2-1 2.8 1 1.7-3-2.3-2v-2L22 9l-1.7-3-2.8 1-2-1L15 3Z'/><circle cx='12' cy='12' r='3'/>"},
      {"user", "<circle cx='12' cy='8' r='4'/><path d='M5 21v-2a7 7 0 0 1 14 0v2'/>"},
      {"logout", "<path d='M9 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h4M10 12h11m-4-4 4 4-4 4'/>"},
      {"chevron-left", "<path d='m14 6-6 6 6 6'/>"},
      {"chevron-right", "<path d='m10 6 6 6-6 6'/>"},
      {"chevron-down", "<path d='m6 9 6 6 6-6'/>"},
      {"arrow-right", "<path d='M4 12h16m-6-6 6 6-6 6'/>"},
      {"globe", "<circle cx='12' cy='12' r='9'/><ellipse cx='12' cy='12' rx='4' ry='9'/><path d='M3 12h18'/>"},
      {"lock", "<rect x='5' y='10' width='14' height='11' rx='2'/><path d='M8 10V7a4 4 0 0 1 8 0v3M12 15v2'/>"},
      {"check", "<path d='m5 12 4 4L19 6'/>"},
      {"close", "<path d='m6 6 12 12M6 18 18 6'/>"},
      {"desktop", "<rect x='3' y='3' width='18' height='14' rx='2'/><path d='M8 21h8M12 17v4'/>"},
      {"palette", "<path d='M12 3a9 9 0 1 0 0 18h1a2 2 0 0 0 1-3.7 2 2 0 0 1 1-3.7h3a3 3 0 0 0 3-3A9 9 0 0 0 12 3Z'/><path d='M7 10h.01M10 6.5h.01M15 7h.01M6 15h.01'/>"},
      {"app", "<rect x='3' y='3' width='7' height='7' rx='2'/><rect x='14' y='3' width='7' height='7' rx='2'/><rect x='3' y='14' width='7' height='7' rx='2'/><path d='M14 17.5h7M17.5 14v7'/>"},
      {"info", "<circle cx='12' cy='12' r='9'/><path d='M12 11v6M12 7h.01'/>"},
      {"folder", "<path d='M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z'/>"},
      {"download", "<path d='M12 4v11m-4-4 4 4 4-4M4 19h16'/>"},
      {"copy", "<rect x='9' y='9' width='11' height='11' rx='2'/><path d='M15 5a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2'/>"}
    };
    const auto c = ClientTheme::colors();
    const bool filledStar = m_name == "star-filled";
    const bool outlineStar = m_name == "star-outline";
    const QColor color = mode == QIcon::Disabled ? c.muted
        : filledStar ? c.accent : outlineStar && mode != QIcon::Active ? c.muted : c.text;
    const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='%3' stroke='%1' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
        .arg(color.name(), shapes.value(filledStar || outlineStar ? "star" : m_name, shapes.value("app")),
             filledStar ? color.name() : QStringLiteral("none"));
    QSvgRenderer renderer(svg.toUtf8());
    renderer.render(painter, rect);
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
    QPixmap result(size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    paint(&painter, QRect(QPoint(), size), mode, state);
    return result;
  }
private:
  QString m_name;
};
} // namespace

namespace ClientTheme {
ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
  auto *list = new QListView(this);
  list->setObjectName("comboOptions");
  list->setFrameShape(QFrame::NoFrame);
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list->setMouseTracking(true);
  setView(list);
  setItemDelegate(new ComboItemDelegate(this));
  setMaxVisibleItems(10);
  setMinimumHeight(36);
}
void ComboBox::showPopup() {
  // Measure the current model each time: site/organization lists change at runtime.
  int popupWidth = width();
  QStyleOptionViewItem option;
  option.initFrom(view());
  option.font = font();
  option.fontMetrics = QFontMetrics(font());
  for (int row = 0; row < count(); ++row)
    popupWidth = qMax(popupWidth, itemDelegate()->sizeHint(option, model()->index(row, modelColumn(), rootModelIndex())).width() + 16);
  const int available = screen() ? screen()->availableGeometry().width() - 24 : popupWidth;
  view()->setMinimumWidth(qMin(popupWidth, qMax(1, available)));
  QComboBox::showPopup();
}
Switch::Switch(QWidget *parent) : QAbstractButton(parent) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
}
QSize Switch::sizeHint() const { return {40, 22}; }
void Switch::paintEvent(QPaintEvent *) {
  const auto c = colors();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled())
    p.setOpacity(0.5);
  const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
  const qreal radius = track.height() / 2;
  p.setPen(isChecked() ? Qt::NoPen : QPen(c.border, 1));
  p.setBrush(isChecked() ? c.accent : c.hover);
  p.drawRoundedRect(track, radius, radius);
  const qreal diameter = track.height() - 6;
  const qreal left = isChecked() ? track.right() - diameter - 3 : track.left() + 3;
  const QRectF knob(left, track.top() + 3, diameter, diameter);
  p.setPen(Qt::NoPen);
  p.setBrush(isChecked() ? Qt::white : c.panel);
  p.drawEllipse(knob);
  const QColor glyph = isChecked() ? c.accent : c.muted;
  const QPointF center = knob.center();
  const qreal r = diameter / 2 - 2;
  QPen glyphPen(glyph, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(glyphPen);
  if (isChecked()) {
    p.drawLine(center + QPointF(-r, 0), center + QPointF(-r / 3, r * 0.7));
    p.drawLine(center + QPointF(-r / 3, r * 0.7), center + QPointF(r, -r * 0.7));
  } else {
    p.drawLine(center + QPointF(-r * 0.7, -r * 0.7), center + QPointF(r * 0.7, r * 0.7));
    p.drawLine(center + QPointF(r * 0.7, -r * 0.7), center + QPointF(-r * 0.7, r * 0.7));
  }
}
ElidedLabel::ElidedLabel(const QString &label, QWidget *parent)
    : QLabel(label, parent), m_fullText(label) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  setTextInteractionFlags(Qt::TextSelectableByMouse);
}
void ElidedLabel::setText(const QString &label) {
  m_fullText = label;
  refresh();
}
QSize ElidedLabel::minimumSizeHint() const {
  return {QFontMetrics(font()).horizontalAdvance(QStringLiteral("…")), QLabel::sizeHint().height()};
}
void ElidedLabel::resizeEvent(QResizeEvent *event) {
  QLabel::resizeEvent(event);
  refresh();
}
void ElidedLabel::refresh() {
  // Before the first layout pass there is no width to elide into, and the full
  // text is what size hints and tests need.
  if (width() <= 0) {
    QLabel::setText(m_fullText);
    return;
  }
  QLabel::setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight,
                                           contentsRect().adjusted(1, 0, -1, 0).width()));
}
QString text(const char *zh, const char *en) {
  const QString language = QSettings().value("appearance/language", "system").toString();
  const bool chinese = language == "zh" || (language == "system" && QLocale::system().language() == QLocale::Chinese);
  return QString::fromUtf8(chinese ? zh : en);
}
Colors colors() { return paletteColors(qApp->palette().color(QPalette::Window).lightness() < 128); }
QIcon icon(const QString &name) { return QIcon(new LineIconEngine(name)); }
QIcon applicationIcon() {
#ifdef Q_OS_MACOS
  return QIcon(":/app-icon-macos.png");
#else
  return QIcon(":/icons/icon.png");
#endif
}
void apply() {
  auto *observer = paletteObserver();
  if (observer->applying) return;
  observer->applying = true;
  const QSettings settings;
  const auto mode = settings.value("appearance/theme", "system").toString();
  QPalette palette = observer->systemPalette;
  const bool dark = mode == "dark" || (mode == "system" && palette.color(QPalette::Window).lightness() < 128);
  const auto c = paletteColors(dark);
  palette.setColor(QPalette::Window, c.background);
  palette.setColor(QPalette::WindowText, c.text);
  palette.setColor(QPalette::Base, c.panel);
  palette.setColor(QPalette::AlternateBase, c.background);
  palette.setColor(QPalette::Text, c.text);
  palette.setColor(QPalette::Button, c.panel);
  palette.setColor(QPalette::ButtonText, c.text);
  palette.setColor(QPalette::Highlight, c.accent);
  palette.setColor(QPalette::HighlightedText, Qt::white);
  palette.setColor(QPalette::Mid, c.border);
  palette.setColor(QPalette::Disabled, QPalette::Text, c.muted);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
  observer->appliedPalette = palette;
  qApp->setPalette(palette);
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  const QString family = settings.value("appearance/font").toString();
  if (!family.isEmpty()) font.setFamily(family);
  font.setPointSize(settings.value("appearance/fontSize", 11).toInt());
  qApp->setFont(font);
  QString sheet = QStringLiteral(R"CSS(
QMainWindow, QDialog { background: @background@; color: @text@; }
QWidget { selection-background-color: @accent@; selection-color: white; }
QLabel { color: @text@; background: transparent; }
QLabel[muted=true], QLabel[role=caption] { color: @muted@; }
QLabel[role=caption] { font-size: 12px; }
QLabel[role=dialogTitle] { font-size: 19px; font-weight: 600; }
QLabel[role=sectionTitle] { font-size: 14px; font-weight: 600; }
QLabel[role=validation] { color: @muted@; padding: 2px 0; }
QLabel#brandName { font-size: 14px; font-weight: 600; }
QLabel#welcomeTitle { font-size: 22px; font-weight: 600; }
QFrame#sidebar, QFrame#settingsSidebar { background: @sidebar@; border: none; border-right: 1px solid @border@; }
QFrame#topbar { background: @panel@; border: none; border-bottom: 1px solid @border@; }
QFrame[surface=true] { background: @panel@; border: 1px solid @border@; border-radius: 12px; }
QFrame#dialogFooter { border: none; border-top: 1px solid @border@; background: @background@; }
QFrame#profilePanel { border: none; border-top: 1px solid @border@; background: transparent; }
QLabel#avatar { background: @selected@; border-radius: 16px; }
QLineEdit, QComboBox, QSpinBox, QFontComboBox { color: @text@; background: @panel@; border: 1px solid @border@; border-radius: 7px; padding: 7px 10px; min-height: 18px; }
QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border-color: @muted@; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: @accent@; }
QLineEdit:disabled, QComboBox:disabled { color: @muted@; background: @background@; }
QLineEdit#assetSearch { background: @searchBackground@; border: 1px solid @border@; border-radius: 8px; padding: 0 8px 0 0; min-height: 36px; }
QLineEdit#assetSearch:hover { border-color: @muted@; }
QLineEdit#assetSearch:focus { background: @panel@; border-color: @accent@; }
QLineEdit#assetSearch:disabled { background: @sidebar@; border-color: @border@; color: @muted@; }
QLineEdit#assetSearch QToolButton { background: transparent; border: none; padding: 0; min-width: 0; min-height: 0; }
QComboBox { padding-right: 32px; combobox-popup: 0; }
QComboBox:on { border-color: @accent@; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; border: none; width: 30px; }
QComboBox::down-arrow { image: url(:/ui/chevron-down.svg); width: 14px; height: 14px; }
QComboBox::down-arrow:on { image: url(:/ui/chevron-up.svg); }
QComboBox QAbstractItemView { background: @panel@; color: @text@; border: none; border-radius: 8px; padding: 4px; outline: 0; selection-background-color: @softAccent@; selection-color: @text@; }
QComboBox QLineEdit { border: none; background: transparent; padding: 0; }
QComboBox#organizationSelector { background: transparent; border: none; font-weight: 500; }
QSpinBox::up-button, QSpinBox::down-button { border: none; background: transparent; width: 20px; }
QSpinBox::up-arrow { image: url(:/ui/chevron-up.svg); width: 10px; height: 10px; }
QSpinBox::down-arrow { image: url(:/ui/chevron-down.svg); width: 10px; height: 10px; }
QPushButton, QToolButton { background: @panel@; color: @text@; border: 1px solid @border@; border-radius: 7px; padding: 6px 12px; min-height: 18px; }
QPushButton:hover, QToolButton:hover { background: @hover@; }
QPushButton:pressed, QToolButton:pressed { background: @selected@; }
QPushButton:focus, QToolButton:focus { border-color: @accent@; }
QPushButton:disabled, QToolButton:disabled { color: @muted@; border-color: @border@; }
QPushButton[primary=true] { background: @accent@; color: white; border: 1px solid @accent@; font-weight: 600; }
QPushButton[primary=true]:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton[primary=true]:disabled { background: @selected@; color: @muted@; border-color: transparent; }
QPushButton[quiet=true], QToolButton[quiet=true] { background: transparent; border: 1px solid transparent; }
QPushButton[quiet=true]:hover, QToolButton[quiet=true]:hover { background: @hover@; }
QPushButton[soft=true], QPushButton[selected=true] { color: @accent@; background: @softAccent@; border: 1px solid transparent; font-weight: 600; }
QPushButton[soft=true]:hover { background: @softHover@; }
QPushButton[soft=true]:disabled { color: @muted@; background: @background@; }
QPushButton[iconOnly=true] { padding: 6px; min-width: 18px; min-height: 18px; }
QFrame#settingsProtocolColumn { background: transparent; border: none; border-right: 1px solid @border@; }
QLabel[role=groupLabel] { color: @muted@; font-size: 11px; font-weight: 600; padding: 4px 8px; }
QPushButton[role=protocolRow] { background: transparent; border: 1px solid transparent; color: @text@; text-align: left; padding: 5px 10px; border-radius: 7px; min-height: 16px; }
QPushButton[role=protocolRow]:hover { background: @hover@; }
QPushButton[role=protocolRow]:checked { background: @selected@; color: @accent@; font-weight: 600; }
QFrame#appCard { background: @panel@; border: 1px solid @border@; border-radius: 10px; }
QFrame#appCard:hover { border-color: @muted@; }
QLabel[role=appIcon] { background: @background@; border: 1px solid @border@; border-radius: 8px; }
QLabel[role=appName] { font-weight: 600; }
QLabel[role=appBadge] { color: @accent@; background: @softAccent@; border-radius: 5px; font-weight: 600; }
QLabel[role=appPath] { color: @muted@; background: @background@; border: 1px solid @border@; border-radius: 6px; font-size: 12px; }
QToolButton#applicationCopyPath { background: transparent; border: none; border-radius: 5px; padding: 0; min-width: 0; min-height: 0; }
QToolButton#applicationCopyPath:hover { background: @hover@; }
QToolButton#applicationCopyPath:pressed { background: @selected@; }
QLabel[role=appComment] { color: @muted@; font-size: 12px; }
QListWidget#categories, QListWidget#settingsNavigation { background: transparent; border: none; outline: 0; }
QListWidget#categories::item, QListWidget#settingsNavigation::item { padding: 0 10px; margin: 2px 0; border-radius: 7px; color: @text@; }
QListWidget#categories::item:hover, QListWidget#settingsNavigation::item:hover { background: @hover@; }
QListWidget#categories::item:selected, QListWidget#settingsNavigation::item:selected { background: @selected@; color: @text@; }
QListWidget#assetGrid { background: transparent; border: none; outline: 0; padding: 0; }
QListWidget#assetGrid::item { padding: 0; background: transparent; border: none; }
QTableWidget { background: @panel@; color: @text@; border: 1px solid @border@; border-radius: 9px; gridline-color: @border@; outline: 0; }
QTableWidget::item { padding: 8px; border: none; border-bottom: 1px solid @border@; }
QTableWidget::item:selected { background: @softAccent@; color: @text@; }
QTableWidget#assetTable::item { padding: 0 12px; border-bottom: 1px solid @tableDivider@; }
QTableWidget#assetTable::item:hover { background: @hover@; }
QTableWidget#assetTable::item:selected { background: @softAccent@; }
QTableWidget#assetTable QPushButton { padding: 4px 10px; }
QTableWidget#assetTable QPushButton#assetFavorite { padding: 0; min-height: 0; min-width: 0; border-radius: 5px; }
QHeaderView { background: @background@; }
QHeaderView::section { color: @muted@; background: @background@; border: none; border-bottom: 1px solid @border@; padding: 10px 8px; font-size: 12px; }
QTableWidget#assetTable QHeaderView::section { padding: 0 12px; }
QTableCornerButton::section { background: @background@; border: none; }
QTabWidget#settingsPages::pane { border: none; background: @background@; }
QTabWidget#connectionMethodTabs::pane { border: 1px solid @border@; border-radius: 7px; background: @panel@; }
QWidget#connectionMethodPage { background: @panel@; border-radius: 7px; }
QTabWidget#connectionMethodTabs QTabBar { background: @background@; }
QTabWidget#connectionMethodTabs QTabBar::tab { color: @muted@; background: @background@; border: none; border-bottom: 2px solid @border@; padding: 10px 12px; }
QTabWidget#connectionMethodTabs QTabBar::tab:selected { color: @accent@; border-bottom-color: @accent@; }
QTabWidget#connectionMethodTabs QTabBar::tab:hover { background: @softAccent@; }
QWidget#settingsPage, QScrollArea#settingsScroll, QScrollArea#settingsScroll > QWidget > QWidget { background: @background@; border: none; }
QGroupBox { border: none; margin-top: 20px; padding-top: 10px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 0; padding: 0; }
QCheckBox { color: @text@; spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid @border@; border-radius: 4px; background: @panel@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; image: url(:/ui/check.svg); }
QCheckBox::indicator:hover { border-color: @accent@; }
QScrollBar:vertical { width: 10px; background: transparent; margin: 3px 0; }
QScrollBar:horizontal { height: 10px; background: transparent; margin: 0 3px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 3px; margin: 0 2px; min-height: 32px; }
QScrollBar::handle:horizontal { background: @scroll@; border-radius: 3px; margin: 2px 0; min-width: 32px; }
QScrollBar::handle:hover { background: @scrollHover@; }
QScrollBar::handle:pressed { background: @accent@; }
QScrollBar::handle:disabled { background: transparent; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: transparent; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: transparent; border: none; }
QMenu { color: @text@; background: @panel@; border: none; padding: 5px; }
QMenu::item { padding: 7px 24px 7px 14px; border-radius: 5px; }
QMenu::item:selected { background: @hover@; }
QMenu::right-arrow { image: url(:/ui/chevron-right.svg); width: 14px; height: 14px; }
QMenu::left-arrow { image: url(:/ui/chevron-left.svg); width: 14px; height: 14px; }
QMenu::separator { height: 1px; background: @border@; margin: 5px 8px; }
QToolTip { color: @text@; background: @panel@; border: 1px solid @border@; border-radius: 5px; padding: 2px 4px; }
QStatusBar { color: @muted@; background: @background@; border: none; font-size: 12px; }
QStatusBar::item { border: none; }
)CSS");
  const QList<QPair<QString, QColor>> values{
    {"background", c.background}, {"panel", c.panel}, {"sidebar", c.sidebar},
    {"text", c.text}, {"muted", c.muted}, {"border", c.border},
    {"hover", c.hover}, {"selected", c.selected}, {"accentHover", c.accent.darker(108)},
    {"tableDivider", blend(c.panel, c.border, 0.6)},
    {"searchBackground", blend(c.sidebar, c.panel, 0.65)},
    {"accent", c.accent}, {"softAccent", c.softAccent},
    {"softHover", blend(c.panel, c.accent, 0.17)},
    {"scroll", blend(c.panel, c.muted, 0.55)},
    {"scrollHover", blend(c.panel, c.muted, 0.85)}
  };
  for (const auto &value : values) sheet.replace("@" + value.first + "@", value.second.name());
  qApp->setStyleSheet(sheet);
  for (auto *window : QApplication::topLevelWidgets())
    applyNativeWindowTheme(window, mode, dark);
  observer->applying = false;
}
} // namespace ClientTheme
