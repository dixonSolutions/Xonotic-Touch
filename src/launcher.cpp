#include "launcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

Launcher::Launcher(QObject *parent)
    : QObject(parent)
{
}

bool Launcher::launch()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString engineDir = QDir(appDir).filePath("engine");
    const QString binary = QDir(engineDir).filePath("xonotic");

    if (!QFile::exists(binary)) {
        return false;
    }

    m_process.setProgram(binary);
    m_process.setArguments({"-xonotic", "+vid_fullscreen", "1"});
    m_process.setWorkingDirectory(engineDir);
    return m_process.startDetached();
}
