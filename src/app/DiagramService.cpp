#include "DiagramService.h"

#include "Theme.h"
#include "VaultController.h"

#ifdef ZN_WITH_WEBENGINE
#include "WebDiagramRenderer.h"
#endif

#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace zn {

namespace {

constexpr qreal kRenderScale = 2.0;  // diagrams are rendered at 2x and shown at half size
constexpr int kPlantUmlTimeoutMs = 20000;

QString configuredPlantUml() {
    return QSettings().value(QStringLiteral("plantuml/command")).toString().trimmed();
}

// Adds missing @startuml/@enduml lines. A local PlantUML also renders at 2x like the other renderers; a dpi
// set by the note itself comes later and wins.
QString plantUmlText(const QString& source, bool hiDpi) {
    QString text = source;
    if (!text.contains(QLatin1String("@start")))
        text = QStringLiteral("@startuml\n") + text + QStringLiteral("\n@enduml\n");
    static const QRegularExpression startLine(QStringLiteral(R"((@start\w+[^\n]*\n))"));
    if (hiDpi && !text.contains(QLatin1String("skinparam dpi"), Qt::CaseInsensitive))
        text.replace(startLine, QStringLiteral("\\1skinparam dpi 192\n"));
    return text;
}

// Colours chosen by the diagram itself (skinparam, <style>, !theme, or a standard library such as C4) are made for
// a white page
bool hasOwnLook(const QString& plantUml) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*(!theme\s|!include\s*<|skinparam\s+\w*colou?r\b|<style>))"),
                                       QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    return re.match(plantUml).hasMatch();
}

QImage decodePng(const QByteArray& png) {
    QImage img = QImage::fromData(png, "PNG");
    if (!img.isNull())
        img.setDevicePixelRatio(kRenderScale);
    return img;
}

}  // namespace

DiagramService::DiagramService(VaultController& vc, QObject* parent) : QObject(parent), vc_(vc) {
#ifdef ZN_WITH_WEBENGINE
    mermaid_ = new WebDiagramRenderer(QUrl(QStringLiteral("qrc:/web/mermaid.html")), QStringLiteral("Mermaid"), this);
    plantUml_ = new WebDiagramRenderer(QUrl(QStringLiteral("qrc:/web/plantuml.html")), QStringLiteral("PlantUML"), this);
#endif
}

DiagramService::~DiagramService() = default;

// Mermaid and the built-in PlantUML follow the app theme. A local PlantUML, and PlantUML diagrams with a look of
// their own, render light, on their own white page.
bool DiagramService::renderDark(DiagramKind kind, const QString& source) const {
    if (!Theme::isDark())
        return false;
    return kind == DiagramKind::Mermaid || (kind == DiagramKind::PlantUml && builtInPlantUml() && !hasOwnLook(source));
}

QString DiagramService::key(DiagramKind kind, const QString& source) const {
    return VaultController::renderKey(kind, source, renderDark(kind, source));
}

void DiagramService::clearMemory() {
    memory_.clear();
    errors_.clear();
    unavailable_.clear();
}

QStringList DiagramService::errors() const {
    return errors_.values() + unavailable_.values();
}

bool DiagramService::webAvailable(QString* why) const {
#ifdef ZN_WITH_WEBENGINE
    // tests set this so headless CI machines never start Chromium
    if (qEnvironmentVariableIsSet("ZN_DISABLE_WEBENGINE")) {
        if (why)
            *why = tr("Rendering is disabled (ZN_DISABLE_WEBENGINE).");
        return false;
    }
    return true;
#else
    if (why)
        *why = tr("This build has no Qt WebEngine, so Mermaid diagrams cannot be rendered.");
    return false;
#endif
}

bool DiagramService::available(DiagramKind kind, QString* why) const {
    switch (kind) {
    case DiagramKind::Mermaid:
        return webAvailable(why);
    case DiagramKind::PlantUml:
        if (!builtInPlantUml() && plantUmlCommand().isEmpty()) {
            if (why)
                *why = tr("PlantUML is not installed. Install it (or Java and plantuml.jar) and set the command in Settings.");
            return false;
        }
        return true;
    default:
        return false;
    }
}

bool DiagramService::builtInPlantUml() const {
    return configuredPlantUml().isEmpty() && webAvailable();
}

QString DiagramService::plantUmlCommand() const {
    const QString configured = configuredPlantUml();
    if (!configured.isEmpty() || webAvailable())
        return configured;
    if (const QString exe = QStandardPaths::findExecutable(QStringLiteral("plantuml")); !exe.isEmpty())
        return exe;
    // apps started from Finder get a minimal PATH without Homebrew or MacPorts
    const QStringList extra = {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin"), QStringLiteral("/opt/local/bin")};
    return QStandardPaths::findExecutable(QStringLiteral("plantuml"), extra);
}

QStringList DiagramService::plantUmlArguments(const QString& command) {
    QStringList parts = QProcess::splitCommand(command);
    if (parts.size() == 1 && parts.first().endsWith(QLatin1String(".jar"), Qt::CaseInsensitive))
        parts = {QStringLiteral("java"), QStringLiteral("-Djava.awt.headless=true"), QStringLiteral("-jar"), parts.first()};
    parts << QStringLiteral("-tpng") << QStringLiteral("-pipe") << QStringLiteral("-charset") << QStringLiteral("UTF-8");
    return parts;
}

QImage DiagramService::image(DiagramKind kind, const QString& source) {
    const QString k = key(kind, source);
    if (auto it = memory_.find(k); it != memory_.end()) {
        if (!it->persisted) {
            vc_.storeRender(k, it->png);
            it->persisted = true;
        }
        return it->image;
    }
    if (auto cached = vc_.cachedRender(k)) {
        Entry e{decodePng(*cached), *cached, true};
        if (!e.image.isNull()) {
            memory_.insert(k, e);
            return e.image;
        }
    }
    if (auto err = errors_.constFind(k); err != errors_.cend())
        return placeholder(kind, *err, true);
    QString why;
    if (!available(kind, &why)) {
        unavailable_.insert(QStringLiteral("%1: %2").arg(diagramKindLabel(kind), why));
        return placeholder(kind, why, true);
    }
    if (source.trimmed().isEmpty())
        return placeholder(kind, tr("Empty diagram. Double-click to edit."), false);
    start(kind, source, {});
    return placeholder(kind, tr("Rendering %1…").arg(diagramKindLabel(kind)), false);
}

void DiagramService::preview(DiagramKind kind, const QString& source, Callback cb) {
    const QString k = key(kind, source);
    if (auto it = memory_.constFind(k); it != memory_.cend()) {
        cb(it->image, {});
        return;
    }
    if (auto err = errors_.constFind(k); err != errors_.cend()) {
        cb({}, *err);
        return;
    }
    QString why;
    if (!available(kind, &why)) {
        cb({}, why);
        return;
    }
    start(kind, source, std::move(cb));
}

void DiagramService::start(DiagramKind kind, const QString& source, Callback cb) {
    const QString k = key(kind, source);
    const bool running = pending_.contains(k);
    pending_[k].append(std::move(cb));
    if (running)
        return;
    auto done = [this, k](const QByteArray& png, const QString& error) { finish(k, png, error, false); };
    if (kind == DiagramKind::PlantUml && !builtInPlantUml()) {
        runPlantUml(plantUmlText(source, true), done);
        return;
    }
#ifdef ZN_WITH_WEBENGINE
    if (kind == DiagramKind::PlantUml)
        plantUml_->render(plantUmlText(source, false), renderDark(kind, source), done);
    else
        mermaid_->render(source, renderDark(kind, source), done);
#else
    done({}, tr("%1 needs Qt WebEngine").arg(diagramKindLabel(kind)));
#endif
}

void DiagramService::finish(const QString& key, const QByteArray& png, const QString& error, bool) {
    const QList<Callback> callbacks = pending_.take(key);
    QImage img;
    QString err = error;
    if (err.isEmpty()) {
        img = decodePng(png);
        if (img.isNull())
            err = tr("The renderer returned no image");
    }
    // a render that finishes after the vault was locked must not keep note content in memory
    if (!vc_.isOpen()) {
        img = {};
        err = tr("The vault is locked");
    } else if (err.isEmpty()) {
        memory_.insert(key, {img, png, false});
    } else {
        errors_.insert(key, err.left(600));
    }
    for (const auto& cb : callbacks)
        if (cb)
            cb(img, err);
    emit rendered(key);
}

void DiagramService::runPlantUml(const QString& text, std::function<void(QByteArray, QString)> done) {
    QStringList args = plantUmlArguments(plantUmlCommand());
    const QString program = args.takeFirst();
    auto* proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // no !include of local files or URLs from a note
    env.insert(QStringLiteral("PLANTUML_SECURITY_PROFILE"), QStringLiteral("SANDBOX"));
    env.insert(QStringLiteral("JAVA_TOOL_OPTIONS"), QStringLiteral("-Djava.awt.headless=true"));
    proc->setProcessEnvironment(env);
    auto finished = std::make_shared<bool>(false);
    auto complete = [proc, done, finished](const QByteArray& png, const QString& err) {
        if (*finished)
            return;
        *finished = true;
        done(png, err);
        proc->deleteLater();
    };
    connect(proc, &QProcess::finished, this, [proc, complete](int code, QProcess::ExitStatus status) {
        const QByteArray out = proc->readAllStandardOutput();
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (status != QProcess::NormalExit || out.isEmpty() || !out.startsWith("\x89PNG"))
            complete({}, err.isEmpty() ? tr("PlantUML failed (exit code %1)").arg(code) : err);
        else
            complete(out, {});
    });
    connect(proc, &QProcess::errorOccurred, this, [proc, complete](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            complete({}, tr("Cannot start PlantUML: %1").arg(proc->errorString()));
    });
    QTimer::singleShot(kPlantUmlTimeoutMs, proc, [proc, complete] {
        proc->kill();
        complete({}, tr("PlantUML timed out"));
    });
    proc->start(program, args);
    proc->write(text.toUtf8());
    proc->closeWriteChannel();
}

QImage DiagramService::placeholder(DiagramKind kind, const QString& message, bool error) const {
    const ThemePalette& pal = Theme::palette();
    const QSizeF size(460, error ? 120 : 96);
    QImage img((size * kRenderScale).toSize(), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(kRenderScale);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(1, 1), size - QSizeF(2, 2)), 10, 10);
    p.fillPath(path, pal.dark ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 8));
    p.setPen(QPen(error ? QColor(0xD7, 0x3A, 0x3A, 160) : pal.separator, 1.2, Qt::DashLine));
    p.drawPath(path);
    QFont f = Theme::instance().uiFont(QFont().pointSizeF() + 1, QFont::DemiBold);
    p.setFont(f);
    p.setPen(pal.title);
    p.drawText(QRectF(18, 12, size.width() - 36, 24), Qt::AlignLeft | Qt::AlignVCenter, diagramKindLabel(kind));
    p.setFont(Theme::instance().uiFont(QFont().pointSizeF()));
    p.setPen(error ? QColor(0xD7, 0x3A, 0x3A) : pal.secondaryText);
    p.drawText(QRectF(18, 40, size.width() - 36, size.height() - 50), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, message);
    return img;
}

}  // namespace zn
