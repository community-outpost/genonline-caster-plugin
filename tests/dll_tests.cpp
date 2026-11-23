// SPDX-License-Identifier: MIT
// Tests of the staged plugin library through its export, loaded the way the game loads it.
#include "test_harness.h"

#include <GenOnlinePluginApi.h>

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace
{

const char* g_dllPath = nullptr;

struct HostRecorder
{
	std::vector<std::string> lines;
};

void GENONLINE_PLUGIN_CALL RecordLog(void* userData, int32_t, const char* message)
{
	static_cast<HostRecorder*>(userData)->lines.push_back(message);
}

GenOnlinePluginHostApi MakeHost(HostRecorder* recorder)
{
	GenOnlinePluginHostApi host = {};
	host.structSize = sizeof(host);
	host.userData = recorder;
	host.Log = &RecordLog;
	host.enabledCapabilities = GENONLINE_PLUGIN_CAP_LOCAL_API;
	return host;
}

GenOnlinePluginGetApiFn LoadGetApi()
{
	HMODULE library = LoadLibraryA(g_dllPath);
	if (library == nullptr)
	{
		std::fprintf(stderr, "Loading %s failed: %lu\n", g_dllPath, GetLastError());
		return nullptr;
	}
	return reinterpret_cast<GenOnlinePluginGetApiFn>(GetProcAddress(library, GENONLINE_PLUGIN_GET_API_EXPORT_NAME));
}

bool GetTable(const GenOnlinePluginHostApi& host, GenOnlinePluginApi* out)
{
	GenOnlinePluginGetApiFn getApi = LoadGetApi();
	if (getApi == nullptr)
	{
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->structSize = sizeof(*out);
	return getApi(GENONLINE_PLUGIN_API_VERSION, &host, out) == GENONLINE_PLUGIN_RESULT_OK;
}

bool AllPrefixed(const HostRecorder& recorder)
{
	for (const std::string& line : recorder.lines)
	{
		if (line.rfind("[genonline_caster] ", 0) != 0)
		{
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE(table_describes_the_caster_plugin)
{
	HostRecorder recorder;
	const GenOnlinePluginHostApi host = MakeHost(&recorder);
	GenOnlinePluginApi api;
	REQUIRE(GetTable(host, &api));
	CHECK(api.structSize == sizeof(GenOnlinePluginApi));
	CHECK(api.apiVersion == GENONLINE_PLUGIN_API_VERSION);
	CHECK(std::strcmp(api.name, "genonline_caster") == 0);
	CHECK(std::strcmp(api.version, GENONLINE_CASTER_EXPECTED_VERSION) == 0);
	CHECK(api.capabilities == GENONLINE_PLUGIN_CAP_LOCAL_API);
	CHECK(api.Initialize != nullptr && api.Tick != nullptr && api.Shutdown != nullptr);
	CHECK(api.IsExternalProcessRunning == nullptr && api.GetAnticheatIdentifier == nullptr);
	CHECK(api.linkedAccount == nullptr && api.anticheat == nullptr && api.transport == nullptr);
	REQUIRE(api.localApi != nullptr);
	CHECK(api.localApi->structSize == sizeof(GenOnlinePluginLocalApi));
	CHECK(api.localApi->version == GENONLINE_PLUGIN_LOCAL_API_VERSION);
	CHECK(api.localApi->StartListener != nullptr && api.localApi->FreeInbound != nullptr);
	CHECK(api.Initialize() == GENONLINE_PLUGIN_RESULT_OK);
	api.Tick();
	api.Shutdown();
	api.Shutdown();
	CHECK(!recorder.lines.empty());
	CHECK(AllPrefixed(recorder));
}

TEST_CASE(wrong_version_and_short_tables)
{
	HostRecorder recorder;
	const GenOnlinePluginHostApi host = MakeHost(&recorder);
	GenOnlinePluginGetApiFn getApi = LoadGetApi();
	REQUIRE(getApi != nullptr);
	GenOnlinePluginApi api = {};
	api.structSize = sizeof(api);
	CHECK(getApi(GENONLINE_PLUGIN_API_VERSION + 1, &host, &api) == GENONLINE_PLUGIN_RESULT_VERSION_MISMATCH);
	CHECK(getApi(GENONLINE_PLUGIN_API_VERSION, nullptr, &api) == GENONLINE_PLUGIN_RESULT_INVALID_ARGUMENT);

	api = {};
	api.structSize = static_cast<uint32_t>(offsetof(GenOnlinePluginApi, localApi));
	REQUIRE(getApi(GENONLINE_PLUGIN_API_VERSION, &host, &api) == GENONLINE_PLUGIN_RESULT_OK);
	CHECK(api.structSize == offsetof(GenOnlinePluginApi, localApi));
	CHECK(api.localApi == nullptr);

	// A host table that ends before Log gets no log line.
	GenOnlinePluginHostApi shortHost = host;
	shortHost.structSize = static_cast<uint32_t>(offsetof(GenOnlinePluginHostApi, Log));
	api = {};
	api.structSize = sizeof(api);
	REQUIRE(getApi(GENONLINE_PLUGIN_API_VERSION, &shortHost, &api) == GENONLINE_PLUGIN_RESULT_OK);
	CHECK(api.Initialize() == GENONLINE_PLUGIN_RESULT_OK);
	api.Shutdown();
	CHECK(recorder.lines.empty());
}

TEST_CASE(listener_accepts_a_reader)
{
	HostRecorder recorder;
	const GenOnlinePluginHostApi host = MakeHost(&recorder);
	GenOnlinePluginApi api;
	REQUIRE(GetTable(host, &api));
	REQUIRE(api.Initialize() == GENONLINE_PLUGIN_RESULT_OK);
	CHECK(api.localApi->IsConnected() == GENONLINE_PLUGIN_FALSE);
	REQUIRE(api.localApi->StartListener(4) == GENONLINE_PLUGIN_TRUE);

	const std::string name = "\\\\.\\pipe\\generalsonline-caster-" + std::to_string(GetCurrentProcessId());
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int i = 0; i < 1000 && pipe == INVALID_HANDLE_VALUE; ++i)
	{
		pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE)
		{
			Sleep(10);
		}
	}
	REQUIRE(pipe != INVALID_HANDLE_VALUE);
	uint32_t readerId = 0;
	int32_t event = -1;
	bool seen = false;
	for (int i = 0; i < 1000 && !seen; ++i)
	{
		seen = api.localApi->TakeReaderEvent(&readerId, &event) == GENONLINE_PLUGIN_TRUE;
		if (!seen)
		{
			Sleep(10);
		}
	}
	CHECK(seen);
	CHECK(readerId != 0);
	CHECK(event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	CHECK(api.localApi->IsConnected() == GENONLINE_PLUGIN_TRUE);
	const uint8_t hello[] = {0x08, 0x02};
	CHECK(api.localApi->EnqueueFrame(0x09, GENONLINE_PLUGIN_LOCAL_API_FRAME_LIFECYCLE, hello, sizeof(hello), 5, readerId) == GENONLINE_PLUGIN_FALSE);
	CHECK(api.localApi->EnqueueFrame(0x09, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, hello, sizeof(hello), 5, readerId) == GENONLINE_PLUGIN_TRUE);
	uint8_t frame[19] = {};
	DWORD read = 0;
	CHECK(ReadFile(pipe, frame, sizeof(frame), &read, nullptr) && read == sizeof(frame));
	CHECK(frame[4] == 0x09 && frame[9] == 5 && frame[17] == 0x08);

	uint32_t inboundReader = 0;
	uint8_t type = 0;
	uint32_t flags = 0;
	const uint8_t* body = nullptr;
	uint32_t bodyLength = 0;
	CHECK(api.localApi->TakeInbound(&inboundReader, &type, &flags, &body, &bodyLength) == GENONLINE_PLUGIN_FALSE);

	api.Shutdown();
	CHECK(api.localApi->IsConnected() == GENONLINE_PLUGIN_FALSE);
	CloseHandle(pipe);
	api.Shutdown();
}

int main(int argc, char** argv)
{
	if (argc >= 3)
	{
		g_dllPath = argv[2];
	}
	return testing::RunMain(argc, argv);
}
