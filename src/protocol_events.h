#pragma once

#include <QString>
#include <functional>

// Call after QApplication construction and before entering its event loop.
// JMS base64 is case-sensitive; never pass it through QUrl's host normalization.
// An empty handler removes the custom event receiver during application shutdown.
void installProtocolEventHandler(std::function<void(QString)> handler);
