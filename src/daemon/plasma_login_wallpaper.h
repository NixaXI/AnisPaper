#pragma once

#include <QString>

// Uses Plasma Login Manager's official KAuth save action.  This is separate
// from plasma-login-wallpaper, which is a greeter preview executable rather
// than a noninteractive installer.
bool installPlasmaLoginWallpaper(const QString &pngPath, QString *error);

