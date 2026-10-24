#include "ExcalidrawDialog.h"

#include "Theme.h"
#include "WebDiagramRenderer.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineView>

namespace zn {

ExcalidrawDialog::ExcalidrawDialog(const QByteArray& sceneJson, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Drawing"));
    resize(1100, 760);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 10);

    view_ = new QWebEngineView(this);
    auto* page = new QWebEnginePage(sandboxedWebProfile(), view_);
    view_->setPage(page);
    bridge_ = new ExcalidrawBridge(this);
    bridge_->scene = QString::fromUtf8(sceneJson);
    bridge_->dark = Theme::isDark();
    auto* channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("zn"), bridge_);
    page->setWebChannel(channel);
    page->load(QUrl(QStringLiteral("qrc:/web/excalidraw.html")));
    lay->addWidget(view_, 1);

    auto* buttons = new QDialogButtonBox(this);
    buttons->setContentsMargins(12, 0, 12, 0);
    auto* hint = new QLabel(tr("Excalidraw runs offline; the drawing is stored encrypted in the vault."), this);
    hint->setStyleSheet(QStringLiteral("color: %1").arg(Theme::palette().tertiaryText.name()));
    auto* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    auto* done = buttons->addButton(tr("Done"), QDialogButtonBox::AcceptRole);
    done->setProperty("accent", true);
    done->setDefault(true);
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(12, 0, 12, 0);
    bottom->addWidget(hint, 1);
    bottom->addWidget(buttons);
    lay->addLayout(bottom);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(done, &QPushButton::clicked, this, &ExcalidrawDialog::save);
    connect(bridge_, &ExcalidrawBridge::savedScene, this,
            [this](const QString& json, const QString& dataUrl, const QString& darkDataUrl, const QString& text) {
                auto decode = [](const QString& url) {
                    const int comma = static_cast<int>(url.indexOf(u','));
                    return comma > 0 ? QByteArray::fromBase64(url.mid(comma + 1).toLatin1()) : QByteArray();
                };
                scene_ = json.toUtf8();
                png_ = decode(dataUrl);
                darkPng_ = decode(darkDataUrl);
                text_ = text;
                accept();
            });
    connect(bridge_, &ExcalidrawBridge::saveFailed, this, [this](const QString& error) {
        QMessageBox::warning(this, tr("Drawing"), tr("The drawing could not be saved: %1").arg(error));
    });
}

void ExcalidrawDialog::save() {
    view_->page()->runJavaScript(QStringLiteral("znSave()"));
}

}  // namespace zn
