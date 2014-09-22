
#include "Plugin.h"

#include "Func.h"
#include "Stats.h"
#include "Stmt.h"
#include "Trigger.h"

#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <set>

#include "util/functable.h"
#include "util/funcchain.h"

namespace plugin { namespace Instrumentation { Plugin plugin; } }

using namespace plugin::Instrumentation;

// A few counters to handle updates for various counters / time-based statistics
static double _last_stats_update = 0.0;
static double _stats_timer = 0.0;
static uint64_t _last_stats_count = 0;
static uint64_t _stats_count = 0;

static std::string _stats_target = "";
static std::ofstream _stats_ofstream;
static bool _stats_separator = false;

static std::string _fdata_target = "";
static std::ofstream _fdata_ofstream;
static bool _fdata_separator = false;

static std::string _fchain_target = "";
static std::ofstream _fchain_ofstream;
static uint64_t _fchain_cutoff = 0;

static CounterSet _original_state;

static double _network_time;
// transient state needed to keep track of counter start points while functions are executing
static std::vector<FunctionCounterSet> _counter_stack;
// persistent counter state
static std::map<uint32_t, FunctionCounterSet> _counters;
typedef std::map<uint32_t, FunctionCounterSet>::iterator _counter_iterator;

static FunctionTable _function_table;
static FunctionCallChain _function_chains;

static FunctionCounterSet::OutputType _output_type = FunctionCounterSet::OUTPUT_JSON;

plugin::Configuration Plugin::Configure()
	{
	plugin::Configuration config;
	config.name = "Instrumentation::Instrumentation";
	config.description = "Plugin that adds low-level instrumentation to a running bro process.";
	config.version.major = 1;
	config.version.minor = 0;
	return config;
	}

void Plugin::SetOutputDataFormat(std::string type)
	{
	if("application/json" == type) 
		{
		_output_type = FunctionCounterSet::OUTPUT_JSON;
		}
	else if("text/csv" == type)
		{
		_output_type = FunctionCounterSet::OUTPUT_CSV;
		}
	else 
		{
		_output_type = FunctionCounterSet::OUTPUT_JSON;
		}
	}

void Plugin::HookUpdateNetworkTime(const double network_time)
	{
	_network_time = network_time;
	++_last_stats_count;

	// trigger on number of packets
	if(_stats_count > 0 && _last_stats_count >= _stats_count)
		{
			_last_stats_count = 0;
			WriteCollection();
		}
	// trigger on network timestamps
	else if(_stats_timer > 0.001 && network_time - _last_stats_update > _stats_timer) 
		{
			_last_stats_update = network_time;
			WriteCollection();
		}
	}

Val* Plugin::CallBroFunction(const BroFunc *func, Frame *parent, val_list *args)
	{
#ifdef PROFILE_BRO_FUNCTIONS
    DEBUG_MSG("Function: %s\n", id->Name());
#endif

    // printf("Executing bro method: %s\n", func->Name());
    std::vector<Func::Body> bodies = func->GetBodies();

    if ( bodies.empty() )
        {
        // Can only happen for events and hooks.
        assert(func->Flavor() == FUNC_FLAVOR_EVENT || func->Flavor() == FUNC_FLAVOR_HOOK);
        //happens in HandlePluginResult() ...
        //loop_over_list(*args, i)
        //    Unref((*args)[i]);

        return func->Flavor() == FUNC_FLAVOR_HOOK ? new Val(true, TYPE_BOOL) : 0;
        }


    /*
	FIXME: something is wrong here, but not yet sure what ...

	HandlePluginResult has a blanket Unref(), but in the case that the arg list is
	passed along to the frame when we make our call, it'll be Unref'd there as well.

	Thus, Ref our arguments here so that the Unref in HandlePluginResult doesn't break
	anything ...
    */
    {
    loop_over_list(*args, i)
    	Ref((*args)[i]);    	
    }

    Frame* f = new Frame(func->FrameSize(), func, args);

    // Hand down any trigger.
    if ( parent )
        {
        f->SetTrigger(parent->GetTrigger());
        f->SetCall(parent->GetCall());
        }

    g_frame_stack.push_back(f); // used for backtracing

    loop_over_list(*args, i)
        f->SetElement(i, (*args)[i]);

    stmt_flow_type flow = FLOW_NEXT;

    Val* result = 0;

    for ( size_t i = 0; i < bodies.size(); ++i )
        {

        Unref(result);

        try
            {
            const Location* loc = bodies[i].stmts->GetLocationInfo();
            uint32_t key = _function_table.add(func, i, loc);

            _counter_stack.push_back(FunctionCounterSet::Create(_network_time));
            _function_chains.add(key);
            result = bodies[i].stmts->Exec(f, flow);
            FunctionCounterSet result = FunctionCounterSet::Create(_network_time);
            result -= _counter_stack.back();
            result.count = 1;
            _counter_stack.pop_back();
            _function_chains.end();

            if(_counters.find(key) != _counters.end())
	            {
	            _counters[key] = (_counters[key] + result);
    	        }
    	    else
    		    {
    		    char sbuf[4096];
    		    _counters[key] = result;
    		    _counters[key].name = std::string(func->Name());
    		    snprintf(sbuf, 4096, "%s:%d", loc->filename, loc->first_line);
    		    _counters[key].location = FunctionTable::beautify(std::string(sbuf));
	    	    }
            }

        catch ( InterpreterException& e )
            {
            // Already reported, but we continue exec'ing remaining bodies.
            continue;
            }

        if ( f->HasDelayed() )
            {
            assert(! result);
            assert(parent);
            parent->SetDelayed();
            break;
            }

        if ( func->Flavor() == FUNC_FLAVOR_HOOK )
            {
            // Ignore any return values of hook bodies, final return value
            // depends on whether a body returns as a result of break statement.
            Unref(result);
            result = 0;

            if ( flow == FLOW_BREAK )
                {
                // Short-circuit execution of remaining hook handler bodies.
                result = new Val(false, TYPE_BOOL);
                break;
                }
         	}
        }

    if ( func->Flavor() == FUNC_FLAVOR_HOOK )
        {
        if ( ! result )
            result = new Val(true, TYPE_BOOL);
        }

    // Warn if the function returns something, but we returned from
    // the function without an explicit return, or without a value.
    else if ( func->FType()->YieldType() && func->FType()->YieldType()->Tag() != TYPE_VOID &&
         (flow != FLOW_RETURN /* we fell off the end */ ||
          ! result /* explicit return with no result */) &&
         ! f->HasDelayed() )
        reporter->Warning("non-void function returns without a value: %s",
                          func->Name());

    g_frame_stack.pop_back();
    Unref(f);

    // hack: since the plugin architecture can't distinguish between a NULL returned by our method
    // and a NULL returned by a function, we rely on the plugin result handler to fix things for us.
    if(NULL == result) {
    	return new Val(0, TYPE_ERROR);
    }
    return result;
	}

Val* Plugin::CallBuiltinFunction(const BuiltinFunc *func, Frame *parent, val_list *args)
	{
    Val* result = func->TheFunc()(parent, args);
    // hack: since the plugin architecture can't distinguish between a NULL returned by our method
    // and a NULL returned by a function, we rely on the plugin result handler to fix things for us.
    if(NULL == result) {
    	return new Val(0, TYPE_ERROR);
    }
    return result;
	}

Val* Plugin::HookCallFunction(const Func* func, Frame *parent, val_list* args)
	{
	if ( func->GetKind() == Func::BRO_FUNC) 
		{
		return CallBroFunction((BroFunc *)func, parent, args);
		} // end standard bro function call
	else if (func->GetKind() == Func::BUILTIN_FUNC)
		{
		return CallBuiltinFunction((BuiltinFunc *)func, parent, args);
		}
	else
		{
		reporter->Warning("[instrumentation] unable to detect function call type.  dropping through to default handler.");
		return NULL;
		}

	}

void Plugin::InitPreScript()
	{
	//reporter->Info("[instrumentation] Initializing instrumentation plugin...\n");
	plugin::Plugin::InitPreScript();

	EnableHook(HOOK_UPDATE_NETWORK_TIME);
	EnableHook(HOOK_CALL_FUNCTION);
	
    // reporter->Info("[instrumentation] initialization completed.\n");
	_original_state.Read();

	}

void Plugin::SetCollectionTimer(const double target) 
	{
	_stats_timer = target;
	}

void Plugin::SetCollectionCount(const uint64_t target)
	{
	_stats_count = target;
	}

void Plugin::SetCollectionTarget(const std::string target)
	{
	_stats_target = target;
	_stats_ofstream.open(_stats_target);
	FunctionCounterSet::ConfigWriter(_stats_ofstream, _output_type);
	}

void Plugin::SetFunctionDataTarget(const std::string target)
	{
	_fdata_target = target;
	_fdata_ofstream.open(_fdata_target);
	FunctionCounterSet::ConfigWriter(_fdata_ofstream, _output_type);
	}

void Plugin::FinalizeFunctionData()
	{
	FunctionCounterSet::FinalizeWriter(_fdata_ofstream, _output_type);
	_fdata_ofstream.flush();
	}

void Plugin::SetChainDataTarget(const std::string target)
	{
	_fchain_target = target;
	_fchain_ofstream.open(_fchain_target);
	}

void Plugin::SetChainDataCutoff(const uint64_t target)
	{
	_fchain_cutoff = target;
	}

void Plugin::WriteChainData()
	{
	assert(_fchain_ofstream.good());
	std::vector<CallChain> chains = _function_chains.list();
	std::vector<CallChain>::iterator iter = chains.begin();

	_fchain_ofstream << "digraph G {" << std::endl;
	std::set<uint32_t> used;

	for(iter; iter != chains.end(); ++iter) 
		{
		if(iter->count < _fchain_cutoff) {
			continue;
		}
		std::vector<uint32_t> entries = iter->entries();
		std::vector<uint32_t>::iterator entry_iter = entries.begin();
		bool first_entry = true;
		_fchain_ofstream << "    ";
		for(entry_iter; entry_iter != entries.end(); ++entry_iter) 
			{
			if(!first_entry) 
				{
				_fchain_ofstream << " -> ";
				}
			else 
				{	
				first_entry = false;
				}
			_fchain_ofstream << *entry_iter;
			if(used.find(*entry_iter) == used.end()) 
				{
				used.insert(*entry_iter);
				}
			// _fchain_ofstream << curr.name << "@" << curr.file << ":" << curr.line;
			}
		_fchain_ofstream << ";" << std::endl;
		}
	for(std::set<uint32_t>::iterator iter = used.begin();
			iter != used.end(); ++iter) 
		{
		FunctionEntry curr = _function_table.lookup(*iter);
		_fchain_ofstream << *iter << " [shape=box,label=\"" << curr.name << "\\n" 
						 << FunctionTable::beautify(curr.file) << ":" << curr.line << "\"];" << std::endl;
		}
	_fchain_ofstream << "}" << std::endl;
	}

void Plugin::WriteFunctionData()
	{
	for(_counter_iterator iter = _counters.begin(); iter != _counters.end(); ++iter)
		{
		if(_fdata_separator) {
			FunctionCounterSet::WriteSeparator(_fdata_ofstream, _output_type);
		}
		else {
			_fdata_separator = true;
		}
		assert(_fdata_ofstream.good());
		iter->second.Write(_fdata_ofstream, _output_type);
		}
	_fdata_ofstream.flush();
	}

void Plugin::WriteCollection()
	{
	assert(_stats_ofstream.good());
	if(_stats_separator) {
		FunctionCounterSet::WriteSeparator(_stats_ofstream, _output_type);
	}
	else {
		_stats_separator = true;
	}
	FunctionCounterSet set = FunctionCounterSet::Create(_network_time);
	set.Write(_stats_ofstream, _output_type);
	}

void Plugin::FlushCollection()
	{
	assert(_stats_ofstream.good());
	_stats_ofstream.flush();
	}

void Plugin::FinalizeCollection()
	{
	FunctionCounterSet::FinalizeWriter(_stats_ofstream, _output_type);
	_stats_ofstream.flush();
	}
