#pragma once
#include <QJsonArray>
#include <QSize>

// `wl_output.mode` reports the current mode in physical pixels.  Keep this
// helper next to monitor enumeration so render producers never need to guess a
// size from the compositor's logical desktop coordinates.
QJsonArray listWaylandOutputs();
QSize physicalWaylandOutputSize(const QString &outputName);

// Session-aware inventory. The returned `name` is the compositor connector
// spelling and is also the key used for the frame bridge.
QString graphicalSessionType();
QJsonArray listSessionOutputs();
QSize physicalSessionOutputSize(const QString &outputName);
QString stableOutputIdentity(const QString &outputName);
