#pragma once

#include "NoteText.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>

#include <functional>

namespace zn {

class VaultController;
class WebDiagramRenderer;

// Turns diagram sources into images. Renders run locally and never touch the disk:
//   Mermaid    - mermaid.js inside an off-the-record Qt WebEngine page with all network requests blocked
//   PlantUML   - PlantUML's engine compiled to JavaScript, in the same kind of page; or a local `plantuml`
//                (or `java -jar plantuml.jar`) fed through stdin/stdout with the sandbox profile, when
//                Settings names one or the build has no WebEngine
// Finished renders are cached in memory and, encrypted, inside the vault, so reopening a note does not
// need WebEngine or a local PlantUML again.
class DiagramService : public QObject {
    Q_OBJECT
public:
    explicit DiagramService(VaultController& vc, QObject* parent = nullptr);
    ~DiagramService() override;

    // The rendered diagram if known; otherwise schedules a render and returns a placeholder.
    QImage image(DiagramKind kind, const QString& source);
    QString key(DiagramKind kind, const QString& source) const;

    using Callback = std::function<void(const QImage& image, const QString& error)>;
    void preview(DiagramKind kind, const QString& source, Callback cb);  // for the diagram editor

    bool available(DiagramKind kind, QString* why = nullptr) const;
    bool busy() const { return !pending_.isEmpty(); }  // a render is running
    QStringList errors() const;                          // renders that failed, diagrams that cannot render
    bool builtInPlantUml() const;
    QString plantUmlCommand() const;  // the local PlantUML to run; empty when the built-in engine renders
    static QStringList plantUmlArguments(const QString& command);
    void clearMemory();

signals:
    void rendered(const QString& key);

private:
    struct Entry {
        QImage image;
        QByteArray png;
        bool persisted = false;
    };
    bool webAvailable(QString* why = nullptr) const;
    bool renderDark(DiagramKind kind) const;
    void start(DiagramKind kind, const QString& source, Callback cb);
    void finish(const QString& key, const QByteArray& png, const QString& error, bool persist);
    void runPlantUml(const QString& text, std::function<void(QByteArray, QString)> done);
    QImage placeholder(DiagramKind kind, const QString& message, bool error) const;

    VaultController& vc_;
    QHash<QString, Entry> memory_;
    QHash<QString, QString> errors_;
    QHash<QString, QList<Callback>> pending_;
    QSet<QString> unavailable_;  // why diagrams in open notes could not render
    WebDiagramRenderer* mermaid_ = nullptr;
    WebDiagramRenderer* plantUml_ = nullptr;
};

}  // namespace zn
