#ifndef XONOTIC_LAUNCHER_H
#define XONOTIC_LAUNCHER_H

#include <QObject>
#include <QProcess>
#include <QUdpSocket>

class Launcher : public QObject
{
    Q_OBJECT

public:
    explicit Launcher(QObject *parent = nullptr);

public slots:
    bool launch();
    void shoot();
    void setMove(bool up, bool down, bool left, bool right);
    void sendLook(float dx, float dy);

signals:
    void hpChanged(int hp);
    void ammoChanged(int ammo);
    void shootingChanged(bool shooting);

private:
    QProcess    m_process;
    QUdpSocket *m_udpSocket;

    bool m_up    = false;
    bool m_down  = false;
    bool m_left  = false;
    bool m_right = false;
};

#endif
