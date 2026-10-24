#include "Icons.h"

#include "Theme.h"

#include <QGuiApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>

namespace zn::icons {

namespace {

// 24x24 viewBox, stroke = currentColor
const QHash<QString, const char*>& sources() {
    static const QHash<QString, const char*> s = {
        {"sidebar", R"(<rect x="3" y="4.5" width="18" height="15" rx="3"/><path d="M9.5 4.5v15M5.5 8h2M5.5 11h2"/>)"},
        {"compose", R"(<path d="M11 4.5H7a3 3 0 0 0-3 3v9.5a3 3 0 0 0 3 3h9.5a3 3 0 0 0 3-3V13"/><path d="M17.6 3.6a1.9 1.9 0 0 1 2.8 2.8l-8 8-3.6.8.8-3.6z"/>)"},
        {"trash", R"(<path d="M4.5 6.5h15M9.5 6.5V4.8c0-.7.5-1.3 1.3-1.3h2.4c.8 0 1.3.6 1.3 1.3v1.7M6.5 6.5l.9 12.2c.1 1 .9 1.8 1.9 1.8h5.4c1 0 1.8-.8 1.9-1.8l.9-12.2M10.2 10.5v6M13.8 10.5v6"/>)"},
        {"format", R"(<path d="M3.5 18.5 8 6.5l4.5 12M5.2 14.2h5.6"/><path d="M14.5 11.6c.5-1 1.5-1.6 2.7-1.6 1.7 0 2.8 1 2.8 2.7v5.8M20 15c-.7-.4-1.6-.6-2.6-.6-1.7 0-2.9.8-2.9 2.1 0 1.2 1 2 2.4 2 1.4 0 2.6-.8 3.1-1.8"/>)"},
        {"checklist", R"(<circle cx="6" cy="7" r="2.2"/><path d="m4.9 7 .8.8 1.5-1.6"/><circle cx="6" cy="17" r="2.2"/><path d="M11 7h9M11 17h9"/>)"},
        {"table", R"(<rect x="3.5" y="4.5" width="17" height="15" rx="2.5"/><path d="M3.5 9.5h17M3.5 14.5h17M9.5 9.5v10M15 9.5v10"/>)"},
        {"diagram", R"(<rect x="9" y="3.5" width="6" height="4.5" rx="1.2"/><rect x="3.5" y="15.5" width="6" height="4.5" rx="1.2"/><rect x="14.5" y="15.5" width="6" height="4.5" rx="1.2"/><path d="M12 8v3.8M6.5 15.5v-1.6c0-.8.6-1.4 1.4-1.4h8.2c.8 0 1.4.6 1.4 1.4v1.6"/>)"},
        {"drawing", R"(<path d="M4 17.5c2.2-3.4 4.2-5.1 5.9-5.1 2.4 0 1 4.6 3.3 4.6 1.5 0 2.8-1.6 4.3-4.4"/><path d="m15.8 4.2 4 4-7.5 7.5-4.6.6.6-4.6z"/>)"},
        {"link", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3.1-3.1a4 4 0 0 0-5.7-5.7L11.6 6.7"/><path d="M14 10a4 4 0 0 0-5.7 0l-3.1 3.1a4 4 0 0 0 5.7 5.7l1.5-1.5"/>)"},
        {"lock", R"(<rect x="5" y="10.5" width="14" height="10" rx="2.5"/><path d="M8 10.5V7.8a4 4 0 0 1 8 0v2.7"/>)"},
        {"lockfill", R"(<rect x="5" y="10.5" width="14" height="10" rx="2.5" fill="currentColor"/><path d="M8 10.5V7.8a4 4 0 0 1 8 0v2.7"/>)"},
        {"search", R"(<circle cx="10.5" cy="10.5" r="6"/><path d="m15 15 5 5"/>)"},
        {"folder", R"(<path d="M3.5 7.5c0-1.1.9-2 2-2h3.8l2 2.2h7.2c1.1 0 2 .9 2 2v7.8c0 1.1-.9 2-2 2h-13c-1.1 0-2-.9-2-2z"/><path d="M3.5 10h17"/>)"},
        {"folderplus", R"(<path d="M20.5 12V9.7c0-1.1-.9-2-2-2h-7.2l-2-2.2H5.5c-1.1 0-2 .9-2 2v9.8c0 1.1.9 2 2 2H12"/><path d="M18 14.5v6M15 17.5h6"/>)"},
        {"notes", R"(<rect x="5" y="3.5" width="14" height="17" rx="2.5"/><path d="M8.5 8h7M8.5 11.5h7M8.5 15h4.5"/>)"},
        {"tag", R"(<path d="M9.5 4 7.5 20M16.5 4l-2 16M4.5 9h15.5M4 15h15.5"/>)"},
        {"pin", R"(<path d="M9 3.5h6M10 3.5v5.2L7 13h10l-3-4.3V3.5M12 13v7.5"/>)"},
        {"pinfill", R"(<path d="M9 3.5h6M10 3.5v5.2L7 13h10l-3-4.3V3.5z" fill="currentColor"/><path d="M12 13v7.5"/>)"},
        {"clear", R"(<circle cx="12" cy="12" r="8.5" fill="currentColor" stroke="none"/><path d="m9 9 6 6M15 9l-6 6" stroke="#ffffff"/>)"},
        {"chevronup", R"(<path d="m6 15 6-6 6 6"/>)"},
        {"chevrondown", R"(<path d="m6 9 6 6 6-6"/>)"},
        {"chevronright", R"(<path d="m9 6 6 6-6 6"/>)"},
        {"close", R"(<path d="m6.5 6.5 11 11M17.5 6.5l-11 11"/>)"},
        {"recent", R"(<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>)"},
        {"vault", R"(<rect x="3.5" y="5" width="17" height="14" rx="3"/><circle cx="12" cy="12" r="3.2"/><path d="M12 8.8v-1M12 16.2v-1M15.2 12h1M7.8 12h1M7 19v1.5M17 19v1.5"/>)"},
        {"mermaid", R"(<path d="M4 18c3-1 5-3.4 5.6-7.4M20 18c-3-1-5-3.4-5.6-7.4M9.6 10.6C10.2 7 11 5 12 4c1 1 1.8 3 2.4 6.6M9.6 10.6c.8.9 1.6 1.4 2.4 1.4s1.6-.5 2.4-1.4"/>)"},
        {"plantuml", R"(<path d="M12 20.5V12M12 12c0-4 2.5-7 7-7.5-.3 4.5-3 7.5-7 7.5zM12 14.5c0-3-2-5.3-5.5-5.7.3 3.4 2.5 5.7 5.5 5.7z"/>)"},
        {"quote", R"(<path d="M5 6.5v11M9 8.5h10M9 12h10M9 15.5h7"/>)"},
        {"code", R"(<path d="m8.5 7.5-4.5 4.5 4.5 4.5M15.5 7.5l4.5 4.5-4.5 4.5M13.5 5.5l-3 13"/>)"},
        {"image", R"(<rect x="3.5" y="5" width="17" height="14" rx="2.5"/><circle cx="9" cy="10" r="1.6"/><path d="m4 17 5-4.5 3.5 3 3-2.5 4.5 4"/>)"},
        {"key", R"(<circle cx="8" cy="12" r="4"/><path d="M12 12h8.5M17.5 12v3M20 12v2.2"/>)"},
    };
    return s;
}

QByteArray svgFor(const QString& name, const QColor& color) {
    const char* body = sources().value(name, nullptr);
    if (!body)
        body = R"(<circle cx="12" cy="12" r="8"/>)";
    QByteArray svg = QByteArrayLiteral(
                         "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
                         "stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'>") +
                     body + "</svg>";
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    return svg;
}

class SvgIconEngine : public QIconEngine {
public:
    SvgIconEngine(QString name, QColor color) : name_(std::move(name)), color_(color) {}
    void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        QColor c = color_.isValid() ? color_ : Theme::palette().secondaryText;
        if (mode == QIcon::Disabled)
            c.setAlphaF(0.35f);
        else if (mode == QIcon::Active || mode == QIcon::Selected)
            c = c.darker(115);
        QSvgRenderer r(svgFor(name_, c));
        p->setOpacity(c.alphaF());
        r.render(p, rect);
        p->setOpacity(1.0);
    }
    QIconEngine* clone() const override { return new SvgIconEngine(name_, color_); }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

private:
    QString name_;
    QColor color_;
};

}  // namespace

QIcon icon(const QString& name, const QColor& color) {
    return QIcon(new SvgIconEngine(name, color));
}

QPixmap pixmap(const QString& name, int size, const QColor& color, qreal dpr) {
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QSvgRenderer r(svgFor(name, color));
    p.setOpacity(color.alphaF());
    r.render(&p, QRectF(0, 0, size, size));
    return pm;
}

}  // namespace zn::icons
