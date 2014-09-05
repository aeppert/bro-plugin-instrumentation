
#ifndef BRO_PLUGIN_INSTRUMENTATION
#define BRO_PLUGIN_INSTRUMENTATION

#include <map>
#include <plugin/Plugin.h>

namespace plugin {
namespace Instrumentation {

class Plugin : public ::plugin::Plugin
{
public:
    virtual void HookUpdateNetworkTime(const double network_time);
    virtual void InitPreScript();
    virtual Val* HookCallFunction(const Func* func, Frame *parent, val_list* args);

	static void SetCollectionTimer(const double target);
	static void SetCollectionCount(const uint64_t target);
	static void SetCollectionTarget(const std::string target);
	static void WriteCollection();
	static void FlushCollection();

protected:
	static double _network_time;
	// Overridden from plugin::Plugin.
	virtual plugin::Configuration Configure();
};

extern Plugin plugin;

}
}

#endif
