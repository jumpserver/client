#pragma once
#include <QColor>
#include <QComboBox>
#include <QIcon>
#include <QString>
namespace ClientTheme {
class ComboBox : public QComboBox {
public:
  explicit ComboBox(QWidget *parent = nullptr);
  void showPopup() override;
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
