#ifndef XONOTIC_LAUNCHER_H
#define XONOTIC_LAUNCHER_H

#include <QObject>
#include <QProcess>

class Launcher : public QObject
{
    Q_OBJECT

public:
    explicit Launcher(QObject *parent = nullptr);

    Q_INVOKABLE bool launch();

private:
    QProcess m_process;
};

#endif
