#include "connection.h"
#include "mainwindow.h"
#include "settings.h"
#include "ui_connection.h"
#include "rpc.h"

#include "precompiled.h"

using json = nlohmann::json;

ConnectionLoader::ConnectionLoader(MainWindow* main, RPC* rpc) {
    this->main = main;
    this->rpc  = rpc;

    d = new QDialog(main);
    connD = new Ui_ConnectionDialog();
    connD->setupUi(d);
    QPixmap logo(":/img/res/logobig.gif");
    connD->topIcon->setBasePixmap(logo.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

ConnectionLoader::~ConnectionLoader() {    
    delete d;
    delete connD;
}

void ConnectionLoader::loadConnection() {
    QTimer::singleShot(1, [=]() { this->doAutoConnect(); });
    d->exec();
}

void ConnectionLoader::doAutoConnect(bool tryEmoonroomcashdStart) {
    // Priority 1: Ensure all params are present.
    if (!verifyParams()) {
        downloadParams([=]() { this->doAutoConnect(); });
        return;
    }

    // Priority 2: Try to connect to detect moonroomcash.conf and connect to it.
    auto config = autoDetectMoonroomcashConf();
    main->logger->write("Attempting autoconnect");

    if (config.get() != nullptr) {
        auto connection = makeConnection(config);

        refreshMoonroomcashdState(connection, [=] () {
            // Refused connection. So try and start embedded moonroomcashd
            if (Settings::getInstance()->useEmbedded()) {
                if (tryEmoonroomcashdStart) {
                    this->showInformation("Starting embedded moonroomcashd");
                    if (this->startEmbeddedMoonroomcashd()) {
                        // Embedded moonroomcashd started up. Wait a second and then refresh the connection
                        main->logger->write("Embedded moonroomcashd started up, trying autoconnect in 1 sec");
                        QTimer::singleShot(1000, [=]() { doAutoConnect(); } );
                    } else {
                        if (config->moonroomcashDaemon) {
                            // moonroomcashd is configured to run as a daemon, so we must wait for a few seconds
                            // to let it start up. 
                            main->logger->write("moonroomcashd is daemon=1. Waiting for it to start up");
                            this->showInformation("moonroomcashd is set to run as daemon", "Waiting for moonroomcashd");
                            QTimer::singleShot(5000, [=]() { doAutoConnect(/* don't attempt to start emoonroomcashd */ false); });
                        } else {
                            // Something is wrong. 
                            // We're going to attempt to connect to the one in the background one last time
                            // and see if that works, else throw an error
                            main->logger->write("Unknown problem while trying to start moonroomcashd");
                            QTimer::singleShot(2000, [=]() { doAutoConnect(/* don't attempt to start emoonroomcashd */ false); });
                        }
                    }
                } else {
                    // We tried to start emoonroomcashd previously, and it didn't work. So, show the error. 
                    main->logger->write("Couldn't start embedded moonroomcashd for unknown reason");
                    QString explanation;
                    if (config->moonroomcashDaemon) {
                        explanation = QString() % "You have moonroomcashd set to start as a daemon, which can cause problems "
                            "with mrc-qt-wallet\n\n."
                            "Please remove the following line from your moonroomcash.conf and restart mrc-qt-wallet\n"
                            "daemon=1";
                    } else {
                        explanation = QString() % "Couldn't start the embedded moonroomcashd.\n\n" %
                            "Please try restarting.\n\nIf you previously started moonroomcashd with custom arguments, you might need to reset moonroomcash.conf.\n\n" %
                            "If all else fails, please run moonroomcashd manually." %
                            (emoonroomcashd ? "The process returned:\n\n" % emoonroomcashd->errorString() : QString(""));
                    }
                    
                    this->showError(explanation);
                }                
            } else {
                // moonroomcash.conf exists, there's no connection, and the user asked us not to start moonroomcashd. Error!
                main->logger->write("Not using embedded and couldn't connect to moonroomcashd");
                QString explanation = QString() % "Couldn't connect to moonroomcashd configured in moonroomcash.conf.\n\n" %
                                      "Not starting embedded moonroomcashd because --no-embedded was passed";
                this->showError(explanation);
            }
        });
    } else {
        if (Settings::getInstance()->useEmbedded()) {
            // moonroomcash.conf was not found, so create one
            createMoonroomcashConf();
        } else {
            // Fall back to manual connect
            doManualConnect();
        }
    } 
}

QString randomPassword() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    const int passwordLength = 10;
    char* s = new char[passwordLength + 1];

    for (int i = 0; i < passwordLength; ++i) {
        s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    }

    s[passwordLength] = 0;
    return QString::fromStdString(s);
}

/**
 * This will create a new moonroomcash.conf, download Moonroomcash parameters.
 */ 
void ConnectionLoader::createMoonroomcashConf() {
    main->logger->write("createMoonroomcashConf");

    auto confLocation = moonroomcashConfWritableLocation();
    main->logger->write("Creating file " + confLocation);

    QFileInfo fi(confLocation);
    QDir().mkdir(fi.dir().absolutePath());

    QFile file(confLocation);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        main->logger->write("Could not create moonroomcash.conf, returning");
        return;
    }
        
    QTextStream out(&file); 
    
    out << "server=1\n";
    out << "addnode=178.128.104.155\n";
    out << "rpcuser=mrc-qt-wallet\n";
    out << "rpcpassword=" % randomPassword() << "\n";
    file.close();

    // Now that moonroomcash.conf exists, try to autoconnect again
    this->doAutoConnect();
}


void ConnectionLoader::downloadParams(std::function<void(void)> cb) {    
    main->logger->write("Adding params to download queue");
    // Add all the files to the download queue
    downloadQueue = new QQueue<QUrl>();
    client = new QNetworkAccessManager(main);   
    
       
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sprout-proving.key"));
    downloadQueue->enqueue(QUrl("https://z.cash/downloads/sprout-verifying.key"));
    

    doNextDownload(cb);    
}

void ConnectionLoader::doNextDownload(std::function<void(void)> cb) {
    auto fnSaveFileName = [&] (QUrl url) {
        QString path = url.path();
        QString basename = QFileInfo(path).fileName();

        return basename;
    };

    if (downloadQueue->isEmpty()) {
        delete downloadQueue;
        client->deleteLater();

        main->logger->write("All Downloads done");
        this->showInformation("All Downloads Finished Successfully!");
        cb();
        return;
    }

    QUrl url = downloadQueue->dequeue();
    int filesRemaining = downloadQueue->size();

    QString filename = fnSaveFileName(url);
    QString paramsDir = moonroomcashParamsDir();

    if (QFile(QDir(paramsDir).filePath(filename)).exists()) {
        main->logger->write(filename + " already exists, skipping");
        doNextDownload(cb);

        return;
    }

    // The downloaded file is written to a new name, and then renamed when the operation completes.
    currentOutput = new QFile(QDir(paramsDir).filePath(filename + ".part"));   

    if (!currentOutput->open(QIODevice::WriteOnly)) {
        main->logger->write("Couldn't open " + currentOutput->fileName() + " for writing");
        this->showError("Couldn't download params. Please check the help site for more info.");
    }
    main->logger->write("Downloading to " + filename);
    qDebug() << "Downloading " << url << " to " << filename;
    
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    currentDownload = client->get(request);
    downloadTime.start();
    
    // Download Progress
    QObject::connect(currentDownload, &QNetworkReply::downloadProgress, [=] (auto done, auto total) {
        // calculate the download speed
        double speed = done * 1000.0 / downloadTime.elapsed();
        QString unit;
        if (speed < 1024) {
            unit = "bytes/sec";
        } else if (speed < 1024*1024) {
            speed /= 1024;
            unit = "kB/s";
        } else {
            speed /= 1024*1024;
            unit = "MB/s";
        }

        this->showInformation(
            "Downloading " % filename % (filesRemaining > 1 ? " ( +" % QString::number(filesRemaining)  % " more remaining )" : QString("")),
            QString::number(done/1024/1024, 'f', 0) % "MB of " % QString::number(total/1024/1024, 'f', 0) + "MB at " % QString::number(speed, 'f', 2) % unit);
    });
    
    // Download Finished
    QObject::connect(currentDownload, &QNetworkReply::finished, [=] () {
        // Rename file
        main->logger->write("Finished downloading " + filename);
        currentOutput->rename(QDir(paramsDir).filePath(filename));

        currentOutput->close();
        currentDownload->deleteLater();
        currentOutput->deleteLater();

        if (currentDownload->error()) {
            main->logger->write("Downloading " + filename + " failed");
            this->showError("Downloading " + filename + " failed. Please check the help site for more info");                
        } else {
            doNextDownload(cb);
        }
    });

    // Download new data available. 
    QObject::connect(currentDownload, &QNetworkReply::readyRead, [=] () {
        currentOutput->write(currentDownload->readAll());
    });    
}

bool ConnectionLoader::startEmbeddedMoonroomcashd() {
    if (!Settings::getInstance()->useEmbedded()) 
        return false;
    
    main->logger->write("Trying to start embedded moonroomcashd");

    // Static because it needs to survive even after this method returns.
    static QString processStdErrOutput;

    if (emoonroomcashd != nullptr) {
        if (emoonroomcashd->state() == QProcess::NotRunning) {
            if (!processStdErrOutput.isEmpty()) {
                QMessageBox::critical(main, "moonroomcashd error", "moonroomcashd said: " + processStdErrOutput, 
                                      QMessageBox::Ok);
            }
            return false;
        } else {
            return true;
        }        
    }

    // Finally, start moonroomcashd    
    QDir appPath(QCoreApplication::applicationDirPath());
#ifdef Q_OS_LINUX
    auto moonroomcashdProgram = appPath.absoluteFilePath("mqw-moonroomcashd");
    if (!QFile(moonroomcashdProgram).exists()) {
        moonroomcashdProgram = appPath.absoluteFilePath("moonroomcashd");
    }
#elif defined(Q_OS_DARWIN)
    auto moonroomcashdProgram = appPath.absoluteFilePath("moonroomcashd");
#else
    auto moonroomcashdProgram = appPath.absoluteFilePath("moonroomcashd.exe");
#endif
    
    if (!QFile(moonroomcashdProgram).exists()) {
        qDebug() << "Can't find moonroomcashd at " << moonroomcashdProgram;
        main->logger->write("Can't find moonroomcashd at " + moonroomcashdProgram); 
        return false;
    }

    emoonroomcashd = new QProcess(main);    
    QObject::connect(emoonroomcashd, &QProcess::started, [=] () {
        //qDebug() << "moonroomcashd started";
    });

    QObject::connect(emoonroomcashd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                        [=](int, QProcess::ExitStatus) {
        //qDebug() << "moonroomcashd finished with code " << exitCode << "," << exitStatus;    
    });

    QObject::connect(emoonroomcashd, &QProcess::errorOccurred, [&] (auto) {
        //qDebug() << "Couldn't start moonroomcashd: " << error;
    });

    QObject::connect(emoonroomcashd, &QProcess::readyReadStandardError, [=]() {
        auto output = emoonroomcashd->readAllStandardError();
       main->logger->write("moonroomcashd stderr:" + output);
        processStdErrOutput.append(output);
    });

#ifdef Q_OS_LINUX
    emoonroomcashd->start(moonroomcashdProgram);
#elif defined(Q_OS_DARWIN)
    emoonroomcashd->start(moonroomcashdProgram);
#else
    emoonroomcashd->setWorkingDirectory(appPath.absolutePath());
    emoonroomcashd->start("moonroomcashd.exe");
#endif // Q_OS_LINUX


    return true;
}

void ConnectionLoader::doManualConnect() {
    auto config = loadFromSettings();

    if (!config) {
        // Nothing configured, show an error
        QString explanation = QString()
                % "A manual connection was requested, but the settings are not configured.\n\n" 
                % "Please set the host/port and user/password in the Edit->Settings menu.";

        showError(explanation);
        doRPCSetConnection(nullptr);

        return;
    }

    auto connection = makeConnection(config);
    refreshMoonroomcashdState(connection, [=] () {
        QString explanation = QString()
                % "Could not connect to moonroomcashd configured in settings.\n\n" 
                % "Please set the host/port and user/password in the Edit->Settings menu.";

        showError(explanation);
        doRPCSetConnection(nullptr);

        return;
    });
}

void ConnectionLoader::doRPCSetConnection(Connection* conn) {
    rpc->setEMoonroomcashd(emoonroomcashd);
    rpc->setConnection(conn);
    
    d->accept();

    delete this;
}

Connection* ConnectionLoader::makeConnection(std::shared_ptr<ConnectionConfig> config) {
    QNetworkAccessManager* client = new QNetworkAccessManager(main);
         
    QUrl myurl;
    myurl.setScheme("http");
    myurl.setHost(config.get()->host);
    myurl.setPort(config.get()->port.toInt());

    QNetworkRequest* request = new QNetworkRequest();
    request->setUrl(myurl);
    request->setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    
    QString userpass = config.get()->rpcuser % ":" % config.get()->rpcpassword;
    QString headerData = "Basic " + userpass.toLocal8Bit().toBase64();
    request->setRawHeader("Authorization", headerData.toLocal8Bit());    

    return new Connection(main, client, request, config);
}

void ConnectionLoader::refreshMoonroomcashdState(Connection* connection, std::function<void(void)> refused) {
    json payload = {
        {"jsonrpc", "1.0"},
        {"id", "someid"},
        {"method", "getinfo"}
    };
    connection->doRPC(payload,
        [=] (auto) {
            // Success, hide the dialog if it was shown. 
            d->hide();
            this->doRPCSetConnection(connection);
        },
        [=] (auto reply, auto res) {            
            // Failed, see what it is. 
            auto err = reply->error();
            //qDebug() << err << ":" << QString::fromStdString(res.dump());

            if (err == QNetworkReply::NetworkError::ConnectionRefusedError) {   
                refused();
            } else if (err == QNetworkReply::NetworkError::AuthenticationRequiredError) {
                main->logger->write("Authentication failed");
                QString explanation = QString() 
                        % "Authentication failed. The username / password you specified was "
                        % "not accepted by moonroomcashd. Try changing it in the Edit->Settings menu";

                this->showError(explanation);
            } else if (err == QNetworkReply::NetworkError::InternalServerError && 
                    !res.is_discarded()) {
                // The server is loading, so just poll until it succeeds
                QString status    = QString::fromStdString(res["error"]["message"]);
                {
                    static int dots = 0;
                    status = status.left(status.length() - 3) + QString(".").repeated(dots);
                    dots++;
                    if (dots > 3)
                        dots = 0;
                }
                this->showInformation("Your moonroomcashd is starting up. Please wait.", status);
                main->logger->write("Waiting for moonroomcashd to come online.");
                // Refresh after one second
                QTimer::singleShot(1000, [=]() { this->refreshMoonroomcashdState(connection, refused); });
            }
        }
    );
}

void ConnectionLoader::showInformation(QString info, QString detail) {
    connD->status->setText(info);
    connD->statusDetail->setText(detail);
}

/**
 * Show error will close the loading dialog and show an error. 
*/
void ConnectionLoader::showError(QString explanation) {    
    rpc->setEMoonroomcashd(nullptr);
    rpc->noConnection();

    QMessageBox::critical(main, "Connection Error", explanation, QMessageBox::Ok);
    d->close();
}

QString ConnectionLoader::locateMoonroomcashConfFile() {
#ifdef Q_OS_LINUX
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, ".moonroomcash/moonroomcash.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QStandardPaths::locate(QStandardPaths::HomeLocation, "Library/Application Support/Moonroomcash/moonroomcash.conf");
#else
    auto confLocation = QStandardPaths::locate(QStandardPaths::AppDataLocation, "../../moonroomcash/moonroomcash.conf");
#endif

    main->logger->write("Found moonroomcashconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::moonroomcashConfWritableLocation() {
#ifdef Q_OS_LINUX
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".moonroomcash/moonroomcash.conf");
#elif defined(Q_OS_DARWIN)
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/Moonroomcash/moonroomcash.conf");
#else
    auto confLocation = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../Moonroomcash/moonroomcash.conf");
#endif

    main->logger->write("Found moonroomcashconf at " + QDir::cleanPath(confLocation));
    return QDir::cleanPath(confLocation);
}

QString ConnectionLoader::moonroomcashParamsDir() {
    #ifdef Q_OS_LINUX
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(".zcash-params"));
#elif defined(Q_OS_DARWIN)
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath("Library/Application Support/ZcashParams"));
#else
    auto paramsLocation = QDir(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("../../ZcashParams"));
#endif

    if (!paramsLocation.exists()) {
        main->logger->write("Creating params location at " + paramsLocation.absolutePath());
        QDir().mkpath(paramsLocation.absolutePath());
    }

    main->logger->write("Found Zcash params directory at " + paramsLocation.absolutePath());
    return paramsLocation.absolutePath();
}

bool ConnectionLoader::verifyParams() {
    QDir paramsDir(moonroomcashParamsDir());

    if (!QFile(paramsDir.filePath("sprout-proving.key")).exists()) return false;
    if (!QFile(paramsDir.filePath("sprout-verifying.key")).exists()) return false;
    
    return true;
}

/**
 * Try to automatically detect a moonroomcash.conf file in the correct location and load parameters
 */ 
std::shared_ptr<ConnectionConfig> ConnectionLoader::autoDetectMoonroomcashConf() {    
    auto confLocation = locateMoonroomcashConfFile();

    if (confLocation.isNull()) {
        // No Moonroomcash file, just return with nothing
        return nullptr;
    }

    QFile file(confLocation);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << file.errorString();
        return nullptr;
    }

    QTextStream in(&file);

    auto moonroomcashconf = new ConnectionConfig();
    moonroomcashconf->host     = "127.0.0.1";
    moonroomcashconf->connType = ConnectionType::DetectedConfExternalMoonroomcashD;
    moonroomcashconf->usingMoonroomcashConf = true;
    moonroomcashconf->moonroomcashDir = QFileInfo(confLocation).absoluteDir().absolutePath();
    moonroomcashconf->moonroomcashDaemon = false;

    Settings::getInstance()->setUsingMoonroomcashConf(confLocation);

    while (!in.atEnd()) {
        QString line = in.readLine();
        auto s = line.indexOf("=");
        QString name  = line.left(s).trimmed().toLower();
        QString value = line.right(line.length() - s - 1).trimmed();

        if (name == "rpcuser") {
            moonroomcashconf->rpcuser = value;
        }
        if (name == "rpcpassword") {
            moonroomcashconf->rpcpassword = value;
        }
        if (name == "rpcport") {
            moonroomcashconf->port = value;
        }
        if (name == "daemon" && value == "1") {
            moonroomcashconf->moonroomcashDaemon = true;
        }
        if (name == "testnet" &&
            value == "1"  &&
            moonroomcashconf->port.isEmpty()) {
                moonroomcashconf->port = "26224";
        }
    }

    // If rpcport is not in the file, and it was not set by the testnet=1 flag, then go to default
    if (moonroomcashconf->port.isEmpty()) moonroomcashconf->port = "16224";
    file.close();

    // In addition to the moonroomcash.conf file, also double check the params. 

    return std::shared_ptr<ConnectionConfig>(moonroomcashconf);
}

/**
 * Load connection settings from the UI, which indicates an unknown, external moonroomcashd
 */ 
std::shared_ptr<ConnectionConfig> ConnectionLoader::loadFromSettings() {
    // Load from the QT Settings. 
    QSettings s;
    
    auto host        = s.value("connection/host").toString();
    auto port        = s.value("connection/port").toString();
    auto username    = s.value("connection/rpcuser").toString();
    auto password    = s.value("connection/rpcpassword").toString();    

    if (username.isEmpty() || password.isEmpty())
        return nullptr;

    auto uiConfig = new ConnectionConfig{ host, port, username, password, false, false, "",  ConnectionType::UISettingsMoonroomCashD};

    return std::shared_ptr<ConnectionConfig>(uiConfig);
}





/***********************************************************************************
 *  Connection Class
 ************************************************************************************/ 
Connection::Connection(MainWindow* m, QNetworkAccessManager* c, QNetworkRequest* r, 
                        std::shared_ptr<ConnectionConfig> conf) {
    this->restclient  = c;
    this->request     = r;
    this->config      = conf;
    this->main        = m;
}

Connection::~Connection() {
    delete restclient;
    delete request;
}

void Connection::doRPC(const json& payload, const std::function<void(json)>& cb, 
                       const std::function<void(QNetworkReply*, const json&)>& ne) {
    if (shutdownInProgress) {
        // Ignoring RPC because shutdown in progress
        return;
    }

    QNetworkReply *reply = restclient->post(*request, QByteArray::fromStdString(payload.dump()));

    QObject::connect(reply, &QNetworkReply::finished, [=] {
        reply->deleteLater();
        if (shutdownInProgress) {
            // Ignoring callback because shutdown in progress
            return;
        }
        
        if (reply->error() != QNetworkReply::NoError) {
            auto parsed = json::parse(reply->readAll(), nullptr, false);
            ne(reply, parsed);
            
            return;
        } 
        
        auto parsed = json::parse(reply->readAll(), nullptr, false);
        if (parsed.is_discarded()) {
            ne(reply, "Unknown error");
        }
        
        cb(parsed["result"]);        
    });
}

void Connection::doRPCWithDefaultErrorHandling(const json& payload, const std::function<void(json)>& cb) {
    doRPC(payload, cb, [=] (auto reply, auto parsed) {
        if (!parsed.is_discarded() && !parsed["error"]["message"].is_null()) {
            this->showTxError(QString::fromStdString(parsed["error"]["message"]));    
        } else {
            this->showTxError(reply->errorString());
        }
    });
}

void Connection::doRPCIgnoreError(const json& payload, const std::function<void(json)>& cb) {
    doRPC(payload, cb, [=] (auto, auto) {
        // Ignored error handling
    });
}

void Connection::showTxError(const QString& error) {
    if (error.isNull()) return;

    // Prevent multiple dialog boxes from showing, because they're all called async
    static bool shown = false;
    if (shown)
        return;

    shown = true;
    QMessageBox::critical(main, "Transaction Error", "There was an error sending the transaction. The error was: \n\n"
        + error, QMessageBox::StandardButton::Ok);
    shown = false;
}

/**
 * Prevent all future calls from going through
 */ 
void Connection::shutdown() {
    shutdownInProgress = true;
}
