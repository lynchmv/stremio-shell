import QtQuick
import QtQuick.Controls
import Qt.labs.platform
import Qt.labs.settings
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtWebEngine
import QtWebChannel
import QtQml

import com.stremio.process 1.0
import com.stremio.screensaver 1.0
import com.stremio.libmpv 1.0
import com.stremio.clipboard 1.0

import "autoupdater.js" as Autoupdater

ApplicationWindow {
    id: root
    visible: true

    minimumWidth: 1000
    minimumHeight: 650

    readonly property int initialWidth: Math.max(root.minimumWidth, Math.min(1600, Screen.desktopAvailableWidth * 0.8))
    readonly property int initialHeight: Math.max(root.minimumHeight, Math.min(1000, Screen.desktopAvailableHeight * 0.8))

    width: root.initialWidth
    height: root.initialHeight

    property bool quitting: false

    color: "#0c0b11";
    title: appTitle

    property var previousVisibility: Window.Windowed
    property bool wasFullScreen: false

    function setFullScreen(fullscreen) {
        if (fullscreen) {
            root.visibility = Window.FullScreen;
            root.wasFullScreen = true;
        } else {
            root.visibility = root.previousVisibility;
            root.wasFullScreen = false;
        }
    }

    function showWindow() {
        if (root.wasFullScreen) {
            root.visibility = Window.FullScreen;
        } else {
            root.visibility = root.previousVisibility;
        }
        root.raise();
        root.requestActivate();
    }

    function updatePreviousVisibility() {
        if (root.visible && root.visibility != Window.FullScreen && root.visibility != Window.Minimized) {
            root.previousVisibility = root.visibility;
        }
    }

    // Transport
    QtObject {
        id: transport
        readonly property string shellVersion: Qt.application.version
        property string serverAddress: "http://127.0.0.1:11470"

        readonly property bool isFullscreen: root.visibility === Window.FullScreen

        signal event(var ev, var args)

        function onEvent(ev, args) {
            if (ev === "quit") quitApp()
            if (ev === "app-ready") transport.flushQueue()
            if (ev === "mpv-command" && args && args[0] !== "run") mpv.command(args)
            if (ev === "mpv-set-prop") {
                mpv.setProperty(args[0], args[1]);
                if (args[0] === "pause") {
                    shouldDisableScreensaver(!args[1]);
                }
            }
            if (ev === "mpv-observe-prop") mpv.observeProperty(args)
            if (ev === "control-event") wakeupEvent()
            if (ev === "wakeup") wakeupEvent()
            if (ev === "set-window-mode") onWindowMode(args)
            if (ev === "open-external") Qt.openUrlExternally(args)
            if (ev === "win-focus" && !root.visible) {
                showWindow();
            }
            if (ev === "win-set-visibility") {
                if (args.hasOwnProperty('fullscreen')) {
                    setFullScreen(args.fullscreen);
                }
            }
            if (ev === "autoupdater-notif-clicked" && autoUpdater.onNotifClicked) {
                autoUpdater.onNotifClicked();
            }
            if (ev === "screensaver-toggle") shouldDisableScreensaver(args.disabled)
            if (ev === "file-close") fileDialog.close()
            if (ev === "file-open") {
                if (typeof args !== "undefined") {
                    var fileDialogDefaults = {
                        title: "Please choose",
                        selectExisting: true,
                        selectFolder: false,
                        selectMultiple: false,
                        nameFilters: [],
                        selectedNameFilter: "",
                        data: null
                    }
                    Object.keys(fileDialogDefaults).forEach(function(key) {
                        fileDialog[key] = args.hasOwnProperty(key) ? args[key] : fileDialogDefaults[key]
                    })
                }
                fileDialog.open()
            }
        }

        property variant queued: []
        function queueEvent() {
            if (transport.queued) transport.queued.push(arguments)
            else transport.event.apply(transport, arguments)
        }
        function flushQueue() {
            if (transport.queued) transport.queued.forEach(function(args) { transport.event.apply(transport, args) })
            transport.queued = null;
        }
    }

    // Utilities
    function onWindowMode(mode) {
        shouldDisableScreensaver(mode === "player")
    }

    function wakeupEvent() {
        shouldDisableScreensaver(true)
        timerScreensaver.restart()
    }

    function shouldDisableScreensaver(condition) {
        if (condition === screenSaver.disabled) return;
        condition ? screenSaver.disable() : screenSaver.enable();
        screenSaver.disabled = condition;
    }

    function isPlayerPlaying() {
        return root.visible && typeof(mpv.getProperty("path"))==="string" && !mpv.getProperty("pause")
    }

    function onAppMessageReceived(instance, message) {
        message = message.toString();
        showWindow();
        if (message !== "SHOW") {
            onAppOpenMedia(message);
        }
    }

    function onAppOpenMedia(message) {
        var url = (message.indexOf('://') > -1 || message.indexOf('magnet:') === 0) ? message : 'file://'+message;
        transport.queueEvent("open-media", url)
    }

    function quitApp() {
        root.quitting = true;
        webView.destroy();
        systemTray.hideIconTray();
        streamingServer.kill();
        streamingServer.waitForFinished(1500);
        Qt.quit();
    }

    Connections {
        target: systemTray

        function onSignalIconMenuAboutToShow() {
            systemTray.updateIsOnTop((root.flags & Qt.WindowStaysOnTopHint) === Qt.WindowStaysOnTopHint);
            systemTray.updateVisibleAction(root.visible);
        }

        function onSignalShow() {
            if(root.visible) {
                root.hide();
            } else {
                showWindow();
            }
        }

        function onSignalAlwaysOnTop() {
            root.raise()
            if (root.flags & Qt.WindowStaysOnTopHint) {
                root.flags &= ~Qt.WindowStaysOnTopHint;
            } else {
                root.flags |= Qt.WindowStaysOnTopHint;
            }
        }

        function onSignalQuit() {
            quitApp();
        }

        function onSignalIconActivated() {
           showWindow();
       }
    }

    // Screen saver - enable & disable
    ScreenSaver {
        id: screenSaver
        property bool disabled: false
    }
    Timer {
        id: timerScreensaver
        interval: 300000
        running: false
        onTriggered: function () { shouldDisableScreensaver(isPlayerPlaying()) }
    }

    Clipboard {
        id: clipboard
    }

    Process {
        id: streamingServer
        property string errMessage:
            "Error while starting streaming server. Please try to restart stremio. If it happens again please contact the Stremio support team for assistance"
        property int errors: 0
        property bool fastReload: false

        onStarted: function() { stayAliveStreamingServer.stop() }
        onFinished: function(code, status) {
            if (!streamingServer.fastReload && errors < 5 && (code !== 0 || status !== 0) && !root.quitting) {
                transport.queueEvent("server-crash", {"code": code, "log": streamingServer.getErrBuff()});
                errors++
                showStreamingServerErr(code)
            }

            if (streamingServer.fastReload) {
                console.log("Streaming server: performing fast re-load")
                streamingServer.fastReload = false
                root.launchServer()
            } else {
                stayAliveStreamingServer.start()
            }
        }
        onAddressReady: function (address) {
            transport.serverAddress = address
            transport.event("server-address", address)
        }
        onErrorThrown: function (error) {
            if (root.quitting) return;
            if (streamingServer.fastReload && error == 1) return;
            transport.queueEvent("server-crash", {"code": error, "log": streamingServer.getErrBuff()});
            showStreamingServerErr(error)
       }
    }
    function showStreamingServerErr(code) {
        errorDialog.text = streamingServer.errMessage
        errorDialog.detailedText = 'Stremio streaming server has thrown an error \nQProcess::ProcessError code: '
            + code + '\n\n'
            + streamingServer.getErrBuff();
        errorDialog.visible = true
    }
    function launchServer() {
        var node_executable = applicationDirPath + "/node"
        if (Qt.platform.os === "windows") node_executable = applicationDirPath + "/stremio-runtime.exe"
        streamingServer.start(node_executable,
            [applicationDirPath +"/server.js"].concat(Qt.application.arguments.slice(1)),
            "EngineFS server started at "
        )
    }
    Timer {
        id: stayAliveStreamingServer
        interval: 10000
        running: false
        onTriggered: function () { root.launchServer() }
    }

    // Player
    MpvObject {
        id: mpv
        anchors.fill: parent
        onMpvEvent: function(ev, args) { transport.event(ev, args) }
    }

    // Main UI (via WebEngineView)
    function getWebUrl() {
        var params = "?loginFlow=desktop"
        var args = Qt.application.arguments
        var shortVer = Qt.application.version.split('.').slice(0, 2).join('.')

        var webuiArg = "--webui-url="
        for (var i=0; i!=args.length; i++) {
            if (args[i].indexOf(webuiArg) === 0) return args[i].slice(webuiArg.length)
        }

        if (args.indexOf("--development") > -1 || debug)
            return "http://127.0.0.1:11470/#"+params

        if (args.indexOf("--staging") > -1)
            return "https://staging.strem.io/#"+params

        return "https://app.strem.io/shell-v"+shortVer+"/#"+params;
    }

    Timer {
        id: retryTimer
        interval: 1000
        running: false
        onTriggered: function () {
            webView.tries++
            webView.url = webView.mainUrl;
        }
    }
    function injectJS() {
        splashScreen.visible = false
        pulseOpacity.running = false
        removeSplashTimer.running = false
        webView.webChannel.registerObject( 'transport', transport )
        var injectedJS = "try { initShellComm() } " +
                "catch(e) { setTimeout(function() { throw e }); e.message || JSON.stringify(e) }"
        webView.runJavaScript(injectedJS, function(err) {
            if (!err) {
                webView.tries = 0
            } else {
                errorDialog.text = "User Interface could not be loaded.\n\nPlease try again later or contact the Stremio support team for assistance."
                errorDialog.detailedText = err
                errorDialog.visible = true

                console.error(err)
            }
        });
    }

    Timer {
        id: removeSplashTimer
        interval: 90000
        running: true
        repeat: false
        onTriggered: function () {
            injectJS()
        }
    }

    WebEngineView {
        id: webView
        focus: true

        readonly property string mainUrl: getWebUrl()

        url: webView.mainUrl
        anchors.fill: parent
        backgroundColor: "transparent"
        property int tries: 0
        readonly property int maxTries: 20

        Component.onCompleted: function() {
            console.log("Loading web UI from URL: "+webView.mainUrl)
            webView.profile.httpUserAgent = webView.profile.httpUserAgent + ' StremioShell/' + Qt.application.version
            webView.profile.httpCacheMaximumSize = 209715200 // 200 MB
        }

        onLoadingChanged: function(loadRequest) {

            var successfullyLoaded = loadRequest.status === WebEngineView.LoadSucceededStatus
            if (successfullyLoaded || webView.tries > 0) {
                splashScreen.visible = false
                pulseOpacity.running = false
            }

            if (successfullyLoaded) {
                injectJS()
            }

            var shouldRetry = loadRequest.status === WebEngineView.LoadFailedStatus ||
                              loadRequest.status === WebEngineView.LoadStoppedStatus
            if (shouldRetry && webView.tries < webView.maxTries) {
                retryTimer.restart()
            }
        }

        onRenderProcessTerminated: function(terminationStatus, exitCode) {
            console.log("render process terminated with code " + exitCode + " and status: " + terminationStatus)
            webView.backgroundColor = "black"
            retryTimer.restart()

            transport.queued = []
            transport.queueEvent("render-process-terminated", { exitCode: exitCode, terminationStatus: terminationStatus, url: webView.url })
        }

        property string hoveredUrl: ""
        onLinkHovered: webView.hoveredUrl = hoveredUrl

        onFullScreenRequested: function(req) {
            setFullScreen(req.toggleOn)
            req.accept()
        }

        onNavigationRequested: function(req) {
            let url = req.url.toString();
            let allowedHost = webView.mainUrl.split('/')[2];
            let targetHost = url.split('/')[2];

            if (allowedHost != targetHost && (req.isMainFrame || targetHost !== 'www.youtube.com')) {
                console.log("Disallowed navigation to " + url);
                Qt.openUrlExternally(url); // NEW: open externally
                req.action = WebEngineView.IgnoreRequest;
            }
        }


        Menu {
            id: ctxMenu
            MenuItem {
                text: "Undo"
                onTriggered: webView.triggerWebAction(WebEngineView.Undo)
            }
            MenuItem {
                text: "Redo"
                onTriggered: webView.triggerWebAction(WebEngineView.Redo)
            }
            MenuSeparator { }
            MenuItem {
                text: "Cut"
                onTriggered: webView.triggerWebAction(WebEngineView.Cut)
            }
            MenuItem {
                text: "Copy"
                onTriggered: webView.triggerWebAction(WebEngineView.Copy)
            }
            MenuItem {
                text: "Paste"
                onTriggered: webView.triggerWebAction(WebEngineView.Paste)
            }
            MenuSeparator { }
            MenuItem {
                text: "Select All"
                onTriggered: webView.triggerWebAction(WebEngineView.SelectAll)
            }
        }

        onContextMenuRequested: function(request) {
            request.accepted = true;
            if (request.isContentEditable) {
                ctxMenu.popup();
            }
        }

        Shortcut {
            sequence: StandardKey.Paste
            onActivated: webView.triggerWebAction(WebEngineView.Paste)
        }

        DropArea {
            anchors.fill: parent
            onDropped: function(dropargs){
                var args = JSON.parse(JSON.stringify(dropargs))
                transport.event("dragdrop", args.urls)
            }
        }
        webChannel: wChannel
    }

    WebChannel {
        id: wChannel
    }

    Rectangle {
        id: splashScreen;
        color: "#0c0b11";
        anchors.fill: parent;
        Image {
            id: splashLogo
            source: "qrc:///images/stremio.png"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter

            SequentialAnimation {
                id: pulseOpacity
                running: true
                NumberAnimation { target: splashLogo; property: "opacity"; to: 1.0; duration: 600; easing.type: Easing.Linear }
                NumberAnimation { target: splashLogo; property: "opacity"; to: 0.3; duration: 600; easing.type: Easing.Linear }
                loops: Animation.Infinite
            }
        }
    }

    MessageDialog {
        id: errorDialog
        title: "Stremio - Application Error"
    }

    FileDialog {
        id: fileDialog
        onAccepted: {
            var fileProtocol = "file://"
            var onWindows = Qt.platform.os === "windows" ? 1 : 0
            var pathSeparators = ["/", "\\"]
            var files = fileDialog.fileUrls.filter(function(fileUrl) {
                return fileUrl.startsWith(fileProtocol)
            })
            .map(function(fileUrl) {
                return decodeURIComponent(fileUrl.substring(fileProtocol.length + onWindows)).replace(/\//g, pathSeparators[onWindows])
            })
            transport.event("file-selected", {
                files: files,
                title: fileDialog.title,
                selectExisting: fileDialog.selectExisting,
                selectFolder: fileDialog.selectFolder,
                selectMultiple: fileDialog.selectMultiple,
                nameFilters: fileDialog.nameFilters,
                selectedNameFilter: fileDialog.selectedNameFilter,
                data: fileDialog.dialogData
            })
        }
        onRejected: {
            transport.event("file-rejected", {
                title: fileDialog.title,
                selectExisting: fileDialog.selectExisting,
                selectFolder: fileDialog.selectFolder,
                selectMultiple: fileDialog.selectMultiple,
                nameFilters: fileDialog.nameFilters,
                selectedNameFilter: fileDialog.selectedNameFilter,
                data: fileDialog.dialogData
            })
        }
        property var dialogData: {}
    }

    onWindowStateChanged: function(state) {
        updatePreviousVisibility();
        transport.event("win-state-changed", { state: state })
    }

    onVisibilityChanged: {
        var enabledAlwaysOnTop = root.visible && root.visibility != Window.FullScreen;
        systemTray.alwaysOnTopEnabled(enabledAlwaysOnTop);
        if (!enabledAlwaysOnTop) {
            root.flags &= ~Qt.WindowStaysOnTopHint;
        }
        updatePreviousVisibility();
        transport.event("win-visibility-changed", { visible: root.visible, visibility: root.visibility,
            isFullscreen: root.visibility === Window.FullScreen })
    }

    property int appState: Qt.application.state;
    onAppStateChanged: {
        var clipboardUrl;
        if (clipboard.text.match(/^(magnet|http|https|file|stremio|ipfs):/))
            clipboardUrl = clipboard.text
        transport.event("app-state-changed", { state: appState, clipboard: clipboardUrl })

        if (Qt.platform.os === "osx" && appState === Qt.ApplicationActive && !root.visible) {
            root.show()
        }
    }

    onClosing: function(event) {
        event.accepted = false
        root.hide()
    }

    signal autoUpdaterErr(var msg, var err);
    signal autoUpdaterRestartTimer();

    Timer {
        id: autoUpdaterLongTimer
        interval: 2 * 60 * 60 * 1000
        running: false
        onTriggered: function() { autoUpdaterShortTimer.restart() }
    }
    Timer {
        id: autoUpdaterShortTimer
        interval: 5 * 60 * 1000
        running: false
        onTriggered: function() { } // set dynamically by Autoupdater.initAutoUpdater()
    }

    Component.onCompleted: function() {
        console.log('Stremio Shell version: ' + Qt.application.version)

        root.height = root.initialHeight
        root.width = root.initialWidth

        var args = Qt.application.arguments
        if (args.indexOf("--development") > -1 && args.indexOf("--streaming-server") === -1)
            console.log("Skipping launch of streaming server under --development");
        else
            launchServer();

        var lastArg = args[1];
        if (args.length > 1 && !lastArg.match('^--'))
            onAppOpenMedia(lastArg)

        console.info(" **** Completed. Loading Autoupdater ***")
        Autoupdater.initAutoUpdater(autoUpdater, root.autoUpdaterErr, autoUpdaterShortTimer, autoUpdaterLongTimer, autoUpdaterRestartTimer, webView.profile.httpUserAgent);
    }
}
