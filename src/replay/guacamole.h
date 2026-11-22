#pragma once

#include "replaysource.h"
#include <QImage>

namespace Replay {

// Used independently by the player and encoder so seeking doesn't affect a job.
class Guacamole {
public:
  explicit Guacamole(SourcePtr source);
  ~Guacamole();
  QImage at(int ms, const Canceled &canceled = {});

private:
  struct Private;
  std::unique_ptr<Private> d;
};

} // namespace Replay
