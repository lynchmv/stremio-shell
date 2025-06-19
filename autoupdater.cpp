#include <autoupdater.h>
#ifdef Q_OS_MACOS
#include <sys/types.h>
#include <sys/sysctl.h>
#include <QDebug>
#endif

AutoUpdater::AutoUpdater() : manager(new QNetworkAccessManager(this)) {
    init_public_key();
}

// HANDLE FATAL ERRORS
void AutoUpdater::emitFatalError(QString msg, QVariant err) {
    this->abort();
    emit error(msg, err);
}

// IS INSTALLED?
bool AutoUpdater::isInstalled() {
    QString dirPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    if (env.contains("LOCALAPPDATA") && dirPath.startsWith(env.value("LOCALAPPDATA"))) return true;
    if (env.contains("ProgramFiles") && dirPath.startsWith(env.value("ProgramFiles"))) return true;
    if (env.contains("ProgramFiles(x86)") && dirPath.startsWith(env.value("ProgramFiles(x86)"))) return true;

    if (dirPath.contains("/Applications") && dirPath.contains(".app")) return true;

#ifdef Q_OS_MACOS
    int ret = 0;
    size_t size = sizeof(ret);
    if (sysctlbyname("sysctl.proc_translated", &ret, &size, nullptr, 0) >= 0 && ret) {
        QDir pathDir("/Applications/Stremio.app");
        qDebug() << "AUTOUPDATER: Installed on Rosetta!";
        return pathDir.exists();
    }
#endif

    if (dirPath.startsWith("/tmp/.mount_")) return true;

    return false;
}

// WRAPPERS for public slots
void AutoUpdater::checkForUpdates(QString endpoint, QString userAgent) {
    if (inProgress) return;
    inProgress = true;
    QMetaObject::invokeMethod(this, "checkForUpdatesPerform", Qt::QueuedConnection,
                              Q_ARG(QString, endpoint), Q_ARG(QString, userAgent));
}

void AutoUpdater::updateFromVersionDesc(QUrl versionDesc, QByteArray base64Sig) {
    if (inProgress) return;
    inProgress = true;
    QMetaObject::invokeMethod(this, "updateFromVersionDescPerform", Qt::QueuedConnection,
                              Q_ARG(QUrl, versionDesc), Q_ARG(QByteArray, base64Sig));
}

void AutoUpdater::abort() {
    QMetaObject::invokeMethod(this, "abortPerform", Qt::QueuedConnection);
}

// SETTINGS
void AutoUpdater::setForceFullUpdate(bool force) {
    forceFullUpdate = force;
}

// UTILS
bool AutoUpdater::moveFileToAppDir(QString from) {
    QDir dir;
    QFileInfo oldFile(from);
    QString dest = QCoreApplication::applicationDirPath() + QDir::separator() + oldFile.fileName();

    if (!QFile::exists(from)) return false;
    if (QFile::exists(dest) && !QFile::remove(dest)) return false;

    return dir.rename(from, dest);
}

int AutoUpdater::executeCmd(QString cmd, QStringList args, bool noWait) {
    QProcess proc;
    proc.setProcessChannelMode(QProcess::ForwardedChannels);

    if (noWait) {
        proc.startDetached(cmd, args);
        return -1;
    }

    proc.start(cmd, args);
    if (!proc.waitForFinished(5 * 60 * 1000)) return -1;

    return proc.exitCode();
}

// CHECK FOR UPDATES
void AutoUpdater::checkForUpdatesPerform(QString endpoint, QString userAgent) {
    QByteArray serverHash = getFileChecksum(QCoreApplication::applicationDirPath() + QDir::separator() + SERVER_FNAME);
    QByteArray asarHash = getFileChecksum(QCoreApplication::applicationDirPath() + QDir::separator() + ASAR_FNAME);

    QUrl url(endpoint);
    QUrlQuery query(url);

    query.addQueryItem("serverSum", serverHash.toHex());
    query.addQueryItem("asarSum", asarHash.toHex());
    query.addQueryItem("shellVersion", QCoreApplication::applicationVersion());

    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent.toUtf8());

    currentCheck = manager->get(request);
    QObject::connect(currentCheck, &QNetworkReply::finished, this, &AutoUpdater::checkForUpdatesFinished);
}

void AutoUpdater::checkForUpdatesFinished() {
    if (currentCheck == nullptr) {
        emitFatalError("internal error - currentCheck nullptr on checkForUpdatesFinished");
        return;
    }

    QNetworkReply* reply = currentCheck;
    reply->deleteLater();
    currentCheck = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonParseError error;
        QJsonDocument jsonResponse = QJsonDocument::fromJson(reply->readAll(), &error);

        if (error.error == QJsonParseError::NoError) {
            emit checkFinished(jsonResponse.toVariant());

            if (jsonResponse.isObject()) {
                QJsonObject obj = jsonResponse.object();
                if (obj.value("upToDate").toBool()) {
                    inProgress = false;
                } else {
                    updateFromVersionDescPerform(
                        QUrl(obj.value("versionDesc").toString()),
                        QByteArray::fromBase64(obj.value("signature").toString().toUtf8())
                    );
                }
            } else {
                emitFatalError("Unable to understand response from checkForUpdates");
            }
        } else {
            emitFatalError("JSON parse error on checkForUpdates " + error.errorString());
        }
    } else if (reply->error() != QNetworkReply::OperationCanceledError) {
        emitFatalError("Network error on checkForUpdates " + reply->url().toString(), reply->error());
    }
}

// GET & VERIFY VERSION DESC
void AutoUpdater::updateFromVersionDescPerform(QUrl versionDesc, QByteArray base64Sig) {
    currentCheck = manager->get(QNetworkRequest(versionDesc));
    currentCheck->setProperty("signature", base64Sig);
    QObject::connect(currentCheck, &QNetworkReply::finished, this, &AutoUpdater::updateFromVersionDescFinished);
}

void AutoUpdater::updateFromVersionDescFinished() {
    if (currentCheck == nullptr) {
        emitFatalError("internal error - currentCheck nullptr on updateFromVersionDescFinished");
        return;
    }

    QNetworkReply* reply = currentCheck;
    reply->deleteLater();
    currentCheck = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray dataReply = reply->readAll();
        QByteArray sig = reply->property("signature").toByteArray();

        if (verify_sig(
            reinterpret_cast<const byte*>(dataReply.data()), dataReply.size(),
            reinterpret_cast<const byte*>(sig.data()), sig.length()) != 0) {
            emitFatalError("Unable to verify update signature");
        } else {
            QJsonParseError error;
            QJsonDocument jsonResponse = QJsonDocument::fromJson(dataReply, &error);

            if (error.error == QJsonParseError::NoError && jsonResponse.isObject()) {
                prepareUpdate(jsonResponse);
            } else if (error.error != QJsonParseError::NoError) {
                emitFatalError("JSON parse error on updateFromVersionDesc " + error.errorString());
            } else {
                emitFatalError("Unable to understand response from updateFromVersionDesc");
            }
        }
    } else if (reply->error() != QNetworkReply::OperationCanceledError) {
        emitFatalError("Network error on updateFromVersionDesc " + reply->url().toString(), reply->error());
    }
}

void AutoUpdater::prepareUpdate(QJsonDocument versionDescDoc) {
    currentVersionDesc = versionDescDoc;

    QJsonObject versionDesc = versionDescDoc.object();
    QJsonObject files = versionDesc.value("files").toObject();

    QVector<QString> toDownload;
    if (forceFullUpdate || versionDesc.value("shellVersion").toString() != QCoreApplication::applicationVersion()) {
        toDownload = FULL_UPDATE_FILES;
    } else {
        toDownload = PARTIAL_UPDATE_FILES;
    }

    if (toDownload.isEmpty()) {
        emitFatalError("internal error - no files to download. Unsupported OS?");
        return;
    }

    for (const QString& prop : toDownload) {
        QJsonObject file = files.value(prop).toObject();
        if (!(file.contains("url") && file.contains("checksum"))) continue;

        enqueueDownload(
            QUrl(file.value("url").toString()),
            QByteArray::fromHex(file.value("checksum").toString().toUtf8())
        );
    }

    startNextDownload();
}

// DOWNLOAD
QByteArray AutoUpdater::getFileChecksum(QString path) {
    QCryptographicHash crypto(QCryptographicHash::Sha256);
    QFile file(path);
    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Failed to open file for checksum:" << path;
        return QByteArray();
    }
    while (!file.atEnd()) {
        crypto.addData(file.read(FILE_READ_CHUNK));
    }
    return crypto.result();
}

void AutoUpdater::enqueueDownload(QUrl from, QByteArray checksum) {
    downloadQueue.enqueue(fDownload(from, checksum));
}

void AutoUpdater::startNextDownload() {
    if (downloadQueue.isEmpty()) {
        inProgress = false;
        emit prepared(preparedFiles, QVariant(currentVersionDesc.object()));
        return;
    }

    fDownload next = downloadQueue.dequeue();
    QUrl url = next.first;
    QByteArray checksum = next.second;

    QString dest = QDir::tempPath() + QDir::separator() + url.fileName();

    if (checksum == getFileChecksum(dest)) {
        preparedFiles.push_back(dest);
        startNextDownload();
        return;
    }

    output.setFileName(dest);
    if (!output.open(QIODevice::WriteOnly)) {
        emitFatalError("error opening file " + dest + " for download: " + output.errorString());
        return;
    }

    currentDownload = manager->get(QNetworkRequest(url));
    currentDownload->setProperty("checksum", checksum);
    QObject::connect(currentDownload, &QNetworkReply::readyRead, this, &AutoUpdater::downloadReadyRead);
    QObject::connect(currentDownload, &QNetworkReply::finished, this, &AutoUpdater::downloadFinished);
}

void AutoUpdater::downloadReadyRead() {
    output.write(currentDownload->readAll());
}

void AutoUpdater::downloadFinished() {
    output.close();

    if (currentDownload == nullptr) {
        emitFatalError("internal error - currentDownload nullptr on downloadFinished");
        return;
    }

    QNetworkReply* reply = currentDownload;
    reply->deleteLater();
    currentDownload = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QString dest = output.fileName();
        QByteArray checksum = reply->property("checksum").toByteArray();

        if (checksum == getFileChecksum(dest)) {
            preparedFiles.push_back(dest);
            startNextDownload();
        } else {
            emitFatalError("Unable to verify checksum for file " + dest);
        }
    } else if (reply->error() != QNetworkReply::OperationCanceledError) {
        emitFatalError("Network error on downloadFinished " + reply->url().toString(), reply->error());
    }
}

// ABORT
void AutoUpdater::abortPerform() {
    if (currentCheck) currentCheck->abort();
    if (currentDownload) currentDownload->abort();

    currentVersionDesc = QJsonDocument();
    downloadQueue.clear();
    preparedFiles.clear();

    output.close();
    inProgress = false;
}

