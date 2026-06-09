#ifndef XONOTIC_LAUNCHER_H
#define XONOTIC_LAUNCHER_H

#include <QObject>
#include <QProcess>

class Launcher : public QObject
{
    Q_OBJECT

public:
    explicit Launcher(QObject *parent = nullptr);

public slots:
    bool launch();

signals:
    void hpChanged(int hp);
    void ammoChanged(int ammo);

private:
    QProcess m_process;
};

#endif
