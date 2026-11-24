#pragma once

#include <QDialog>
#include <memory>

namespace Replay {

class Window : public QDialog {
public:
  explicit Window(QWidget *parent = nullptr);
  ~Window() override;
  void retranslate();
  void openFiles(const QStringList &paths);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void reject() override { close(); }

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace Replay
