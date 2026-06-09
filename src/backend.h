#ifndef XONOTIC_BACKEND_H
#define XONOTIC_BACKEND_H

#include <QQmlExtensionPlugin>

class Backend : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override;
};

#endif
