#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace zn {

namespace {

constexpr QColor kAccent(0xFC, 0xB8, 0x27);

ThemePalette lightPalette() {
    ThemePalette p;
    p.dark = false;
    p.window = QColor(0xFF, 0xFF, 0xFF);
    p.sidebar = QColor(0xEE, 0xEE, 0xEC);
    p.list = QColor(0xFF, 0xFF, 0xFF);
    p.editor = QColor(0xFF, 0xFF, 0xFF);
    p.title = QColor(0x1D, 0x1D, 0x1F);
    p.text = QColor(0x45, 0x45, 0x45);
    p.secondaryText = QColor(0x6E, 0x6E, 0x73);
    p.tertiaryText = QColor(0x8E, 0x8E, 0x93);
    p.separator = QColor(0xE3, 0xE3, 0xE1);
    p.accent = kAccent;
    p.selection = QColor(0xFE, 0xE3, 0x9A);
    p.selectionInactive = QColor(0xE6, 0xE6, 0xE4);
    p.sidebarSelectionInactive = QColor(0xFC, 0xB8, 0x27, 80);
    p.hover = QColor(0, 0, 0, 12);
    p.searchHighlight = QColor(0xFC, 0xB8, 0x27, 120);
    p.findCurrent = QColor(0xFC, 0xB8, 0x27, 230);
    p.field = QColor(0, 0, 0, 13);
    p.chip = QColor(0xFC, 0xB8, 0x27, 60);
    p.chipText = QColor(0x7A, 0x52, 0x00);
    p.codeBackground = QColor(0, 0, 0, 12);
    p.quoteBar = kAccent;
    p.link = QColor(0xB8, 0x74, 0x00);
    return p;
}

ThemePalette darkPalette() {
    ThemePalette p;
    p.dark = true;
    p.window = QColor(0x1E, 0x1E, 0x1E);
    p.sidebar = QColor(0x26, 0x26, 0x27);
    p.list = QColor(0x1E, 0x1E, 0x1E);
    p.editor = QColor(0x1E, 0x1E, 0x1E);
    p.title = QColor(0xF5, 0xF5, 0xF7);
    p.text = QColor(0xDC, 0xDC, 0xDC);
    p.secondaryText = QColor(0x98, 0x98, 0x9D);
    p.tertiaryText = QColor(0x7C, 0x7C, 0x80);
    p.separator = QColor(0x38, 0x38, 0x3A);
    p.accent = kAccent;
    p.selection = QColor(0x6B, 0x51, 0x14);
    p.selectionInactive = QColor(0x3A, 0x3A, 0x3C);
    p.sidebarSelectionInactive = QColor(0xFC, 0xB8, 0x27, 56);
    p.hover = QColor(255, 255, 255, 14);
    p.searchHighlight = QColor(0xFC, 0xB8, 0x27, 105);
    p.findCurrent = QColor(0xFC, 0xB8, 0x27, 210);
    p.field = QColor(255, 255, 255, 20);
    p.chip = QColor(0xFC, 0xB8, 0x27, 70);
    p.chipText = QColor(0xFF, 0xD7, 0x7A);
    p.codeBackground = QColor(255, 255, 255, 16);
    p.quoteBar = kAccent;
    p.link = QColor(0xFC, 0xC2, 0x4E);
    return p;
}

}  // namespace

Theme& Theme::instance() {
    static Theme t;
    return t;
}

void Theme::apply(QApplication& app) {
    app_ = &app;
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    rebuild();
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { rebuild(); });
}

void Theme::setForcedScheme(Qt::ColorScheme scheme) {
    forced_ = scheme;
    if (app_)
        rebuild();
}

void Theme::rebuild() {
    const Qt::ColorScheme scheme = forced_ != Qt::ColorScheme::Unknown ? forced_
                                   : app_ ? app_->styleHints()->colorScheme()
                                          : Qt::ColorScheme::Light;
    const bool dark = scheme == Qt::ColorScheme::Dark;
    palette_ = dark ? darkPalette() : lightPalette();

    QPalette pal;
    pal.setColor(QPalette::Window, palette_.window);
    pal.setColor(QPalette::WindowText, palette_.title);
    pal.setColor(QPalette::Base, palette_.editor);
    pal.setColor(QPalette::AlternateBase, palette_.sidebar);
    pal.setColor(QPalette::Text, palette_.text);
    pal.setColor(QPalette::PlaceholderText, palette_.tertiaryText);
    pal.setColor(QPalette::Button, dark ? QColor(0x3A, 0x3A, 0x3C) : QColor(0xF4, 0xF4, 0xF2));
    pal.setColor(QPalette::ButtonText, palette_.title);
    pal.setColor(QPalette::Highlight, palette_.accent);
    pal.setColor(QPalette::HighlightedText, QColor(0x1D, 0x1D, 0x1F));
    pal.setColor(QPalette::Link, palette_.link);
    pal.setColor(QPalette::ToolTipBase, palette_.window);
    pal.setColor(QPalette::ToolTipText, palette_.title);
    pal.setColor(QPalette::Mid, palette_.separator);
    pal.setColor(QPalette::Disabled, QPalette::Text, palette_.tertiaryText);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, palette_.tertiaryText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, palette_.tertiaryText);
    if (app_) {
        app_->setPalette(pal);
        app_->setFont(uiFont(QApplication::font().pointSizeF() > 0 ? QApplication::font().pointSizeF() : 10));
        const auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
        app_->setStyleSheet(QStringLiteral(R"(
            QToolTip { border: 1px solid %1; padding: 4px 6px; }
            QMenu { padding: 5px; border: 1px solid %1; }
            QMenu::item { padding: 4px 22px 4px 18px; border-radius: 5px; }
            QMenu::item:selected { background: %2; color: #1d1d1f; }
            QMenu::separator { height: 1px; background: %1; margin: 4px 8px; }
            QSplitter::handle { background: %1; }
            QScrollBar:vertical { width: 10px; background: transparent; margin: 2px; }
            QScrollBar::handle:vertical { background: %3; border-radius: 3px; min-height: 30px; }
            QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
            QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
            QScrollBar:horizontal { height: 10px; background: transparent; margin: 2px; }
            QScrollBar::handle:horizontal { background: %3; border-radius: 3px; min-width: 30px; }
            QPushButton { padding: 5px 14px; border-radius: 6px; border: 1px solid %1; background: %4; }
            QPushButton:hover { border-color: %3; }
            QPushButton:default, QPushButton[accent="true"] { background: %2; border-color: %2; color: #1d1d1f; font-weight: 600; }
            QPushButton:disabled { color: %5; }
            QLineEdit, QPlainTextEdit, QSpinBox, QComboBox { border: 1px solid %1; border-radius: 6px; padding: 4px 6px; background: %6; }
            QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid %2; }
            QCheckBox::indicator:checked { background: %2; border: 1px solid %2; border-radius: 3px; }
        )")
                                  .arg(c(palette_.separator), c(palette_.accent),
                                       c(dark ? QColor(255, 255, 255, 60) : QColor(0, 0, 0, 55)),
                                       c(pal.color(QPalette::Button)), c(palette_.tertiaryText), c(palette_.editor)));
    }
    emit changed();
}

QFont Theme::uiFont(qreal pointSize, QFont::Weight weight) const {
    QFont f = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    f.setPointSizeF(pointSize);
    f.setWeight(weight);
    return f;
}

TextStyles Theme::textStyles() const {
    TextStyles s;
    s.bodyFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    s.monoFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    const qreal base = std::max<qreal>(11.0, QApplication::font().pointSizeF() + 2.5);
    s.bodySize = base;
    s.titleSize = base * 1.95;
    s.headingSize = base * 1.45;
    s.subheadingSize = base * 1.18;
    s.monoSize = base * 0.92;
    s.text = palette_.text;
    s.codeBackground = palette_.codeBackground;
    s.highlight = palette_.dark ? QColor(0xFC, 0xB8, 0x27, 90) : QColor(0xFC, 0xB8, 0x27, 110);
    s.link = palette_.link;
    return s;
}

}  // namespace zn
