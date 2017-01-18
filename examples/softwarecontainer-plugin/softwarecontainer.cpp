/****************************************************************************
**
** Copyright (C) 2017 Pelagicore AG
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Pelagicore Application Manager.
**
** $QT_BEGIN_LICENSE:LGPL-QTAS$
** Commercial License Usage
** Licensees holding valid commercial Qt Automotive Suite licenses may use
** this file in accordance with the commercial license agreement provided
** with the Software or, alternatively, in accordance with the terms
** contained in a written agreement between you and The Qt Company.  For
** licensing terms and conditions see https://www.qt.io/terms-conditions.
** For further information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
** SPDX-License-Identifier: LGPL-3.0
**
****************************************************************************/

#include <tuple>

#include <QtDBus/QtDBus>
#include <QtAppManCommon/global.h>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <qplatformdefs.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include "softwarecontainer.h"


QT_BEGIN_NAMESPACE

QDBusArgument &operator<<(QDBusArgument &argument, const QMap<QString,QString> &map)
{
    argument.beginMap(QMetaType::QString, QMetaType::QString);
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        argument.beginMapEntry();
        argument << it.key() << it.value();
        argument.endMapEntry();
    }
    argument.endMap();

    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, QMap<QString,QString> &map)
{
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key, value;
        argument >> key >> value;
        map.insert(key, value);
        argument.endMapEntry();
    }
    argument.endMap();

    return argument;
}

QT_END_NAMESPACE


QT_USE_NAMESPACE_AM

SoftwareContainerManager::SoftwareContainerManager()
{
    static bool once = false;
    if (!once) {
        once = true;
        qDBusRegisterMetaType<QMap<QString, QString>>();
    }
}

QString SoftwareContainerManager::identifier() const
{
    return QStringLiteral("softwarecontainer");
}

bool SoftwareContainerManager::supportsQuickLaunch() const
{
    return false;
}

void SoftwareContainerManager::setConfiguration(const QVariantMap &configuration)
{
    m_configuration = configuration;
}

ContainerInterface *SoftwareContainerManager::create()
{
    if (!m_interface) {
        QString dbus = configuration().value(QStringLiteral("dbus")).toString();
        QDBusConnection conn(QStringLiteral("sc-bus"));

        if (dbus.isEmpty())
            dbus = QStringLiteral("system");

        if (dbus == QLatin1String("system"))
             conn = QDBusConnection::systemBus();
        else if (dbus == QLatin1String("session"))
            conn = QDBusConnection::sessionBus();
        else
            conn = QDBusConnection::connectToBus(dbus, QStringLiteral("sc-bus"));

        if (!conn.isConnected()) {
            qWarning() << "The" << dbus << "D-Bus is not available to connect to the SoftwareContainer agent.";
            return nullptr;
        }
        m_interface = new QDBusInterface(QStringLiteral("com.pelagicore.SoftwareContainerAgent"),
                                         QStringLiteral("/com/pelagicore/SoftwareContainerAgent"),
                                         QStringLiteral("com.pelagicore.SoftwareContainerAgent"),
                                         conn, this);
        if (m_interface->lastError().isValid()) {
            qWarning() << "Could not connect to com.pelagicore.SoftwareContainerAgent, "
                          "/com/pelagicore/SoftwareContainerAgent on the" << dbus << "D-Bus";
            delete m_interface;
            m_interface = nullptr;
            return nullptr;
        }

        if (!connect(m_interface, SIGNAL(ProcessStateChanged(int,uint,bool,uint)), this, SLOT(processStateChanged(int,uint,bool,uint)))) {
            qWarning() << "Could not connect to the com.pelagicore.SoftwareContainerAgent.ProcessStateChanged "
                          "signal on the" << dbus << "D-Bus";
            delete m_interface;
            m_interface = nullptr;
            return nullptr;
        }
    }

    QString config = qSL("[]");
    QVariant v = configuration().value("createConfig");
    if (v.isValid())
        config = QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));

    QDBusMessage reply = m_interface->call(QDBus::Block, "Create", config);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "SoftwareContainer failed to create a new container:" << reply.errorMessage()
                   << "(config was:" << config << ")";
        return nullptr;
    }

    int containerId = reply.arguments().at(0).value<int>();
    bool success = reply.arguments().at(1).value<bool>();

    if (!success) {
        qWarning() << "SoftwareContainer failed to create a new container. (config was:" << config << ")";
        return nullptr;
    }

    SoftwareContainer *container = new SoftwareContainer(this, containerId);
    m_containers.insert(containerId, container);
    connect(container, &QObject::destroyed, this, [this, containerId]() { m_containers.remove(containerId); });
    return container;
}

QDBusInterface *SoftwareContainerManager::interface() const
{
    return m_interface;
}

QVariantMap SoftwareContainerManager::configuration() const
{
    return m_configuration;
}

void SoftwareContainerManager::processStateChanged(int containerId, uint processId, bool isRunning, uint exitCode)
{
    Q_UNUSED(processId)

    SoftwareContainer *container = m_containers.value(containerId);
    if (!container) {
        qWarning() << "Received a processStateChanged signal for unknown container" << containerId;
        return;
    }

    if (!isRunning)
        container->containerExited(exitCode);
}



SoftwareContainer::SoftwareContainer(SoftwareContainerManager *manager, int containerId)
    : m_manager(manager)
    , m_id(containerId)
{ }

SoftwareContainer::~SoftwareContainer()
{
    if (m_fifoFd >= 0)
        QT_CLOSE(m_fifoFd);
    if (!m_fifoPath.isEmpty())
        ::unlink(m_fifoPath);
}

SoftwareContainerManager *SoftwareContainer::manager() const
{
    return m_manager;
}

bool SoftwareContainer::attachApplication(const QVariantMap &application)
{
    m_state = QProcess::Starting;
    m_application = application;

    m_hostPath = application.value(qSL("baseDir")).toString();
    if (m_hostPath.isEmpty())
        m_hostPath = QDir::currentPath();

    m_appRelativeCodePath = application.value(qSL("codeFilePath")).toString();
    m_containerPath = qSL("/app");

    m_ready = true;
    emit ready();
    return true;
}

QString SoftwareContainer::controlGroup() const
{
    return QString();
}

bool SoftwareContainer::setControlGroup(const QString &groupName)
{
    Q_UNUSED(groupName)
    return false;
}

bool SoftwareContainer::setProgram(const QString &program)
{
    m_program = program;
    return true;
}

void SoftwareContainer::setBaseDirectory(const QString &baseDirectory)
{
    m_baseDir = baseDirectory;
}

bool SoftwareContainer::isReady() const
{
    return m_ready;
}

QString SoftwareContainer::mapContainerPathToHost(const QString &containerPath) const
{
    return containerPath;
}

QString SoftwareContainer::mapHostPathToContainer(const QString &hostPath) const
{
    return hostPath;
}

bool SoftwareContainer::start(const QStringList &arguments, const QProcessEnvironment &environment)
{
    auto iface = manager()->interface();
    if (!iface)
        return false;

    if (!QFile::exists(m_program))
        return false;

    // this is the one and only capability in io.qt.ApplicationManager.Application.json
    static const QStringList capabilities { qSL("io.qt.ApplicationManager.Application") };

    QDBusMessage reply = iface->call(QDBus::Block, "SetCapabilities", m_id, QVariant::fromValue(capabilities));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "SoftwareContainer failed to set capabilities to" << capabilities << ":" << reply.errorMessage();
        return false;
    }

    if (!reply.arguments().at(0).value<bool>()) {
        qWarning() << "SoftwareContainer failed to set capabilities to" << capabilities;
        return false;
    }

    // parse out the actual socket file name from the DBus specification
    QString dbusP2PSocket = environment.value(QStringLiteral("AM_DBUS_PEER_ADDRESS"));
    dbusP2PSocket = dbusP2PSocket.mid(dbusP2PSocket.indexOf(QLatin1Char('=')) + 1);
    dbusP2PSocket = dbusP2PSocket.left(dbusP2PSocket.indexOf(QLatin1Char(',')));
    QFileInfo dbusP2PInfo(dbusP2PSocket);

    QFileInfo fontCacheInfo(qSL("/var/cache/fontconfig"));

    QList<std::tuple<QString, QString, bool>> bindMounts; // bool == isReadOnly
    // the private P2P D-Bus
    bindMounts.append(std::make_tuple(dbusP2PInfo.absoluteFilePath(), dbusP2PInfo.absoluteFilePath(), false));

    // we need to share the fontconfig cache - otherwise the container startup might take a long time
    bindMounts.append(std::make_tuple(fontCacheInfo.absoluteFilePath(), fontCacheInfo.absoluteFilePath(), false));

    // the actual path to the application
    bindMounts.append(std::make_tuple(m_hostPath, m_containerPath, true));

    // for development only - mount the user's $HOME dir into the container as read-only. Otherwise
    // you would have to `make install` the AM into /usr on every rebuild
    bindMounts.append(std::make_tuple(QDir::homePath(), QDir::homePath(), true));

    // do all the bind-mounts in parallel to waste as little time as possible
    QList<QDBusPendingReply<bool>> bindMountResults;

    for (auto it = bindMounts.cbegin(); it != bindMounts.cend(); ++it)
        bindMountResults << iface->asyncCall("BindMount", m_id, std::get<0>(*it), std::get<1>(*it), std::get<2>(*it));

    for (auto pending : qAsConst(bindMountResults))
        QDBusPendingCallWatcher(pending).waitForFinished();

    for (int i = 0; i < bindMounts.size(); ++i) {
        if (bindMountResults.at(i).isError() || !bindMountResults.at(i).value()) {
            qWarning() << "SoftwareContainer failed to bind-mount the directory" << std::get<0>(bindMounts.at(i))
                       << "into the container at" << std::get<1>(bindMounts.at(i)) << ":" << bindMountResults.at(i).error().message();
            return false;
        }
    }

    // create an unique fifo name in /tmp
    m_fifoPath = QDir::tempPath().toLocal8Bit() + "/sc-" + QUuid::createUuid().toByteArray().mid(1,36) + ".fifo";
    if (mkfifo(m_fifoPath, 0600) != 0) {
        qWarning() << "Failed to create FIFO at" << m_fifoPath << "to redirect stdout/stderr out of the container:" << strerror(errno);
        m_fifoPath.clear();
        return false;
    }

    // open fifo for reading
    // due to QTBUG-15261 (bytesAvailable() on a fifo always returns 0) we use a raw fd
    m_fifoFd = QT_OPEN(m_fifoPath, O_RDONLY | O_NONBLOCK);
    if (m_fifoFd < 0) {
        qWarning() << "Failed to open FIFO at" << m_fifoPath << "for reading:" << strerror(errno);
        return false;
    }

    // read from fifo and dump to message handler
    QSocketNotifier *sn = new QSocketNotifier(m_fifoFd, QSocketNotifier::Read, this);
    connect(sn, &QSocketNotifier::activated, this, [sn](int fifoFd) {
        int bytesAvailable = 0;
        if (ioctl(fifoFd, FIONREAD, &bytesAvailable) == 0) {
            static const int bufferSize = 4096;
            static QByteArray buffer(bufferSize, 0);

            while (bytesAvailable > 0) {
                auto bytesRead = QT_READ(fifoFd, buffer.data(), std::min(bytesAvailable, bufferSize));
                if (bytesRead < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    sn->setEnabled(false);
                    return;
                } else if (bytesRead > 0) {
                    QT_WRITE(STDOUT_FILENO, buffer.constData(), bytesRead);
                    bytesAvailable -= bytesRead;
                }
            }
        }
    });

    // SC expects a plain string instead of individual args
    QString cmdLine = m_program;
    for (auto &&arg : arguments)
        cmdLine += QStringLiteral(" \"") + arg + QStringLiteral("\"");
    //cmdLine.prepend(QStringLiteral("/usr/bin/strace ")); // useful if things go wrong...

    // we start with a copy of the AM's environment, but some variables would overwrite important
    // redirections set by SC gateways.
    static const char *forbiddenVars[] = {
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "DBUS_SYSTEM_BUS_ADDRESS",
        "PULSE_SERVER",
        nullptr
    };

    QMap<QString, QString> envVars;
    for (auto &&envVar : environment.keys()) {
        bool forbidden = false;
        for (const char **p = forbiddenVars; *p; ++p) {
            if (envVar == QLatin1String(*p)) {
                forbidden = true;
                break;
            }
        }
        if (!forbidden)
            envVars.insert(envVar, environment.value(envVar));
    }
    QVariant venvVars = QVariant::fromValue(envVars);

    qDebug () << "SoftwareContainer is trying to launch application" << m_id
              << "\n * command ..." << cmdLine
              << "\n * directory ." << m_containerPath
              << "\n * output ...." << m_fifoPath
              << "\n * environment" << envVars;

#if 0
    qWarning() << "Attach to container now!";
    qWarning().nospace() << "  sudo lxc-attach -n SC-" << m_id;
    sleep(10000000);
#endif

    reply = iface->call(QDBus::Block, "Execute", m_id, cmdLine, m_containerPath, QString::fromLocal8Bit(m_fifoPath), venvVars);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "SoftwareContainer failed to execute application" << m_id << "in directory" << m_containerPath << "in the container:" << reply.errorMessage();
        return false;
    }

    if (!reply.arguments().at(1).value<bool>()) {
        qWarning() << "SoftwareContainer failed to execute application" << m_id << "in directory" << m_containerPath << "in the container.";
        return false;
    }

    m_pid = reply.arguments().at(0).value<int>();

    m_state = QProcess::Running;
    QTimer::singleShot(0, this, [this]() {
        emit stateChanged(m_state);
        emit started();
    });
    return true;
}

bool SoftwareContainer::isStarted() const
{
    return (m_pid);
}

qint64 SoftwareContainer::processId() const
{
    return m_pid;
}

QProcess::ProcessState SoftwareContainer::state() const
{
    return m_state;
}

void SoftwareContainer::kill()
{
    auto iface = manager()->interface();

    if (iface) {
        QDBusMessage reply = iface->call(QDBus::Block, "Destroy", m_id);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qWarning() << "SoftwareContainer failed to destroy container" << reply.errorMessage();
        }

        if (!reply.arguments().at(0).value<bool>()) {
            qWarning() << "SoftwareContainer failed to destroy container.";
        }
    }
}

void SoftwareContainer::terminate()
{
    //TODO: handle graceful shutdown
    kill();
}

void SoftwareContainer::containerExited(uint exitCode)
{
    m_state = QProcess::NotRunning;
    emit stateChanged(m_state);
    emit finished(WEXITSTATUS(exitCode), WIFEXITED(exitCode) ? QProcess::NormalExit : QProcess::CrashExit);
    deleteLater();
}