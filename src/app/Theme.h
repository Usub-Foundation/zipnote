#pragma once

#include "NoteDocument.h"

#include <QColor>
#include <QFont>
#include <QObject>

class QApplication;

namespace zn {

// Design tokens: a warm yellow accent on neutral grays, light and dark variants.
struct ThemePalette {
    bool dark = false;
    QColor window;
    QColor sidebar;
    QColor list;
    QColor editor;
    QColor title;          // note titles, headings
    QColor text;           // body text
    QColor secondaryText;  // dates, snippets
    QColor tertiaryText;   // section headers, counts
    QColor separator;
    QColor accent;
    QColor selection;          // selected row when the list has focus
    QColor selectionInactive;  // selected row otherwise
    QColor hover;
    QColor searchHighlight;    // matched text in list cells and the editor
    QColor findCurrent;        // current match of Find in Note
    QColor field;              // search field background
    QColor chip;               // search tokens, tags
    QColor chipText;
    QColor codeBackground;
    QColor quoteBar;
    QColor link;
};

class Theme : public QObject {
    Q_OBJECT
public:
    static Theme& instance();
    static const ThemePalette& palette() { return instance().palette_; }
    static bool isDark() { return instance().palette_.dark; }

    void apply(QApplication& app);
    // Forces a scheme instead of following the system (command line --theme, platforms without a scheme).
    void setForcedScheme(Qt::ColorScheme scheme);
    TextStyles textStyles() const;
    QFont uiFont(qreal pointSize, QFont::Weight weight = QFont::Normal) const;

signals:
    void changed();

private:
    Theme() = default;
    void rebuild();
    QApplication* app_ = nullptr;
    ThemePalette palette_;
    Qt::ColorScheme forced_ = Qt::ColorScheme::Unknown;
};

}  // namespace zn
