#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QUrl>

#include <functional>

class QWebEnginePage;
class QWebEngineProfile;

namespace zn {

// An off-the-record WebEngine profile that can only load qrc:, data: and blob: URLs. The Mermaid, PlantUML
// and Excalidraw pages run in it, so note contents never leave the process.
QWebEngineProfile* sandboxedWebProfile();

class DiagramPageBridge : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void ready() { emit pageReady(); }
    Q_INVOKABLE void done(int id, bool ok, const QString& dataUrl, const QString& error) { emit finished(id, ok, dataUrl, error); }
signals:
    void pageReady();
    void finished(int id, bool ok, const QString& dataUrl, const QString& error);
};

// Renders diagram sources to PNG in a hidden page that stays loaded for the app's lifetime. The page defines
// znRender(id, source, dark) and answers through the "zn" channel object.
class WebDiagramRenderer : public QObject {
    Q_OBJECT
public:
    WebDiagramRenderer(const QUrl& page, const QString& name, QObject* parent = nullptr);
    using Done = std::function<void(const QByteArray& png, const QString& error)>;
    void render(const QString& source, bool dark, Done done);

private:
    struct Job {
        int id;
        QString source;
        bool dark;
    };
    void ensurePage();
    void pump();

    QUrl url_;
    QString name_;  // for messages: "Mermaid", "PlantUML"
    QWebEnginePage* page_ = nullptr;
    DiagramPageBridge* bridge_ = nullptr;
    bool ready_ = false;
    int nextId_ = 1;
    QQueue<Job> queue_;
    QHash<int, Done> running_;
};

}  // namespace zn
