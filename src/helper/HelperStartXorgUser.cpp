/***************************************************************************
* Copyright (c) 2021 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include "Configuration.h"
#include "Constants.h"
#include "SignalHandler.h"
#include "XAuth.h"

#include <unistd.h>

using namespace SDDM;

static const QEvent::Type StartupEventType =
        static_cast<QEvent::Type>(QEvent::registerEventType());

class StartupEvent : public QEvent
{
public:
    StartupEvent()
        : QEvent(StartupEventType)
    {
    }
};

class XOrgUserHelper : public QObject
{
    Q_OBJECT
public:
    explicit XOrgUserHelper(int fd, const QString &serverCmd, const QString &clientCmd,
                            QObject *parent = nullptr)
        : QObject(parent)
        , m_fd(fd)
        , m_serverCmd(serverCmd)
        , m_clientCmd(clientCmd)
    {
        m_xauth.setAuthDirectory(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    }

    bool start()
    {
        // Create xauthority
        m_xauth.setup();

        // Generate xauthority file
        // For the X server's copy, the display number doesn't matter.
        // An empty file would result in no access control!
        if (!m_xauth.addCookie(m_display)) {
            qCritical("Failed to write xauth file");
            return false;
        }

        // Start server process
        if (!startServer())
            return false;

        // Setup display
        startDisplayCommand();

        // Start client process
        if (!startClient())
            return false;

        return true;
    }

    void stop()
    {
        if (m_clientProcess) {
            qInfo("Stopping client...");
            m_clientProcess->terminate();
            if (!m_clientProcess->waitForFinished(5000))
                m_clientProcess->kill();
            m_clientProcess->deleteLater();
            m_clientProcess = nullptr;
        }

        if (m_serverProcess) {
            qInfo("Stopping server...");
            m_serverProcess->terminate();
            if (!m_serverProcess->waitForFinished(5000))
                m_serverProcess->kill();
            m_serverProcess->deleteLater();
            m_serverProcess = nullptr;

            displayFinished();
        }
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == StartupEventType) {
            if (!start())
                QCoreApplication::exit(127);
            return true;
        }

        return QObject::event(event);
    }

private:
    int m_fd = -1;
    QString m_serverCmd;
    QString m_clientCmd;
    QString m_display = QStringLiteral(":0");
    XAuth m_xauth;
    QProcess *m_serverProcess = nullptr;
    QProcess *m_clientProcess = nullptr;

    bool startProcess(const QString &cmd, const QProcessEnvironment &env,
                      QProcess **p = nullptr)
    {
        auto args = QProcess::splitCommand(cmd);
        const auto program = args.takeFirst();

        // Make sure to forward the input of sddm-helper-x11 onto the Xorg
        // server, otherwise it will complain that only console users are allowed
        auto *process = new QProcess(this);
        process->setInputChannelMode(QProcess::ForwardedInputChannel);
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        process->setProcessEnvironment(env);

        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                process, [](int exitCode, QProcess::ExitStatus exitStatus) {
            if (exitCode != 0 || exitStatus != QProcess::NormalExit)
                QCoreApplication::instance()->quit();
        });

        process->start(program, args);
        if (!process->waitForStarted(10000)) {
            qWarning("Failed to start \"%s\": %s",
                     qPrintable(cmd),
                     qPrintable(process->errorString()));
            return false;
        }

        if (p)
            *p = process;

        return true;
    }

    bool startServer()
    {
        // Create pipe for communicating with X server
        // 0 == read from X, 1 == write to X
        int pipeFds[2];
        if (::pipe(pipeFds) != 0) {
            qCritical("Could not create pipe to start X server");
            return false;
        }

        // Server environment
        // Not setting XORG_RUN_AS_USER_OK=1 will make Xorg require root privileges
        // under Fedora and all distros that use their patch.
        // https://src.fedoraproject.org/rpms/xorg-x11-server/blob/rawhide/f/0001-Fedora-hack-Make-the-suid-root-wrapper-always-start-.patch
        // https://fedoraproject.org/wiki/Changes/XorgWithoutRootRights
        QProcessEnvironment serverEnv = QProcessEnvironment::systemEnvironment();
        serverEnv.insert(QStringLiteral("XORG_RUN_AS_USER_OK"), QStringLiteral("1"));

        // Append xauth and display fd to the command
        auto args = QStringList()
                << QStringLiteral("-auth") << m_xauth.authPath()
                << QStringLiteral("-displayfd") << QString::number(pipeFds[1]);

        // Append VT from environment
        args << QStringLiteral("vt%1").arg(serverEnv.value(QStringLiteral("XDG_VTNR")));

        // Log to stdout
        args << QStringLiteral("-logfile") << QStringLiteral("/dev/null");

        // Command string
        m_serverCmd += QLatin1Char(' ') + args.join(QLatin1Char(' '));

        // Start the server process
        qInfo("Running server: %s", qPrintable(m_serverCmd));
        if (!startProcess(m_serverCmd, serverEnv, &m_serverProcess)) {
            ::close(pipeFds[0]);
            return false;
        }

        // Close the other side of pipe in our process, otherwise reading
        // from it may stuck even X server exit
        ::close(pipeFds[1]);

        // Read the display number from the pipe
        QFile readPipe;
        if (!readPipe.open(pipeFds[0], QIODevice::ReadOnly)) {
            qCritical("Failed to open pipe to start X Server");
            ::close(pipeFds[0]);
            return false;
        }
        QByteArray displayNumber = readPipe.readLine();
        if (displayNumber.size() < 2) {
            // X server gave nothing (or a whitespace)
            qCritical("Failed to read display number from pipe");
            ::close(pipeFds[0]);
            return false;
        }
        displayNumber.prepend(QByteArray(":"));
        displayNumber.remove(displayNumber.size() -1, 1); // trim trailing whitespace
        m_display = QString::fromLocal8Bit(displayNumber);
        qDebug("X11 display: %s", qPrintable(m_display));

        // Send the display name to the caller
        if (m_fd > 0)
            ::write(m_fd, displayNumber.constData(), displayNumber.size());

        // Close our pipe
        ::close(pipeFds[0]);

        return true;
    }

    void startDisplayCommand()
    {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DISPLAY"), m_display);
        env.insert(QStringLiteral("XAUTHORITY"), m_xauth.authPath());

        // Set cursor
        qInfo("Setting default cursor...");
        QProcess *setCursor = nullptr;
        if (startProcess(QStringLiteral("xsetroot -cursor_name left_ptr"), env, &setCursor)) {
            if (!setCursor->waitForFinished(1000)) {
                qWarning() << "Could not setup default cursor";
                setCursor->kill();
            }
            setCursor->deleteLater();
        }

        // Display setup script
        auto cmd = mainConfig.X11.DisplayCommand.get();
        qInfo("Running display setup script: %s", qPrintable(cmd));
        QProcess *displayScript = nullptr;
        if (startProcess(cmd, env, &displayScript)) {
            if (!displayScript->waitForFinished(30000))
                displayScript->kill();
            displayScript->deleteLater();
        }
    }

    void displayFinished()
    {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DISPLAY"), m_display);
        env.insert(QStringLiteral("XAUTHORITY"), m_xauth.authPath());

        auto cmd = mainConfig.X11.DisplayStopCommand.get();
        qInfo("Running display stop script: %s", qPrintable(cmd));
        QProcess *displayStopScript = nullptr;
        if (startProcess(cmd, env, &displayStopScript)) {
            if (!displayStopScript->waitForFinished(5000))
                displayStopScript->kill();
            displayStopScript->deleteLater();
        }

        // Remove xauthority file
        QFile::remove(m_xauth.authPath());
    }

    bool startClient()
    {
        // Client environment
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DISPLAY"), m_display);
        env.insert(QStringLiteral("XAUTHORITY"), m_xauth.authPath());

        // Start the client process
        qInfo("Running client: %s", qPrintable(m_clientCmd));
        if (!startProcess(m_clientCmd, env, &m_clientProcess))
            return false;

        return true;
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("sddm-helper-x11"));
    app.setApplicationVersion(QStringLiteral(SDDM_VERSION));
    app.setOrganizationName(QStringLiteral("SDDM"));

    // QCommandLineParser fails to parse arguments like these, so
    // we manually do it ourselves
    const auto args = QCoreApplication::arguments();
    int pos;
    QString fdArg, serverCmd, clientCmd;

    if ((pos = args.indexOf(QStringLiteral("--fd"))) >= 0) {
        if (pos >= args.length() - 1) {
            qCritical("This application is not supposed to be executed manually");
            return 127;
        }
        fdArg = args[pos + 1];
    }

    if ((pos = args.indexOf(QStringLiteral("--server"))) >= 0) {
        if (pos >= args.length() - 1) {
            qCritical("This application is not supposed to be executed manually");
            return 127;
        }
        serverCmd = args[pos + 1];
    }

    if ((pos = args.indexOf(QStringLiteral("--client"))) >= 0) {
        if (pos >= args.length() - 1) {
            qCritical("This application is not supposed to be executed manually");
            return 127;
        }
        clientCmd = args[pos + 1];
    }

    if (serverCmd.isEmpty() || clientCmd.isEmpty()) {
        qCritical("This application is not supposed to be executed manually");
        return 127;
    }

    int fd = -1;
    if (!fdArg.isEmpty()) {
        auto fdOk = false;
        fd = fdArg.toInt(&fdOk);
        if (!fdOk) {;
            qCritical("This application is not supposed to be executed manually");
            return 127;
        }
    }

    auto *signalHandler = new SignalHandler(&app);
    signalHandler->initialize();
    QObject::connect(signalHandler, &SignalHandler::sigintReceived, &QCoreApplication::quit);
    QObject::connect(signalHandler, &SignalHandler::sigtermReceived, &QCoreApplication::quit);

    auto *helper = new XOrgUserHelper(fd, serverCmd, clientCmd);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [helper] {
        qInfo("Quitting...");
        helper->stop();
        helper->deleteLater();
    });

    QCoreApplication::postEvent(helper, new StartupEvent());

    return app.exec();
}

#include "HelperStartXorgUser.moc"
