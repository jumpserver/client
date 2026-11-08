#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QString>
namespace ClientTheme {
class ComboBox : public QComboBox {
public:
  explicit ComboBox(QWidget *parent = nullptr);
  void showPopup() override;
};
// Toggle switch used by the local application settings page.
class Switch : public QAbstractButton {
public:
  explicit Switch(QWidget *parent = nullptr);
  QSize sizeHint() const override;
protected:
  void paintEvent(QPaintEvent *event) override;
};
// Label that keeps its full text available while eliding to the given width.
class ElidedLabel : public QLabel {
public:
  explicit ElidedLabel(const QString &text = QString(), QWidget *parent = nullptr);
  void setText(const QString &text);
  QSize minimumSizeHint() const override;
protected:
  void resizeEvent(QResizeEvent *event) override;
private:
  void refresh();
  QString m_fullText;
};
struct Colors {
  QColor background, panel, sidebar, text, muted, border, hover, selected,
      accent, softAccent;
};
QString text(const char *zh, const char *en);
Colors colors();
QIcon icon(const QString &name);
QIcon applicationIcon();
QRect dialogAvailableGeometry(const QWidget *dialog, bool initialPlacement);
void positionDialog(QWidget *dialog, bool initialPlacement);
void apply();
} // namespace ClientTheme
