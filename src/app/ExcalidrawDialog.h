#pragma once

#include <QDialog>

class QWebEngineView;

namespace zn {

class ExcalidrawBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString scene MEMBER scene CONSTANT)
    Q_PROPERTY(bool dark MEMBER dark CONSTANT)
public:
    using QObject::QObject;
    QString scene;
    bool dark = false;
    Q_INVOKABLE void saved(const QString& sceneJson, const QString& pngDataUrl, const QString& darkPngDataUrl,
                           const QString& text) {
        emit savedScene(sceneJson, pngDataUrl, darkPngDataUrl, text);
    }
    Q_INVOKABLE void failed(const QString& error) { emit saveFailed(error); }
signals:
    void savedScene(const QString& sceneJson, const QString& pngDataUrl, const QString& darkPngDataUrl, const QString& text);
    void saveFailed(const QString& error);
};

// The Excalidraw editor (MIT, bundled offline) in a dialog. The scene is saved as an encrypted attachment
// with a PNG preview for the note and its text labels for search.
class ExcalidrawDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExcalidrawDialog(const QByteArray& sceneJson, QWidget* parent = nullptr);

    QByteArray sceneJson() const { return scene_; }
    QByteArray previewPng() const { return png_; }
    QByteArray darkPreviewPng() const { return darkPng_; }  // rendered by Excalidraw's dark mode
    QString searchText() const { return text_; }

public slots:
    void save();

private:
    QWebEngineView* view_;
    ExcalidrawBridge* bridge_;
    QByteArray scene_;
    QByteArray png_;
    QByteArray darkPng_;
    QString text_;
};

}  // namespace zn
