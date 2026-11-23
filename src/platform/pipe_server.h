// SPDX-License-Identifier: MIT
// The named pipe server of the local readers: one worker thread with overlapped I/O on up to kMaxReaders pipe instances.
#pragma once

#include "core/frame_codec.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace genonline_caster
{

class HostLink;
class ReaderTable;

// The pipe name of this process, \\.\pipe\generalsonline-caster-<pid>.
std::string PipeName();

class PipeServer
{
public:
	PipeServer(ReaderTable& table, const HostLink& host);
	~PipeServer();

	PipeServer(const PipeServer&) = delete;
	PipeServer& operator=(const PipeServer&) = delete;

	// Creates the events and the worker thread for up to maxReaders readers, clamped to 1..kMaxReaders. Returns true when the worker runs.
	bool Start(uint32_t maxReaders);

	// Stops the worker and closes every pipe instance. Safe to call more than once and without Start.
	void Stop();

	// Tells the worker that new frames are queued.
	void Wake();

	bool IsRunning() const
	{
		return m_thread != nullptr;
	}

private:
	enum { kSendChunkBytes = 8192 };

	enum class SlotState
	{
		Closed,
		Listening,
		Connected,
	};

	// The I/O state of one pipe instance. Only the worker thread touches it.
	struct Slot
	{
		SlotState state = SlotState::Closed;
		HANDLE pipe = INVALID_HANDLE_VALUE;
		HANDLE connectEvent = nullptr;
		HANDLE readEvent = nullptr;
		HANDLE writeEvent = nullptr;
		OVERLAPPED connectOverlapped = {};
		OVERLAPPED readOverlapped = {};
		OVERLAPPED writeOverlapped = {};
		bool readPending = false;
		bool writePending = false;
		uint32_t inboundLength = 0;
		uint8_t inbound[kInboundHeaderBytes + kMaxInboundBodyBytes] = {};
		uint8_t sendBuffer[kSendChunkBytes] = {};
		uint64_t retryAtMs = 0;
	};

	static unsigned __stdcall WorkerThunk(void* param);
	void WorkerProc();

	// 0 when the instance could not be created, 1 while it waits for a reader, 2 when a reader is already connected.
	int StartListening(Slot& slot, DWORD* createError);
	void OnConnected(int index);
	void CloseSlot(int index);
	bool BeginRead(Slot& slot);
	bool BeginWrite(int index);
	void PumpWrites();
	bool ProcessInboundBytes(int index, uint32_t bytesTransferred);
	void CloseHandles();

	ReaderTable& m_table;
	const HostLink& m_host;
	HANDLE m_thread = nullptr;
	HANDLE m_wakeEvent = nullptr;
	HANDLE m_shutdownEvent = nullptr;
	std::atomic<bool> m_shuttingDown{false};
	Slot m_slots[kMaxReaders];
	// Only the first m_maxReaders slots are used, and the pipe allows as many instances.
	int m_maxReaders = kMaxReaders;
};

} // namespace genonline_caster
