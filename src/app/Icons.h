#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace zn::icons {

// Toolbar and sidebar line icons, drawn as SVG paths. Stroke color follows the theme.
QIcon icon(const QString& name, const QColor& color = {});
QPixmap pixmap(const QString& name, int size, const QColor& color, qreal dpr = 1.0);

}  // namespace zn::icons
