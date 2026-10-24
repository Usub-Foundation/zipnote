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
//   PlantUML   - a local `plantuml` (or `java -jar plantuml.jar`) fed through stdin/stdout, sandbox profile
// Finished renders are cached in memory and, encrypted, inside the vault, so reopening a note does not
// need WebEngine or Java again.
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
    QString plantUmlCommand() const;
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
    void start(DiagramKind kind, const QString& source, bool dark, Callback cb);
    void finish(const QString& key, const QByteArray& png, const QString& error, bool persist);
    void runPlantUml(const QString& source, std::function<void(QByteArray, QString)> done);
    QImage placeholder(DiagramKind kind, const QString& message, bool error) const;

    VaultController& vc_;
    QHash<QString, Entry> memory_;
    QHash<QString, QString> errors_;
    QHash<QString, QList<Callback>> pending_;
    WebDiagramRenderer* web_ = nullptr;
    int runningPlantUml_ = 0;
};

}  // namespace zn
