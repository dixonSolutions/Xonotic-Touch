#include "inputdevice.h"

#include <QDebug>

// Linux uinput — only available on Linux; the project targets ARM64 Ubuntu Touch.
#include <linux/uinput.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>

namespace {

// Axis range used for EV_ABS events
constexpr int kAxisMin  = -32767;
constexpr int kAxisMax  =  32767;
constexpr int kAxisFuzz =  16;
constexpr int kAxisFlat =  512;   // dead zone ≈ 1.5 %

// Ordered list maps pressButton(index) → kernel button code
constexpr unsigned int kButtonCodes[] = {
    BTN_SOUTH,   // 0 — A / primary attack
    BTN_EAST,    // 1 — B
    BTN_NORTH,   // 2 — X
    BTN_WEST,    // 3 — Y
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtonCodes) / sizeof(kButtonCodes[0]));

// Ordered list maps setAxis(index) → kernel abs code
constexpr unsigned int kAxisCodes[] = {
    ABS_X,   // 0 — left stick horizontal (strafe)
    ABS_Y,   // 1 — left stick vertical   (forward/back)
};
constexpr int kAxisCount = static_cast<int>(sizeof(kAxisCodes) / sizeof(kAxisCodes[0]));

} // namespace

InputDevice::InputDevice(QObject *parent)
    : QObject(parent)
{
    if (!openDevice()) {
        qWarning() << "InputDevice: failed to open /dev/uinput —"
                   << strerror(errno)
                   << "(touch input will be unavailable)";
    }
}

InputDevice::~InputDevice()
{
    closeDevice();
}

// ─── private helpers ────────────────────────────────────────────────────────

bool InputDevice::openDevice()
{
    m_fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (m_fd < 0) return false;

    // ── Register event types ──────────────────────────────────────────────
    if (ioctl(m_fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    if (ioctl(m_fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (ioctl(m_fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (ioctl(m_fd, UI_SET_EVBIT, EV_REL) < 0) goto fail;

    // ── Register buttons ──────────────────────────────────────────────────
    for (int i = 0; i < kButtonCount; ++i) {
        if (ioctl(m_fd, UI_SET_KEYBIT, kButtonCodes[i]) < 0) goto fail;
    }

    // ── Register absolute axes ────────────────────────────────────────────
    for (int i = 0; i < kAxisCount; ++i) {
        if (ioctl(m_fd, UI_SET_ABSBIT, kAxisCodes[i]) < 0) goto fail;
    }

    // ── Register relative axes (camera look) ─────────────────────────────
    if (ioctl(m_fd, UI_SET_RELBIT, REL_X) < 0) goto fail;
    if (ioctl(m_fd, UI_SET_RELBIT, REL_Y) < 0) goto fail;

    // ── Device identity ───────────────────────────────────────────────────
    {
        struct uinput_setup usetup;
        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_VIRTUAL;
        usetup.id.vendor  = 0x1d6b;   // Linux Foundation
        usetup.id.product = 0x0001;
        usetup.id.version = 1;
        strncpy(usetup.name, "Xonotic Touch Gamepad", UINPUT_MAX_NAME_SIZE - 1);

        if (ioctl(m_fd, UI_DEV_SETUP, &usetup) < 0) goto fail;
    }

    // ── Per-axis range configuration ──────────────────────────────────────
    for (int i = 0; i < kAxisCount; ++i) {
        struct uinput_abs_setup abs_setup;
        memset(&abs_setup, 0, sizeof(abs_setup));
        abs_setup.code              = static_cast<unsigned short>(kAxisCodes[i]);
        abs_setup.absinfo.minimum   = kAxisMin;
        abs_setup.absinfo.maximum   = kAxisMax;
        abs_setup.absinfo.fuzz      = kAxisFuzz;
        abs_setup.absinfo.flat      = kAxisFlat;
        if (ioctl(m_fd, UI_ABS_SETUP, &abs_setup) < 0) goto fail;
    }

    // ── Create the device node ────────────────────────────────────────────
    if (ioctl(m_fd, UI_DEV_CREATE) < 0) goto fail;

    qDebug() << "InputDevice: virtual gamepad created on /dev/uinput";
    return true;

fail:
    qWarning() << "InputDevice: uinput setup failed —" << strerror(errno);
    ::close(m_fd);
    m_fd = -1;
    return false;
}

void InputDevice::closeDevice()
{
    if (m_fd < 0) return;
    ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
}

void InputDevice::writeEvent(unsigned short type, unsigned short code, int value)
{
    if (m_fd < 0) return;

    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (::write(m_fd, &ev, sizeof(ev)) < 0) {
        qWarning() << "InputDevice: write failed —" << strerror(errno);
    }
}

void InputDevice::syncReport()
{
    writeEvent(EV_SYN, SYN_REPORT, 0);
}

// ─── public slots ────────────────────────────────────────────────────────────

void InputDevice::setAxis(int axis, float value)
{
    if (axis < 0 || axis >= kAxisCount) return;

    // Clamp and scale the normalised [-1, 1] float to the kernel int range
    const float clamped  = qBound(-1.0f, value, 1.0f);
    const int   intValue = static_cast<int>(clamped * static_cast<float>(kAxisMax));

    writeEvent(EV_ABS, static_cast<unsigned short>(kAxisCodes[axis]), intValue);
    syncReport();
}

void InputDevice::pressButton(int btn, bool pressed)
{
    if (btn < 0 || btn >= kButtonCount) return;

    writeEvent(EV_KEY, static_cast<unsigned short>(kButtonCodes[btn]), pressed ? 1 : 0);
    syncReport();
}

void InputDevice::sendLook(float dx, float dy)
{
    // Relative mouse-style deltas — Darkplaces maps these as cursor movement
    writeEvent(EV_REL, REL_X, static_cast<int>(dx));
    writeEvent(EV_REL, REL_Y, static_cast<int>(dy));
    syncReport();
}
