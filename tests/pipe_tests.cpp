// SPDX-License-Identifier: MIT
// Tests of the pipe server with a reader connected in the same process.
#include "test_harness.h"

#include "core/frame_codec.h"
#include "core/host_link.h"
#include "core/reader_table.h"
#include "platform/pipe_server.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

using namespace genonline_caster;

namespace
{

constexpr DWORD kWaitMs = 10000;

void GENONLINE_PLUGIN_CALL IgnoreLog(void*, int32_t, const char*)
{
}

HostLink BoundHost()
{
	GenOnlinePluginHostApi table = {};
	table.structSize = sizeof(table);
	table.Log = &IgnoreLog;
	HostLink host;
	host.Bind(table, GENONLINE_PLUGIN_CAP_LOCAL_API);
	return host;
}

// Opens the pipe as a reader, retrying while the server creates its instances.
HANDLE ConnectReader()
{
	const std::string name = PipeName();
	const ULONGLONG deadline = GetTickCount64() + kWaitMs;
	while (GetTickCount64() < deadline)
	{
		HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
		{
			return pipe;
		}
		Sleep(10);
	}
	return INVALID_HANDLE_VALUE;
}

template <typename Condition>
bool WaitFor(Condition condition)
{
	const ULONGLONG deadline = GetTickCount64() + kWaitMs;
	while (GetTickCount64() < deadline)
	{
		if (condition())
		{
			return true;
		}
		Sleep(5);
	}
	return false;
}

bool ReadExactly(HANDLE pipe, uint8_t* buffer, DWORD length)
{
	DWORD done = 0;
	while (done < length)
	{
		DWORD read = 0;
		if (!ReadFile(pipe, buffer + done, length - done, &read, nullptr) || read == 0)
		{
			return false;
		}
		done += read;
	}
	return true;
}

bool WriteAll(HANDLE pipe, const std::vector<uint8_t>& bytes)
{
	DWORD written = 0;
	return WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
}

std::vector<uint8_t> InboundFrame(uint8_t frameType, const std::vector<uint8_t>& body)
{
	const uint32_t length = static_cast<uint32_t>(body.size());
	std::vector<uint8_t> frame = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length >> 16),
		static_cast<uint8_t>(length >> 24), frameType};
	frame.insert(frame.end(), body.begin(), body.end());
	return frame;
}

struct Event
{
	uint32_t readerId = 0;
	int32_t event = -1;
};

// Waits for the next reader event.
Event WaitForEvent(ReaderTable& table)
{
	Event event;
	WaitFor([&] { return table.TakeReaderEvent(&event.readerId, &event.event); });
	return event;
}

} // namespace

TEST_CASE(reader_gets_hello_and_its_request_reaches_the_host)
{
	HostLink host = BoundHost();
	ReaderTable table;
	PipeServer server(table, host);
	REQUIRE(server.Start(4));

	HANDLE pipe = ConnectReader();
	REQUIRE(pipe != INVALID_HANDLE_VALUE);

	const Event connected = WaitForEvent(table);
	CHECK(connected.event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	REQUIRE(connected.readerId != 0);
	CHECK(table.IsConnected());

	const uint8_t hello[] = {0x08, 0x02};
	REQUIRE(table.EnqueueFrame(0x09, GENONLINE_PLUGIN_LOCAL_API_FRAME_HELLO, hello, sizeof(hello), 0x1122334455667788ull, connected.readerId));
	server.Wake();
	uint8_t frame[kOutboundHeaderBytes + sizeof(hello)];
	REQUIRE(ReadExactly(pipe, frame, sizeof(frame)));
	const uint8_t expected[] = {0x02, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x08, 0x02};
	CHECK(std::memcmp(frame, expected, sizeof(expected)) == 0);

	const std::vector<uint8_t> request = {0x08, 0x2A, 0x12, 0x00};
	REQUIRE(WriteAll(pipe, InboundFrame(kInboundRequest, request)));

	TakenInbound taken;
	REQUIRE(WaitFor([&] { return table.TakeInbound(&taken); }));
	CHECK(taken.frameType == kInboundRequest);
	CHECK(taken.readerId == connected.readerId);
	CHECK(taken.flags == 0);
	REQUIRE(taken.bodyLength == request.size());
	CHECK(std::memcmp(taken.body, request.data(), request.size()) == 0);
	delete[] taken.body;

	CloseHandle(pipe);
	const Event gone = WaitForEvent(table);
	CHECK(gone.event == GENONLINE_PLUGIN_LOCAL_API_READER_GONE);
	CHECK(gone.readerId == connected.readerId);
	CHECK(!table.IsConnected());
	server.Stop();
	server.Stop();
}

TEST_CASE(bad_inbound_length_drops_the_reader)
{
	HostLink host = BoundHost();
	ReaderTable table;
	PipeServer server(table, host);
	REQUIRE(server.Start(4));
	HANDLE pipe = ConnectReader();
	REQUIRE(pipe != INVALID_HANDLE_VALUE);
	const Event connected = WaitForEvent(table);
	REQUIRE(connected.event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);

	const std::vector<uint8_t> zeroLength = {0x00, 0x00, 0x00, 0x00, kInboundRequest};
	REQUIRE(WriteAll(pipe, zeroLength));
	const Event gone = WaitForEvent(table);
	CHECK(gone.event == GENONLINE_PLUGIN_LOCAL_API_READER_GONE);
	CHECK(gone.readerId == connected.readerId);
	CHECK(!table.IsConnected());

	// The slot listens again for the next reader, which gets a new id.
	HANDLE next = ConnectReader();
	CHECK(next != INVALID_HANDLE_VALUE);
	const Event again = WaitForEvent(table);
	CHECK(again.event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	CHECK(again.readerId != connected.readerId);
	CloseHandle(next);
	CloseHandle(pipe);
	server.Stop();
}

TEST_CASE(a_reader_beyond_max_readers_is_busy)
{
	HostLink host = BoundHost();
	ReaderTable table;
	PipeServer server(table, host);
	REQUIRE(server.Start(2));
	HANDLE readers[2];
	for (HANDLE& reader : readers)
	{
		reader = ConnectReader();
		REQUIRE(reader != INVALID_HANDLE_VALUE);
	}
	for (int i = 0; i < 2; ++i)
	{
		CHECK(WaitForEvent(table).event == GENONLINE_PLUGIN_LOCAL_API_READER_HELLO_NEEDED);
	}
	HANDLE third = CreateFileA(PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	CHECK(third == INVALID_HANDLE_VALUE);
	CHECK(GetLastError() == ERROR_PIPE_BUSY);
	for (HANDLE reader : readers)
	{
		CloseHandle(reader);
	}
	server.Stop();
	CHECK(!table.IsConnected());
}

int main(int argc, char** argv)
{
	return testing::RunMain(argc, argv);
}
