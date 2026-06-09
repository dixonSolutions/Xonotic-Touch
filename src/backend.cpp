#include "backend.h"
#include "launcher.h"

void Backend::registerTypes(const char *uri)
{
    qmlRegisterType<Launcher>(uri, 1, 0, "Launcher");
}
