#pragma once

#include <QWidget>
#include <memory>

namespace Replay {

class PluginSettings : public QWidget {
public:
  explicit PluginSettings(QWidget *parent = nullptr);
  ~PluginSettings() override;
  void retranslate();

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace Replay
