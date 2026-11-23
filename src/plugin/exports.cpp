// SPDX-License-Identifier: MIT
// The one export of the plugin DLL, the root plugin table and the local API table.
#include "build_config.h"
#include "core/api_table.h"
#include "core/host_link.h"
#include "core/reader_table.h"
#include "platform/pipe_server.h"

#include <GenOnlinePluginApi.h>

namespace genonline_caster
{

namespace
{

constexpr uint32_t kOfferedCapabilities = GENONLINE_PLUGIN_CAP_LOCAL_API;

HostLink g_host;
ReaderTable g_table;
PipeServer g_server(g_table, g_host);
bool g_shutDown = false;

int32_t GENONLINE_PLUGIN_CALL Initialize()
{
	g_host.Logf(GENONLINE_PLUGIN_LOG_INFO, "Initializing version %s", GENONLINE_CASTER_PLUGIN_VERSION);
	return GENONLINE_PLUGIN_RESULT_OK;
}

void GENONLINE_PLUGIN_CALL Tick()
{
}

void GENONLINE_PLUGIN_CALL Shutdown()
{
	if (g_shutDown)
	{
		return;
	}
	g_shutDown = true;
	g_server.Stop();
	g_host.Log(GENONLINE_PLUGIN_LOG_INFO, "Shut down");
	g_host.Release();
}

int32_t GENONLINE_PLUGIN_CALL StartListener(uint32_t maxReaders)
{
	try
	{
		if (!g_server.Start(maxReaders))
		{
			return GENONLINE_PLUGIN_FALSE;
		}
		g_host.Logf(GENONLINE_PLUGIN_LOG_INFO, "Listening on %s", PipeName().c_str());
		return GENONLINE_PLUGIN_TRUE;
	}
	catch (...)
	{
		g_host.Log(GENONLINE_PLUGIN_LOG_ERROR, "StartListener failed");
		return GENONLINE_PLUGIN_FALSE;
	}
}

int32_t GENONLINE_PLUGIN_CALL IsConnected()
{
	return g_table.IsConnected() ? GENONLINE_PLUGIN_TRUE : GENONLINE_PLUGIN_FALSE;
}

int32_t GENONLINE_PLUGIN_CALL EnqueueFrame(uint8_t frameType, int32_t frameClass, const void* body, uint32_t bodyLength, uint64_t timestampMicros, uint32_t readerId)
{
	if (!g_table.EnqueueFrame(frameType, frameClass, body, bodyLength, timestampMicros, readerId))
	{
		return GENONLINE_PLUGIN_FALSE;
	}
	g_server.Wake();
	return GENONLINE_PLUGIN_TRUE;
}

int32_t GENONLINE_PLUGIN_CALL TakeReaderEvent(uint32_t* outReaderId, int32_t* outEvent)
{
	return g_table.TakeReaderEvent(outReaderId, outEvent) ? GENONLINE_PLUGIN_TRUE : GENONLINE_PLUGIN_FALSE;
}

int32_t GENONLINE_PLUGIN_CALL TakeInbound(uint32_t* outReaderId, uint8_t* outFrameType, uint32_t* outFlags, const uint8_t** outBody, uint32_t* outBodyLength)
{
	if (outReaderId == nullptr || outFrameType == nullptr || outFlags == nullptr || outBody == nullptr || outBodyLength == nullptr)
	{
		return GENONLINE_PLUGIN_FALSE;
	}
	TakenInbound taken;
	if (!g_table.TakeInbound(&taken))
	{
		return GENONLINE_PLUGIN_FALSE;
	}
	*outReaderId = taken.readerId;
	*outFrameType = taken.frameType;
	*outFlags = taken.flags;
	*outBody = taken.body;
	*outBodyLength = taken.bodyLength;
	return GENONLINE_PLUGIN_TRUE;
}

void GENONLINE_PLUGIN_CALL FreeInbound(const uint8_t* body)
{
	delete[] body;
}

const GenOnlinePluginLocalApi g_localApiTable = {
	sizeof(GenOnlinePluginLocalApi),
	GENONLINE_PLUGIN_LOCAL_API_VERSION,
	&StartListener,
	&IsConnected,
	&EnqueueFrame,
	&TakeReaderEvent,
	&TakeInbound,
	&FreeInbound,
};

GenOnlinePluginApi CompleteTable()
{
	GenOnlinePluginApi table = {};
	table.structSize = sizeof(GenOnlinePluginApi);
	table.apiVersion = GENONLINE_PLUGIN_API_VERSION;
	table.name = GENONLINE_CASTER_PLUGIN_NAME;
	table.version = GENONLINE_CASTER_PLUGIN_VERSION;
	table.capabilities = kOfferedCapabilities;
	table.Initialize = &Initialize;
	table.Tick = &Tick;
	table.Shutdown = &Shutdown;
	table.localApi = &g_localApiTable;
	return table;
}

} // namespace

} // namespace genonline_caster

extern "C" GENONLINE_PLUGIN_EXPORT int32_t GENONLINE_PLUGIN_CALL GenOnlinePlugin_GetApi(uint32_t hostApiVersion, const GenOnlinePluginHostApi* host, GenOnlinePluginApi* out)
{
	using namespace genonline_caster;
	const int32_t check = CheckGetApiArguments(hostApiVersion, host, out);
	if (check != GENONLINE_PLUGIN_RESULT_OK)
	{
		return check;
	}
	const int32_t filled = FillPluginApi(CompleteTable(), out);
	if (filled != GENONLINE_PLUGIN_RESULT_OK)
	{
		return filled;
	}
	g_host.Bind(*host, kOfferedCapabilities);
	g_shutDown = false;
	return GENONLINE_PLUGIN_RESULT_OK;
}
