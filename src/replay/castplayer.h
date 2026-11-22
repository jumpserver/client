#pragma once

#include "replaysource.h"
#include <QImage>

namespace Replay {

class CastPlayer {
public:
  CastPlayer(const QString &path, const Canceled &canceled);
  ~CastPlayer();
  int duration() const;
  QImage at(int ms, const Canceled &canceled);

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace Replay
