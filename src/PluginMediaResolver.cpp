#include "PluginMediaResolver.h"

#include "plugin/PluginManager.h"
#include "plugin/PbPluginApi.h"

IPbMediaPlugin *PluginMediaResolver::findHandler(const QString &url)
{
    return PluginManager::instance().findHandler(url);
}

void PluginMediaResolver::resolve(IPbMediaPlugin *plugin, const QString &url, int quality,
                                  const std::function<void(const PbResolvedMedia &)> &callback)
{
    if (!plugin)
        return;
    plugin->resolve(url, quality, callback);
}
