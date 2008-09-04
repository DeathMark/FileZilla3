#include "FileZilla.h"
#include "backend.h"
#include "socket.h"
#include <errno.h>

int CBackend::m_nextId = 0;

CBackend::CBackend(CSocketEventHandler* pEvtHandler) : m_pEvtHandler(pEvtHandler)
{
	m_Id = GetNextId();
}

int CBackend::GetNextId()
{
	const int id = m_nextId++;
	if (m_nextId < 0)
		m_nextId = 0;
	return id;
}

CSocketBackend::CSocketBackend(CSocketEventHandler* pEvtHandler, CSocket* pSocket) : CBackend(pEvtHandler), m_pSocket(pSocket)
{
	m_error = false;
	m_lastCount = 0;
	m_lastError = 0;

	m_pSocket->SetEventHandler(pEvtHandler, GetId());

	CRateLimiter* pRateLimiter = CRateLimiter::Get();
	if (pRateLimiter)
		pRateLimiter->AddObject(this);
}

CSocketBackend::~CSocketBackend()
{
	m_pSocket->SetEventHandler(0, -1);

	CRateLimiter* pRateLimiter = CRateLimiter::Get();
	if (pRateLimiter)
		pRateLimiter->RemoveObject(this);
}

void CSocketBackend::Write(const void *buffer, unsigned int len)
{
	wxLongLong max = GetAvailableBytes(CRateLimiter::outbound);
	if (max == 0)
	{
		Wait(CRateLimiter::outbound);
		m_error = true;
		m_lastError = EAGAIN;
		return;
	}
	else if (max > 0 && max < len)
		len = max.GetLo();

	m_lastCount = m_pSocket->Write(buffer, len, m_lastError);
	m_error = m_lastCount == -1;

	if (!m_error && max != -1)
		UpdateUsage(CRateLimiter::outbound, m_lastCount);
}

void CSocketBackend::Read(void *buffer, unsigned int len)
{
	wxLongLong max = GetAvailableBytes(CRateLimiter::inbound);
	if (max == 0)
	{
		Wait(CRateLimiter::inbound);
		m_error = true;
		m_lastError = EAGAIN;
		return;
	}
	else if (max > 0 && max < len)
		len = max.GetLo();

	m_lastCount = m_pSocket->Read(buffer, len, m_lastError);
	m_error = m_lastCount == -1;

	if (!m_error && max != -1)
		UpdateUsage(CRateLimiter::inbound, m_lastCount);
}

void CSocketBackend::Peek(void *buffer, unsigned int len)
{
	m_lastCount = m_pSocket->Peek(buffer, len, m_lastError);
	m_error = m_lastCount == -1;
}

void CSocketBackend::OnRateAvailable(enum CRateLimiter::rate_direction direction)
{
	CSocketEvent *evt;
	if (direction == CRateLimiter::outbound)
		evt = new CSocketEvent(m_pEvtHandler, GetId(), CSocketEvent::write);
	else
		evt = new CSocketEvent(m_pEvtHandler, GetId(), CSocketEvent::read);
	
	CSocketEventDispatcher::Get().SendEvent(evt);
}
