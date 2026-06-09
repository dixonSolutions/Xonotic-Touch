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
    const QString binary = QDir(engineDir).filePath("xonotic");

    if (!QFile::exists(binary)) {
        return false;
    }

    m_process.setProgram(binary);
    m_process.setArguments({
        "-xonotic",
        "+vid_fullscreen", "1",
        "+vid_width", "1080",
        "+vid_height", "2340",
        "+joy_enable", "1",
    });
    m_process.setWorkingDirectory(engineDir);
    return m_process.startDetached();
}
