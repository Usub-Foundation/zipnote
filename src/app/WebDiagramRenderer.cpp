#include "WebDiagramRenderer.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>

namespace zn {

namespace {

constexpr int kRenderTimeoutMs = 15000;

class LocalOnlyInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        const QString scheme = info.requestUrl().scheme();
        if (scheme != u"qrc" && scheme != u"data" && scheme != u"blob")
            info.block(true);
    }
};

QString jsString(const QString& s) {
    // JSON string literal = valid JS string literal
    return QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{s}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

}  // namespace

QWebEngineProfile* sandboxedWebProfile() {
    static QWebEngineProfile* profile = [] {
        auto* p = new QWebEngineProfile(qApp);  // no storage name: off the record, nothing written to disk
        p->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        p->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        p->setUrlRequestInterceptor(new LocalOnlyInterceptor(p));
        p->setSpellCheckEnabled(false);
        QWebEngineSettings* s = p->settings();
        s->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        s->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        s->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
        s->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        s->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
        s->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
        s->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);  // copy/paste inside Excalidraw
        s->setAttribute(QWebEngineSettings::JavascriptCanPaste, true);
        return p;
    }();
    return profile;
}

WebDiagramRenderer::WebDiagramRenderer(QObject* parent) : QObject(parent) {}

void WebDiagramRenderer::ensurePage() {
    if (page_)
        return;
    page_ = new QWebEnginePage(sandboxedWebProfile(), this);
    bridge_ = new MermaidBridge(this);
    auto* channel = new QWebChannel(page_);
    channel->registerObject(QStringLiteral("zn"), bridge_);
    page_->setWebChannel(channel);
    connect(bridge_, &MermaidBridge::pageReady, this, [this] {
        ready_ = true;
        pump();
    });
    connect(bridge_, &MermaidBridge::finished, this, [this](int id, bool ok, const QString& dataUrl, const QString& error) {
        Done done = running_.take(id);
        if (!done)
            return;
        if (!ok) {
            done({}, error);
            return;
        }
        const int comma = static_cast<int>(dataUrl.indexOf(u','));
        done(QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1()), {});
    });
    page_->load(QUrl(QStringLiteral("qrc:/web/mermaid.html")));
}

void WebDiagramRenderer::render(const QString& source, bool dark, Done done) {
    ensurePage();
    const int id = nextId_++;
    running_.insert(id, std::move(done));
    queue_.enqueue({id, source, dark});
    QTimer::singleShot(kRenderTimeoutMs, this, [this, id] {
        if (Done d = running_.take(id))
            d({}, tr("Mermaid rendering timed out"));
    });
    pump();
}

void WebDiagramRenderer::pump() {
    if (!ready_)
        return;
    while (!queue_.isEmpty()) {
        const Job j = queue_.dequeue();
        page_->runJavaScript(QStringLiteral("znRender(%1, %2, %3)").arg(j.id).arg(jsString(j.source)).arg(j.dark ? "true" : "false"));
    }
}

}  // namespace zn
