#include "backend.h"
#include "launcher.h"

#include <QQmlEngine>
#include <QJSEngine>

void Backend::registerTypes(const char *uri)
{
    qmlRegisterSingletonType<Launcher>(uri, 1, 0, "Launcher",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Launcher(); });
}
