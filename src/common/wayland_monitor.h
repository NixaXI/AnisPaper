#pragma once
#include <QJsonArray>
#include <QSize>

// `wl_output.mode` reports the current mode in physical pixels.  Keep this
// helper next to monitor enumeration so render producers never need to guess a
// size from the compositor's logical desktop coordinates.
QJsonArray listWaylandOutputs();
QSize physicalWaylandOutputSize(const QString &outputName);
