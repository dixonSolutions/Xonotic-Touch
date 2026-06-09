#ifndef XONOTIC_INPUTDEVICE_H
#define XONOTIC_INPUTDEVICE_H

#include <QObject>

/**
 * Virtual gamepad backed by /dev/uinput.
 *
 * Registers:
 *   EV_ABS  — ABS_X (axis 0, strafe), ABS_Y (axis 1, forward/back)
 *   EV_KEY  — BTN_SOUTH/EAST/NORTH/WEST  (buttons 0–3)
 *   EV_REL  — REL_X / REL_Y              (camera look deltas)
 *
 * All public slots are QML-callable once the type is registered as a singleton.
 */
class InputDevice : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen CONSTANT)

public:
    explicit InputDevice(QObject *parent = nullptr);
    ~InputDevice() override;

    bool isOpen() const { return m_fd >= 0; }

public slots:
    /**
     * Push an analog axis value.
     * @param axis  0 = ABS_X (strafe),  1 = ABS_Y (forward/back)
     * @param value normalised range [-1.0, 1.0]
     */
    void setAxis(int axis, float value);

    /**
     * Press or release a gamepad button.
     * @param btn  0 = BTN_SOUTH (A), 1 = BTN_EAST (B),
     *             2 = BTN_NORTH (X), 3 = BTN_WEST (Y)
     */
    void pressButton(int btn, bool pressed);

    /**
     * Send a relative look/camera delta (REL_X / REL_Y).
     * Values are raw pixel deltas from the touch drag.
     */
    void sendLook(float dx, float dy);

private:
    int m_fd = -1;

    bool openDevice();
    void closeDevice();
    void writeEvent(unsigned short type, unsigned short code, int value);
    void syncReport();
};

#endif
