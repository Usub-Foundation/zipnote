#include "Crypto.h"
#include "MainWindow.h"
#include "Theme.h"

#ifdef ZN_WITH_WEBENGINE
#include "ExcalidrawDialog.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QIcon>
#include <QMessageBox>
#include <QStyleHints>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <unistd.h>
#endif

int main(int argc, char** argv) {
#ifdef ZN_WITH_WEBENGINE
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#if defined(Q_OS_LINUX)
    // Chromium refuses to start its sandbox for root; pages are local-only anyway (see WebDiagramRenderer).
    if (geteuid() == 0 && qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX"))
        qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
#endif
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ZipNotes"));
    QApplication::setOrganizationName(QStringLiteral("ZipNotes"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION_STRING));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/zipnotes.svg")));

    if (!zn::crypto::init()) {
        QMessageBox::critical(nullptr, QStringLiteral("ZipNotes"), QStringLiteral("libsodium could not be initialised."));
        return 1;
    }

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Encrypted notes in a single zip file"));
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument(QStringLiteral("vault"), QStringLiteral("Vault (.zip) to open"));
    const QCommandLineOption theme(QStringLiteral("theme"), QStringLiteral("light or dark"), QStringLiteral("scheme"));
    // automation for screenshots and smoke tests; the password comes from an environment variable so it
    // never appears in the process list
    const QCommandLineOption passwordEnv(QStringLiteral("password-env"), QStringLiteral("Unlock with the password in this environment variable"), QStringLiteral("name"));
    const QCommandLineOption steps(QStringLiteral("steps"), QStringLiteral("Automation steps separated by ';'"), QStringLiteral("steps"));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot after the steps and quit"), QStringLiteral("png"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Window size WxH"), QStringLiteral("size"));
    const QCommandLineOption createDemo(QStringLiteral("create-demo"), QStringLiteral("Create the vault with sample notes if it does not exist"));
    QCommandLineOption excalidrawCheck(QStringLiteral("check-excalidraw"), QStringLiteral("Smoke test of the bundled Excalidraw"), QStringLiteral("png"));
    excalidrawCheck.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOptions({theme, passwordEnv, steps, screenshot, size, createDemo, excalidrawCheck});
    cli.process(app);

    if (cli.value(theme) == u"dark")
        zn::Theme::instance().setForcedScheme(Qt::ColorScheme::Dark);
    else if (cli.value(theme) == u"light")
        zn::Theme::instance().setForcedScheme(Qt::ColorScheme::Light);
    zn::Theme::instance().apply(app);

#ifdef ZN_WITH_WEBENGINE
    if (cli.isSet(excalidrawCheck)) {
        // Loads the offline Excalidraw with a small scene, saves it and writes the preview it produced.
        const QByteArray scene = R"({"type":"excalidraw","version":2,"elements":[
            {"id":"r1","type":"rectangle","x":20,"y":20,"width":220,"height":90,"angle":0,"strokeColor":"#1e1e1e",
             "backgroundColor":"#ffec99","fillStyle":"hachure","strokeWidth":2,"strokeStyle":"solid","roughness":1,
             "opacity":100,"groupIds":[],"roundness":{"type":3},"seed":1,"version":1,"versionNonce":1,"isDeleted":false,
             "boundElements":[],"updated":1,"link":null,"locked":false},
            {"id":"t1","type":"text","x":48,"y":50,"width":170,"height":25,"angle":0,"strokeColor":"#1e1e1e",
             "backgroundColor":"transparent","fillStyle":"solid","strokeWidth":1,"strokeStyle":"solid","roughness":1,
             "opacity":100,"groupIds":[],"roundness":null,"seed":2,"version":1,"versionNonce":2,"isDeleted":false,
             "boundElements":[],"updated":1,"link":null,"locked":false,"text":"Encrypted sketch","fontSize":20,
             "fontFamily":1,"textAlign":"left","verticalAlign":"top","baseline":18,"containerId":null,
             "originalText":"Encrypted sketch","lineHeight":1.25}],
            "appState":{"viewBackgroundColor":"#ffffff"},"files":{}})";
        QByteArray sceneData = scene;
        if (const QString file = qEnvironmentVariable("ZN_SCENE"); !file.isEmpty()) {
            QFile f(file);
            if (f.open(QIODevice::ReadOnly))
                sceneData = f.readAll();
        }
        auto* dlg = new zn::ExcalidrawDialog(sceneData);
        dlg->show();
        const QString out = cli.value(excalidrawCheck);
        QTimer::singleShot(9000, dlg, [dlg, out] {
            dlg->grab().save(out + QStringLiteral(".dialog.png"));
            QMetaObject::invokeMethod(dlg, "save");
        });
        QObject::connect(dlg, &QDialog::accepted, [dlg, out] {
            QFile f(out);
            if (f.open(QIODevice::WriteOnly))
                f.write(dlg->previewPng());
            QFile dark(out + QStringLiteral(".dark.png"));
            if (dark.open(QIODevice::WriteOnly))
                dark.write(dlg->darkPreviewPng());
            qInfo().noquote() << "excalidraw scene bytes:" << dlg->sceneJson().size() << "text:" << dlg->searchText()
                              << "preview bytes:" << dlg->previewPng().size();
            dlg->deleteLater();
            QApplication::quit();
        });
        QTimer::singleShot(30000, [] {
            qWarning("excalidraw check timed out");
            QApplication::exit(2);
        });
        return app.exec();
    }
#endif

    zn::MainWindow w;
    if (cli.isSet(size)) {
        const QStringList wh = cli.value(size).split(u'x');
        if (wh.size() == 2)
            w.resize(wh[0].toInt(), wh[1].toInt());
    }
    w.show();

    const QStringList args = cli.positionalArguments();
    if (!args.isEmpty()) {
        const QString path = QFileInfo(args.first()).absoluteFilePath();
        const QString password = cli.isSet(passwordEnv) ? qEnvironmentVariable(cli.value(passwordEnv).toUtf8().constData()) : QString();
        if (cli.isSet(createDemo) && !QFileInfo::exists(path) && !password.isEmpty())
            w.createWith(path, password);
        else if (!password.isEmpty())
            w.unlockWith(path, password);
        else
            w.showVault(path);
    }
    if (cli.isSet(screenshot) || cli.isSet(steps))
        w.runAutomation(cli.value(steps).split(u';', Qt::SkipEmptyParts), cli.value(screenshot));
    return app.exec();
}
