// SPDX-License-Identifier: MIT
#include "platform/pipe_server.h"

#include "core/host_link.h"
#include "core/reader_table.h"

#include <process.h>

#include <cstdio>
#include <cstring>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace genonline_caster
{

namespace
{

constexpr DWORD kPipeBufferBytes = 32768;
constexpr DWORD kPipeRetryWaitMs = 1000;
// The worker wakes at least this often, so a missed wake never stalls it for longer.
constexpr DWORD kPipeIdleWaitMs = 250;

// A security descriptor whose only access control entry grants the current user full access to the pipe.
struct PipeSecurity
{
	SECURITY_ATTRIBUTES attributes;
	SECURITY_DESCRIPTOR descriptor;
	// DWORD arrays keep the SID and the ACL aligned.
	DWORD tokenBuffer[128];
	DWORD aclBuffer[128];
};

bool BuildPipeSecurity(PipeSecurity& security)
{
	HANDLE token = nullptr;
	DWORD needed = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		return false;
	}
	const BOOL haveUser = GetTokenInformation(token, TokenUser, security.tokenBuffer, sizeof(security.tokenBuffer), &needed);
	CloseHandle(token);
	if (!haveUser)
	{
		return false;
	}

	PSID userSid = reinterpret_cast<TOKEN_USER*>(security.tokenBuffer)->User.Sid;
	ACL* acl = reinterpret_cast<ACL*>(security.aclBuffer);
	const DWORD aclLength = static_cast<DWORD>((sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(userSid) + 3) & ~3u);
	if (aclLength > sizeof(security.aclBuffer))
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return false;
	}
	if (!InitializeAcl(acl, aclLength, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, userSid)
		|| !InitializeSecurityDescriptor(&security.descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !SetSecurityDescriptorDacl(&security.descriptor, TRUE, acl, FALSE))
	{
		return false;
	}
	security.attributes.nLength = sizeof(security.attributes);
	security.attributes.lpSecurityDescriptor = &security.descriptor;
	security.attributes.bInheritHandle = FALSE;
	return true;
}

void CloseEvent(HANDLE& handle)
{
	if (handle != nullptr)
	{
		CloseHandle(handle);
		handle = nullptr;
	}
}

} // namespace

std::string PipeName()
{
	char name[64];
	std::snprintf(name, sizeof(name), "\\\\.\\pipe\\generalsonline-caster-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
	return name;
}

PipeServer::PipeServer(ReaderTable& table, const HostLink& host)
	: m_table(table)
	, m_host(host)
{
}

PipeServer::~PipeServer()
{
	Stop();
}

bool PipeServer::Start(uint32_t maxReaders)
{
	if (m_thread != nullptr)
	{
		return true;
	}
	m_maxReaders = maxReaders < 1 ? 1 : (maxReaders > static_cast<uint32_t>(kMaxReaders) ? kMaxReaders : static_cast<int>(maxReaders));
	m_shuttingDown = false;
	m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	m_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	bool created = m_wakeEvent != nullptr && m_shutdownEvent != nullptr;
	for (int i = 0; i < kMaxReaders; ++i)
	{
		Slot& slot = m_slots[i];
		slot.connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		slot.readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		slot.writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		created = created && slot.connectEvent != nullptr && slot.readEvent != nullptr && slot.writeEvent != nullptr;
	}
	if (created)
	{
		m_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &PipeServer::WorkerThunk, this, 0, nullptr));
	}
	if (m_thread == nullptr)
	{
		m_host.Logf(GENONLINE_PLUGIN_LOG_ERROR, "Starting the caster pipe failed, error %lu", static_cast<unsigned long>(GetLastError()));
		CloseHandles();
		return false;
	}
	return true;
}

void PipeServer::Stop()
{
	m_shuttingDown = true;
	if (m_shutdownEvent != nullptr)
	{
		SetEvent(m_shutdownEvent);
	}
	if (m_thread != nullptr)
	{
		// The worker returns within kPipeIdleWaitMs, and the DLL must not unload while it runs.
		WaitForSingleObject(m_thread, INFINITE);
		CloseHandle(m_thread);
		m_thread = nullptr;
	}
	for (int i = 0; i < kMaxReaders; ++i)
	{
		CloseSlot(i);
	}
	CloseHandles();
}

void PipeServer::CloseHandles()
{
	for (Slot& slot : m_slots)
	{
		CloseEvent(slot.connectEvent);
		CloseEvent(slot.readEvent);
		CloseEvent(slot.writeEvent);
	}
	CloseEvent(m_wakeEvent);
	CloseEvent(m_shutdownEvent);
}

void PipeServer::Wake()
{
	if (m_wakeEvent != nullptr)
	{
		SetEvent(m_wakeEvent);
	}
}

bool PipeServer::ProcessInboundBytes(int index, uint32_t bytesTransferred)
{
	Slot& slot = m_slots[index];
	slot.inboundLength += bytesTransferred;
	for (;;)
	{
		uint8_t frameType = 0;
		uint32_t bodyLength = 0;
		const InboundParse parse = ParseInbound(slot.inbound, slot.inboundLength, &frameType, &bodyLength);
		if (parse == InboundParse::NeedMore)
		{
			return true;
		}
		if (parse == InboundParse::BadLength)
		{
			return false;
		}
		m_table.HandleInboundFrame(index, frameType, slot.inbound + kInboundHeaderBytes, bodyLength, GetTickCount64());
		const uint32_t frameLength = kInboundHeaderBytes + bodyLength;
		slot.inboundLength -= frameLength;
		if (slot.inboundLength > 0)
		{
			std::memmove(slot.inbound, slot.inbound + frameLength, slot.inboundLength);
		}
	}
}

unsigned __stdcall PipeServer::WorkerThunk(void* param)
{
	static_cast<PipeServer*>(param)->WorkerProc();
	return 0;
}

int PipeServer::StartListening(Slot& slot, DWORD* createError)
{
	*createError = 0;
	const std::string name = PipeName();

	// Only the user the game runs as may open the pipe, and never over the network. Without that user no pipe is created.
	PipeSecurity security;
	if (!BuildPipeSecurity(security))
	{
		*createError = GetLastError();
		return 0;
	}

	slot.pipe = CreateNamedPipeA(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, static_cast<DWORD>(m_maxReaders), kPipeBufferBytes, kPipeBufferBytes, 0,
		&security.attributes);
	if (slot.pipe == INVALID_HANDLE_VALUE)
	{
		*createError = GetLastError();
		return 0;
	}

	ZeroMemory(&slot.connectOverlapped, sizeof(slot.connectOverlapped));
	slot.connectOverlapped.hEvent = slot.connectEvent;
	ResetEvent(slot.connectEvent);

	const BOOL connectedImmediately = ConnectNamedPipe(slot.pipe, &slot.connectOverlapped);
	const DWORD lastError = GetLastError();
	if (connectedImmediately || lastError == ERROR_PIPE_CONNECTED)
	{
		return 2;
	}
	if (lastError == ERROR_IO_PENDING)
	{
		return 1;
	}
	*createError = lastError;
	CloseHandle(slot.pipe);
	slot.pipe = INVALID_HANDLE_VALUE;
	return 0;
}

void PipeServer::OnConnected(int index)
{
	Slot& slot = m_slots[index];
	slot.state = SlotState::Connected;
	slot.readPending = false;
	slot.writePending = false;
	slot.inboundLength = 0;
	m_table.OnConnected(index, GetTickCount64());
	if (!BeginRead(slot))
	{
		CloseSlot(index);
	}
}

void PipeServer::CloseSlot(int index)
{
	Slot& slot = m_slots[index];
	if (slot.state == SlotState::Closed && slot.pipe == INVALID_HANDLE_VALUE)
	{
		return;
	}

	// The worker is the only thread with I/O on the pipe of a slot, so CancelIo cancels exactly these operations.
	if (slot.pipe != INVALID_HANDLE_VALUE && (slot.readPending || slot.writePending || slot.state == SlotState::Listening))
	{
		DWORD bytesTransferred = 0;
		CancelIo(slot.pipe);
		if (slot.readPending)
		{
			GetOverlappedResult(slot.pipe, &slot.readOverlapped, &bytesTransferred, TRUE);
		}
		if (slot.writePending)
		{
			GetOverlappedResult(slot.pipe, &slot.writeOverlapped, &bytesTransferred, TRUE);
		}
		if (slot.state == SlotState::Listening)
		{
			GetOverlappedResult(slot.pipe, &slot.connectOverlapped, &bytesTransferred, TRUE);
		}
	}
	slot.readPending = false;
	slot.writePending = false;

	if (slot.pipe != INVALID_HANDLE_VALUE)
	{
		if (slot.state == SlotState::Connected)
		{
			DisconnectNamedPipe(slot.pipe);
		}
		CloseHandle(slot.pipe);
		slot.pipe = INVALID_HANDLE_VALUE;
	}
	slot.inboundLength = 0;
	m_table.OnClosed(index);
	slot.state = SlotState::Closed;
	slot.retryAtMs = 0;
}

bool PipeServer::BeginRead(Slot& slot)
{
	ZeroMemory(&slot.readOverlapped, sizeof(slot.readOverlapped));
	slot.readOverlapped.hEvent = slot.readEvent;
	ResetEvent(slot.readEvent);
	if (slot.inboundLength >= sizeof(slot.inbound))
	{
		return false;
	}
	slot.readPending = true;
	DWORD bytesRead = 0;
	if (!ReadFile(slot.pipe, slot.inbound + slot.inboundLength, static_cast<DWORD>(sizeof(slot.inbound) - slot.inboundLength), &bytesRead, &slot.readOverlapped))
	{
		if (GetLastError() != ERROR_IO_PENDING)
		{
			slot.readPending = false;
			return false;
		}
	}
	// readEvent is signalled also when the read completes at once on an overlapped handle.
	return true;
}

bool PipeServer::BeginWrite(int index)
{
	Slot& slot = m_slots[index];
	if (slot.state != SlotState::Connected || slot.writePending)
	{
		return true;
	}
	const uint32_t available = m_table.Peek(index, slot.sendBuffer, kSendChunkBytes);
	if (available == 0)
	{
		return true;
	}
	ZeroMemory(&slot.writeOverlapped, sizeof(slot.writeOverlapped));
	slot.writeOverlapped.hEvent = slot.writeEvent;
	ResetEvent(slot.writeEvent);
	// A reader that does not read leaves its write pending while the other readers carry on.
	if (!WriteFile(slot.pipe, slot.sendBuffer, available, nullptr, &slot.writeOverlapped))
	{
		if (GetLastError() != ERROR_IO_PENDING)
		{
			return false;
		}
	}
	slot.writePending = true;
	return true;
}

void PipeServer::PumpWrites()
{
	for (int i = 0; i < m_maxReaders; ++i)
	{
		if (m_slots[i].state == SlotState::Connected && !BeginWrite(i))
		{
			CloseSlot(i);
		}
	}
}

void PipeServer::WorkerProc()
{
	enum HandleKind
	{
		kShutdown,
		kWake,
		kConnect,
		kRead,
		kWrite,
	};
	constexpr int kMaxHandles = 2 + 3 * kMaxReaders;
	bool loggedCreateFailure = false;

	while (!m_shuttingDown)
	{
		HANDLE handles[kMaxHandles];
		int handleSlot[kMaxHandles];
		HandleKind handleKind[kMaxHandles];
		DWORD count = 0;

		// Every free slot keeps an instance listening, so a reader can connect while the others are busy.
		for (int i = 0; i < m_maxReaders; ++i)
		{
			Slot& slot = m_slots[i];
			if (slot.state != SlotState::Closed || GetTickCount64() < slot.retryAtMs)
			{
				continue;
			}
			DWORD createError = 0;
			const int started = StartListening(slot, &createError);
			if (started == 0)
			{
				if (!loggedCreateFailure)
				{
					loggedCreateFailure = true;
					m_host.Logf(GENONLINE_PLUGIN_LOG_WARNING, "Caster pipe creation failed, error %lu, retrying", static_cast<unsigned long>(createError));
				}
				slot.retryAtMs = GetTickCount64() + kPipeRetryWaitMs;
			}
			else
			{
				slot.state = SlotState::Listening;
				if (started == 2)
				{
					OnConnected(i);
				}
			}
		}

		handles[count] = m_shutdownEvent;
		handleSlot[count] = -1;
		handleKind[count++] = kShutdown;
		handles[count] = m_wakeEvent;
		handleSlot[count] = -1;
		handleKind[count++] = kWake;
		for (int i = 0; i < m_maxReaders; ++i)
		{
			Slot& slot = m_slots[i];
			if (slot.state == SlotState::Listening)
			{
				handles[count] = slot.connectEvent;
				handleSlot[count] = i;
				handleKind[count++] = kConnect;
			}
			else if (slot.state == SlotState::Connected)
			{
				if (slot.readPending)
				{
					handles[count] = slot.readEvent;
					handleSlot[count] = i;
					handleKind[count++] = kRead;
				}
				if (slot.writePending)
				{
					handles[count] = slot.writeEvent;
					handleSlot[count] = i;
					handleKind[count++] = kWrite;
				}
			}
		}

		const DWORD waitResult = WaitForMultipleObjects(count, handles, FALSE, kPipeIdleWaitMs);
		if (waitResult == WAIT_FAILED)
		{
			break;
		}
		if (waitResult < WAIT_OBJECT_0 + count)
		{
			const DWORD which = waitResult - WAIT_OBJECT_0;
			const int index = handleSlot[which];
			if (handleKind[which] == kShutdown)
			{
				break;
			}
			if (handleKind[which] == kConnect)
			{
				Slot& slot = m_slots[index];
				DWORD bytesTransferred = 0;
				if (GetOverlappedResult(slot.pipe, &slot.connectOverlapped, &bytesTransferred, FALSE))
				{
					OnConnected(index);
				}
				else
				{
					// The reader came and went before it was accepted, so the slot listens again.
					CloseSlot(index);
				}
			}
			else if (handleKind[which] == kRead)
			{
				Slot& slot = m_slots[index];
				DWORD bytesTransferred = 0;
				const bool readOk = GetOverlappedResult(slot.pipe, &slot.readOverlapped, &bytesTransferred, FALSE) != FALSE;
				slot.readPending = false;
				// A failed or empty read means the reader is gone, and a bad frame length drops it too.
				if (!readOk || bytesTransferred == 0 || !ProcessInboundBytes(index, bytesTransferred) || !BeginRead(slot))
				{
					CloseSlot(index);
				}
			}
			else if (handleKind[which] == kWrite)
			{
				Slot& slot = m_slots[index];
				DWORD written = 0;
				const bool writeOk = GetOverlappedResult(slot.pipe, &slot.writeOverlapped, &written, FALSE) != FALSE;
				slot.writePending = false;
				if (!writeOk)
				{
					CloseSlot(index);
				}
				else
				{
					// A short write leaves the rest queued for the next pump.
					m_table.Consume(index, written);
				}
			}
		}
		PumpWrites();
	}

	for (int i = 0; i < kMaxReaders; ++i)
	{
		CloseSlot(i);
	}
}

} // namespace genonline_caster
