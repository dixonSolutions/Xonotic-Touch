#include "launcher.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTimer>

Launcher::Launcher(QObject *parent)
    : QObject(parent)
    , m_udpSocket(new QUdpSocket(this))
{
}

bool Launcher::launch()
{
    QString appRoot = qgetenv("APP_DIR");
    if (appRoot.isEmpty()) {
        appRoot = QDir::currentPath();
    }

    const QString engineDir = QDir(appRoot).filePath("engine");
    const QString binary    = QDir(engineDir).filePath("xonotic");

    if (!QFile::exists(binary)) {
        return false;
    }

    QStringList args;
    args << "-xonotic"
         << "+vid_fullscreen"      << "1"
         << "+vid_width"           << "1080"
         << "+vid_height"          << "2160"
         << "+joy_enable"          << "1"
         << "+cl_movement"         << "1"
         << "+sensitivity"         << "3.5"
         << "+m_pitch"             << "0.022"
         << "+m_yaw"               << "0.022"
         << "+con_closeontoggle"   << "1"
         << "+scr_screenshot_jpeg" << "0";

    m_process.setWorkingDirectory(engineDir);
    m_process.start(binary, args);
    return m_process.waitForStarted(3000);
}

void Launcher::shoot()
{
    m_process.write("+attack\n");
    QTimer::singleShot(80, this, [this] { m_process.write("-attack\n"); });
    emit shootingChanged(true);
    QTimer::singleShot(150, this, [this] { emit shootingChanged(false); });
}

void Launcher::setMove(bool up, bool down, bool left, bool right)
{
    if (up    != m_up)    { m_process.write(up    ? "+forward\n"   : "-forward\n");  m_up    = up; }
    if (down  != m_down)  { m_process.write(down  ? "+back\n"      : "-back\n");     m_down  = down; }
    if (left  != m_left)  { m_process.write(left  ? "+moveleft\n"  : "-moveleft\n"); m_left  = left; }
    if (right != m_right) { m_process.write(right ? "+moveright\n" : "-moveright\n");m_right = right; }
}

void Launcher::sendLook(float dx, float dy)
{
    // Primary: Quake-protocol UDP console command to darkplaces
    QByteArray cmd = "\xff\xff\xff\xffmousemove "
        + QByteArray::number(static_cast<int>(dx)) + " "
        + QByteArray::number(static_cast<int>(dy)) + "\n";

    if (m_udpSocket->writeDatagram(cmd, QHostAddress::LocalHost, 26000) < 0) {
        // Fallback: write directly to the process stdin
        m_process.write(("mouse " + QString::number(static_cast<int>(dx))
                         + " " + QString::number(static_cast<int>(dy)) + "\n").toUtf8());
    }
}
