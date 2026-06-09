#include "backend.h"
#include "inputdevice.h"
#include "launcher.h"

#include <QQmlEngine>
#include <QJSEngine>

void Backend::registerTypes(const char *uri)
{
    qmlRegisterSingletonType<Launcher>(uri, 1, 0, "Launcher",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Launcher(); });

    qmlRegisterSingletonType<InputDevice>(uri, 1, 0, "InputDevice",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new InputDevice(); });
}
