
#ifndef BRO_PLUGIN_INSTRUMENTATION
#define BRO_PLUGIN_INSTRUMENTATION

#include "syshooks/syshook-malloc.h"
#include "syshooks/syshook-io.h"

#include <map>
#include <plugin/Plugin.h>

#include "Func.h"

#include <signal.h>
#include <stdio.h>
#include <inttypes.h>
#include "util/counters.h"

namespace plugin {
namespace Instrumentation {

class Plugin : public ::plugin::Plugin
{
public:
    virtual void HookUpdateNetworkTime(double network_time);
    virtual void InitPreScript();
    virtual std::pair<Val*, bool> HookCallFunction(const Func* func, Frame *parent, val_list* args);

	static void SetCollectionTimer(const double target);
	static void SetCollectionCount(const uint64_t target);
	static void SetCollectionTarget(const std::string target);
	static void WriteCollection();
	static void FlushCollection();
	static void FinalizeCollection();

	static void SetFunctionDataTarget(const std::string target);
	static void WriteFunctionData();
	static void FinalizeFunctionData();

	static void SetOutputDataFormat(std::string type);

	static void SetChainDataCutoff(const uint64_t target);
	static void SetChainDataTarget(const std::string target);
	static void WriteChainData();

    static void ExportStart(const uint16_t port);
    static void ExportAdd(const std::string key, const std::string value);
    static void ExportRemove(const std::string key);
    static void ExportUpdate(); 

protected:
	virtual plugin::Configuration Configure();
	static Val* CallBroFunction(const BroFunc* func, Frame *parent, val_list* args);
	static Val* CallBuiltinFunction(const BuiltinFunc* func, Frame *parent, val_list* args);
};

extern Plugin plugin;

}
}

#endif
