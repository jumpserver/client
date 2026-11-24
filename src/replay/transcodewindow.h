#pragma once

#include <QDialog>
#include <QResizeEvent>
#include <functional>
#include <memory>

namespace Replay {

class TranscodeWindow : public QDialog {
public:
  TranscodeWindow(QWidget *parent, std::function<void()> openPluginSettings,
                  std::function<void(const QString &)> openOutput);
  ~TranscodeWindow() override;
  void retranslate();

protected:
  void resizeEvent(QResizeEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace Replay
