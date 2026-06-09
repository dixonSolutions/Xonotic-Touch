#include "launcher.h"

#include <QDir>
#include <QFile>

Launcher::Launcher(QObject *parent)
    : QObject(parent)
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
