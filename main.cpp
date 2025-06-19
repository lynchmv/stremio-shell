#include <clocale>

#include <QCoreApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtWebEngineQuick/QtWebEngineQuick>

#include <QSysInfo>
#include <QStandardPaths>
#include <QSystemTrayIcon>

#include "systemtray.h"
#include "mainapplication.h"
#include "stremioprocess.h"
#include "mpv.h"
#include "screensaver.h"
#include "razerchroma.h"
#include "qclipboardproxy.h"

#define APP_TITLE "Stremio - Freedom to Stream"
#define DESKTOP true

// Type alias already provided
typedef QApplication Application;

void InitializeParameters(QQmlApplicationEngine *engine, MainApp& app) {
    QQmlContext *ctx = engine->rootContext();
    auto systemTray = new SystemTray();

    ctx->setContextProperty("applicationDirPath", QGuiApplication::applicationDirPath());
    ctx->setContextProperty("appTitle", QString(APP_TITLE));
    ctx->setContextProperty("autoUpdater", app.autoupdater);
    ctx->setContextProperty("systemTray", systemTray);

#ifdef QT_DEBUG
    ctx->setContextProperty("debug", true);
#else
    ctx->setContextProperty("debug", false);
#endif
}

int main(int argc, char **argv)
{
    // Qt6 WebEngine + OpenGL requirement: Set attributes before app
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGLRhi);

    Application app(argc, argv); // use QApplication (already typedef'd)

    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--autoplay-policy=no-user-gesture-required");

#ifdef _WIN32
    Application::setAttribute(Qt::AA_UseOpenGLES);
    auto winVer = QSysInfo::windowsVersion();
    if (winVer <= QSysInfo::WV_WINDOWS8 && winVer != QSysInfo::WV_None)
        qputenv("NODE_SKIP_PLATFORM_CHECK", "1");
    if (winVer <= QSysInfo::WV_WINDOWS7 && winVer != QSysInfo::WV_None)
        qputenv("QT_ANGLE_PLATFORM", "d3d9");
#endif

#ifndef Q_OS_LINUX
    Application::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    Application::setApplicationName("Stremio");
    Application::setApplicationVersion(STREMIO_SHELL_VERSION);
    Application::setOrganizationName("Smart Code ltd");
    Application::setOrganizationDomain("stremio.com");

    MainApp mainApp(argc, argv, true);

#ifndef Q_OS_MACOS
    if (mainApp.isSecondary()) {
        if (mainApp.arguments().count() > 1)
            mainApp.sendMessage(mainApp.arguments().at(1).toUtf8());
        else
            mainApp.sendMessage("SHOW");
        return 0;
    }
#endif

    app.setWindowIcon(QIcon(":/images/stremio_window.png"));

    // libmpv workaround
    std::setlocale(LC_NUMERIC, "C");

    // Explicitly initialize QtWebEngine for Qt6
    QtWebEngineQuick::initialize();

    // Load engine
    QQmlApplicationEngine engine;

    // Register custom types
    qmlRegisterType<Process>("com.stremio.process", 1, 0, "Process");
    qmlRegisterType<ScreenSaver>("com.stremio.screensaver", 1, 0, "ScreenSaver");
    qmlRegisterType<MpvObject>("com.stremio.libmpv", 1, 0, "MpvObject");
    qmlRegisterType<RazerChroma>("com.stremio.razerchroma", 1, 0, "RazerChroma");
    qmlRegisterType<ClipboardProxy>("com.stremio.clipboard", 1, 0, "Clipboard");

    InitializeParameters(&engine, mainApp);
    engine.load(QUrl(QStringLiteral("qrc:/main.qml")));

    // Check if loading main.qml succeeded
    if (engine.rootObjects().isEmpty())
        return -1;

#ifndef Q_OS_MACOS
    QObject::connect(&mainApp, &SingleApplication::receivedMessage, &mainApp, &MainApp::processMessage);
#endif

    // Safe connection to QML slot
    QObject *rootObj = engine.rootObjects().first();
    QObject::connect(&mainApp,
                     SIGNAL(receivedMessage(QVariant, QVariant)),
                     rootObj,
                     SLOT(onAppMessageReceived(QVariant, QVariant)));

    return app.exec();
}

