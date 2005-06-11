#include "FileZilla.h"
#include "logging_private.h"
#include "ControlSocket.h"
#include <idna.h>
#include "asynchostresolver.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_EVENT_TABLE(CControlSocket, wxEvtHandler)
	EVT_SOCKET(wxID_ANY, CControlSocket::OnSocketEvent)
END_EVENT_TABLE();

COpData::COpData()
{
	opId = cmd_none;
	opState = 0;

	pNextOpData = 0;

	waitForAsyncRequest = false;
}

COpData::~COpData()
{
	delete [] pNextOpData;
}

CControlSocket::CControlSocket(CFileZillaEnginePrivate *pEngine)
	: wxSocketClient(wxSOCKET_NOWAIT), CLogging(pEngine)
{
	m_pEngine = pEngine;
	m_pCurOpData = 0;
	m_pSendBuffer = 0;
	m_nSendBufferLen = 0;
	m_pCurrentServer = 0;
	m_pTransferStatus = 0;
	m_transferStatusSendState = 0;
	m_onConnectCalled = false;

	SetEvtHandlerEnabled(true);
	SetEventHandler(*this);
	SetNotify(wxSOCKET_INPUT_FLAG | wxSOCKET_OUTPUT_FLAG | wxSOCKET_CONNECTION_FLAG | wxSOCKET_LOST_FLAG);
	Notify(true);
}

CControlSocket::~CControlSocket()
{
	DoClose();
}

void CControlSocket::OnConnect(wxSocketEvent &event)
{
}

void CControlSocket::OnReceive(wxSocketEvent &event)
{
}

void CControlSocket::OnSend(wxSocketEvent &event)
{
	if (m_pSendBuffer)
	{
		if (!m_nSendBufferLen)
		{
			delete [] m_pSendBuffer;
			m_pSendBuffer = 0;
			return;
		}

		Write(m_pSendBuffer, m_nSendBufferLen);
		if (Error())
		{
			if (LastError() != wxSOCKET_WOULDBLOCK)
				DoClose();
			return;
		}

		int numsent = LastCount();

		if (numsent)
			m_pEngine->SetActive(false);

		if (numsent == m_nSendBufferLen)
		{
			m_nSendBufferLen = 0;
			delete [] m_pSendBuffer;
			m_pSendBuffer = 0;
		}
		else
		{
			memmove(m_pSendBuffer, m_pSendBuffer + numsent, m_nSendBufferLen - numsent);
			m_nSendBufferLen -= numsent;
		}
	}
}

void CControlSocket::OnClose(wxSocketEvent &event)
{
	if (GetCurrentCommandId() != cmd_connect)
		LogMessage(::Error, _("Disconnected from server"));
	DoClose();
}

int CControlSocket::Connect(const CServer &server)
{
	LogMessage(Status, _("Connecting to %s:%d..."), server.GetHost().c_str(), server.GetPort());

	CAsyncHostResolver *resolver = new CAsyncHostResolver(m_pEngine, ConvertDomainName(server.GetHost()));
	m_pEngine->AddNewAsyncHostResolver(resolver);

	resolver->Create();
	resolver->Run();

	if (m_pCurrentServer)
		delete m_pCurrentServer;
	m_pCurrentServer = new CServer(server);

	return FZ_REPLY_WOULDBLOCK;
}

int CControlSocket::ContinueConnect(const wxIPV4address *address)
{
	LogMessage(__TFILE__, __LINE__, this, Debug_Verbose, _T("ContinueConnect(%d) cmd=%d, m_pEngine=%d, m_pCurrentServer=%d"), (int)address, GetCurrentCommandId(), m_pEngine, m_pCurrentServer);
	if (GetCurrentCommandId() != cmd_connect ||
		!m_pCurrentServer)
	{
		LogMessage(Debug_Warning, _T("Invalid context for call to ContinueConnect()"));
		return DoClose(FZ_REPLY_INTERNALERROR);
	}
	
	if (!address)
	{
		LogMessage(::Error, _("Invalid hostname or host not found"));
		return ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_CRITICALERROR);
	}

	wxIPV4address addr = *address;
	addr.Service(m_pCurrentServer->GetPort());

	bool res = wxSocketClient::Connect(addr, false);

	if (!res && LastError() != wxSOCKET_WOULDBLOCK)
		return ResetOperation(FZ_REPLY_ERROR);

	return FZ_REPLY_WOULDBLOCK;
}

int CControlSocket::Disconnect()
{
	LogMessage(Status, _("Disconnected from server"));

	DoClose();
	return FZ_REPLY_OK;
}

void CControlSocket::OnSocketEvent(wxSocketEvent &event)
{
	switch (event.GetSocketEvent())
	{
	case wxSOCKET_CONNECTION:
		m_onConnectCalled = true;
		OnConnect(event);
		break;
	case wxSOCKET_INPUT:
		if (!m_onConnectCalled)
		{
			m_onConnectCalled = true;
			OnConnect(event);
		}
		OnReceive(event);
		break;
	case wxSOCKET_OUTPUT:
		OnSend(event);
		break;
	case wxSOCKET_LOST:
		OnClose(event);
		break;
	}
}

enum Command CControlSocket::GetCurrentCommandId() const
{
	if (m_pCurOpData)
		return m_pCurOpData->opId;

	return m_pEngine->GetCurrentCommandId();
}

int CControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(Debug_Verbose, _T("CControlSocket::ResetOperation(%d)"), nErrorCode);
	
	if (nErrorCode & FZ_REPLY_WOULDBLOCK)
	{
		LogMessage(::Debug_Warning, _T("ResetOperation with FZ_REPLY_WOULDBLOCK in nErrorCode (%d)"), nErrorCode);
	}
	
	if (m_pCurOpData)
	{
		delete m_pCurOpData;
		m_pCurOpData = 0;
	}

	if (nErrorCode != FZ_REPLY_OK)
	{
		if (nErrorCode & FZ_REPLY_CRITICALERROR)
			LogMessage(::Error, _("Critical error"));
		switch (GetCurrentCommandId())
		{
		case cmd_connect:
			LogMessage(::Error, _("Could not connect to server"));
			break;
		case cmd_list:
			LogMessage(::Error, _("Failed to retrieve directory listing"));
			break;
		default:
			break;
		}
	}
	else
	{
		switch (GetCurrentCommandId())
		{
		case cmd_list:
			LogMessage(Status, _("Directory listing successful"));
			break;
		case cmd_transfer:
			LogMessage(Status, _("File transfer successful"));
			break;
		default:
			break;
		}
	}

	ResetTransferStatus();

	return m_pEngine->ResetOperation(nErrorCode);
}

int CControlSocket::DoClose(int nErrorCode /*=FZ_REPLY_DISCONNECTED*/)
{
	nErrorCode = ResetOperation(FZ_REPLY_ERROR | FZ_REPLY_DISCONNECTED | nErrorCode);
	Close();

	delete [] m_pSendBuffer;
	m_pSendBuffer = 0;
	m_nSendBufferLen = 0;

	delete m_pCurrentServer;
	m_pCurrentServer = 0;
	
	m_onConnectCalled = false;

	SendDirectoryListing(0);

	return nErrorCode;
}

bool CControlSocket::Send(const char *buffer, int len)
{
	if (m_pSendBuffer)
	{
		char *tmp = m_pSendBuffer;
		m_pSendBuffer = new char[m_nSendBufferLen + len];
		memcpy(m_pSendBuffer, tmp, m_nSendBufferLen);
		memcpy(m_pSendBuffer + m_nSendBufferLen, buffer, len);
		m_nSendBufferLen += len;
		delete [] tmp;
	}
	else
	{
		Write(buffer, len);
		int numsent = 0;
		if (Error())
		{
			if (LastError() != wxSOCKET_WOULDBLOCK)
			{
				DoClose();
				return false;
			}
		}
		else
			numsent = LastCount();

		if (numsent)
			m_pEngine->SetActive(false);

		if (numsent < len)
		{
			char *tmp = m_pSendBuffer;
			m_pSendBuffer = new char[m_nSendBufferLen + len - numsent];
			memcpy(m_pSendBuffer, tmp, m_nSendBufferLen);
			memcpy(m_pSendBuffer + m_nSendBufferLen, buffer, len - numsent);
			m_nSendBufferLen += len - numsent;
			delete [] tmp;
		}
	}

	return true;
}

wxString CControlSocket::ConvertDomainName(wxString domain)
{
	const wxWCharBuffer buffer = wxConvCurrent->cWX2WC(domain);

	int len = 0;
	while (buffer.data()[len])
		len++;

	char *utf8 = new char[len * 2 + 2];
	wxMBConvUTF8 conv;
	conv.WC2MB(utf8, buffer, len * 2 + 2);

	char *output;
	if (idna_to_ascii_8z(utf8, &output, IDNA_ALLOW_UNASSIGNED))
	{
		delete [] utf8;
		LogMessage(::Debug_Warning, _T("Could not convert domain name"));
		return domain;
	}
	delete [] utf8;

	wxString result = wxConvCurrent->cMB2WX(output);
	free(output);
	return result;
}

void CControlSocket::Cancel()
{
	if (GetCurrentCommandId() != cmd_none)
	{
		if (GetCurrentCommandId() == cmd_connect)
			DoClose(FZ_REPLY_CANCELED);
		else
			ResetOperation(FZ_REPLY_CANCELED);
		LogMessage(::Error, _("Interrupted by user"));
	}
}

void CControlSocket::ResetTransferStatus()
{
	delete m_pTransferStatus;
	m_pTransferStatus = 0;

	m_pEngine->AddNotification(new CTransferStatusNotification(0));

	m_transferStatusSendState = 0;
}

void CControlSocket::InitTransferStatus(wxFileOffset totalSize, wxFileOffset startOffset)
{
	delete m_pTransferStatus;
	m_pTransferStatus = new CTransferStatus();

	m_pTransferStatus->started = wxDateTime::Now();
	m_pTransferStatus->totalSize = totalSize;
	m_pTransferStatus->startOffset = startOffset;
	m_pTransferStatus->currentOffset = startOffset;
}

void CControlSocket::UpdateTransferStatus(wxFileOffset transferredBytes)
{
	if (!m_pTransferStatus)
		return;

	m_pTransferStatus->currentOffset += transferredBytes;

	if (!m_transferStatusSendState)
		m_pEngine->AddNotification(new CTransferStatusNotification(new CTransferStatus(*m_pTransferStatus)));
	m_transferStatusSendState = 2;
}

bool CControlSocket::GetTransferStatus(CTransferStatus &status, bool &changed)
{
	if (!m_pTransferStatus)
	{
		changed = false;
		m_transferStatusSendState = 0;
		return false;
	}

	status = *m_pTransferStatus;
	if (m_transferStatusSendState == 2)
	{
		changed = true;
		m_transferStatusSendState = 1;
		return true;
	}
	else
	{
		changed = false;
		m_transferStatusSendState = 0;
		return true;
	}
}


const CServer* CControlSocket::GetCurrentServer() const
{
	return m_pCurrentServer;
}

void CControlSocket::SendDirectoryListing(CDirectoryListing* pListing)
{
	m_pEngine->SendDirectoryListing(pListing);
}
