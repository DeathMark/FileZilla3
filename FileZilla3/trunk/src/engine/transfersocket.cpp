#include <filezilla.h>
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "ftpcontrolsocket.h"
#include "iothread.h"
#include "optionsbase.h"
#include "tlssocket.h"
#include "transfersocket.h"
#include <errno.h>
#include "proxy.h"
#include "servercapabilities.h"

CTransferSocket::CTransferSocket(CFileZillaEnginePrivate *pEngine, CFtpControlSocket *pControlSocket, TransferMode transferMode)
: CEventHandler(pEngine->socket_event_dispatcher_.event_loop_)
, CSocketEventHandler(pEngine->socket_event_dispatcher_)
{
	m_pEngine = pEngine;
	m_pControlSocket = pControlSocket;

	m_pSocketServer = 0;
	m_pSocket = 0;
	m_pBackend = 0;
	m_pProxyBackend = 0;
	m_pTlsSocket = 0;

	m_pDirectoryListingParser = 0;

	m_bActive = false;

	m_transferMode = transferMode;

	m_pTransferBuffer = 0;
	m_transferBufferLen = 0;

	m_transferEndReason = TransferEndReason::none;
	m_binaryMode = true;

	m_onCloseCalled = false;

	m_postponedReceive = false;
	m_postponedSend = false;

	m_shutdown = false;

	m_madeProgress = 0;
}

CTransferSocket::~CTransferSocket()
{
	RemoveHandler();
	if (m_transferEndReason == TransferEndReason::none)
		m_transferEndReason = TransferEndReason::successful;
	ResetSocket();

	if (m_pControlSocket) {
		if (m_transferMode == TransferMode::upload || m_transferMode == TransferMode::download) {
			CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
			if (pData && pData->pIOThread) {
				if (m_transferMode == TransferMode::download)
					FinalizeWrite();
				pData->pIOThread->SetEventHandler(0);
			}
		}
	}
}

void CTransferSocket::ResetSocket()
{
	delete m_pProxyBackend;
	if( m_pBackend == m_pTlsSocket ) {
		m_pBackend = 0;
	}
	delete m_pTlsSocket;
	delete m_pBackend;
	delete m_pSocketServer;
	delete m_pSocket;
	m_pProxyBackend = 0;
	m_pTlsSocket = 0;
	m_pBackend = 0;
	m_pSocketServer = 0;
	m_pSocket = 0;

}

wxString CTransferSocket::SetupActiveTransfer(const wxString& ip)
{
	ResetSocket();
	m_pSocketServer = CreateSocketServer();

	if (!m_pSocketServer) {
		m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("CreateSocketServer failed"));
		return wxString();
	}

	int error;
	int port = m_pSocketServer->GetLocalPort(error);
	if (port == -1)	{
		ResetSocket();

		m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("GetLocalPort failed: %s"), CSocket::GetErrorDescription(error));
		return wxString();
	}

	wxString portArguments;
	if (m_pSocketServer->GetAddressFamily() == CSocket::ipv6) {
		portArguments = wxString::Format(_T("|2|%s|%d|"), ip, port);
	}
	else {
		portArguments = ip;
		portArguments += wxString::Format(_T(",%d,%d"), port / 256, port % 256);
		portArguments.Replace(_T("."), _T(","));
	}

	return portArguments;
}

void CTransferSocket::OnSocketEvent(CSocketEvent &event)
{
	if (m_pProxyBackend)
	{
		switch (event.GetType())
		{
		case CSocketEvent::connection:
			{
				const int error = event.GetError();
				if (error) {
					m_pControlSocket->LogMessage(MessageType::Error, _("Proxy handshake failed: %s"), CSocket::GetErrorDescription(error));
					TransferEnd(TransferEndReason::failure);
				}
				else {
					delete m_pProxyBackend;
					m_pProxyBackend = 0;
					OnConnect();
				}
			}
			return;
		case CSocketEvent::close:
			{
				const int error = event.GetError();
				m_pControlSocket->LogMessage(MessageType::Error, _("Proxy handshake failed: %s"), CSocket::GetErrorDescription(error));
				TransferEnd(TransferEndReason::failure);
			}
			return;
		default:
			// Uninteresting
			return;
		}
		return;
	}

	if (m_pSocketServer) {
		if (event.GetType() == CSocketEvent::connection)
			OnAccept(event.GetError());
		else
			m_pControlSocket->LogMessage(MessageType::Debug_Info, _T("Unhandled socket event %d from listening socket"), event.GetType());
		return;
	}

	switch (event.GetType())
	{
	case CSocketEvent::connection:
		if (event.GetError()) {
			if (m_transferEndReason == TransferEndReason::none) {
				m_pControlSocket->LogMessage(MessageType::Error, _("The data connection could not be established: %s"), CSocket::GetErrorDescription(event.GetError()));
				TransferEnd(TransferEndReason::transfer_failure);
			}
		}
		else
			OnConnect();
		break;
	case CSocketEvent::read:
		OnReceive();
		break;
	case CSocketEvent::write:
		OnSend();
		break;
	case CSocketEvent::close:
		OnClose(event.GetError());
		break;
	case CSocketEvent::hostaddress:
		// Booooring. No seriously, we connect by IP already, nothing to resolve
		break;
	default:
		m_pControlSocket->LogMessage(MessageType::Debug_Info, _T("Unhandled socket event %d"), event.GetType());
		break;
	}
}

void CTransferSocket::OnAccept(int error)
{
	m_pControlSocket->SetAlive();
	m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("CTransferSocket::OnAccept(%d)"), error);

	if (!m_pSocketServer)
	{
		m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("No socket server in OnAccept"), error);
		return;
	}

	m_pSocket = m_pSocketServer->Accept(error);
	if (!m_pSocket)
	{
		if (error == EAGAIN)
			m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("No pending connection"));
		else {
			m_pControlSocket->LogMessage(MessageType::Status, _("Could not accept connection: %s"), CSocket::GetErrorDescription(error));
			TransferEnd(TransferEndReason::transfer_failure);
		}
		return;
	}
	delete m_pSocketServer;
	m_pSocketServer = 0;

	OnConnect();
}

void CTransferSocket::OnConnect()
{
	m_pControlSocket->SetAlive();
	m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("CTransferSocket::OnConnect"));

	if (!m_pSocket)
	{
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("CTransferSocket::OnConnect called without socket"));
		return;
	}

	if (!m_pBackend)
	{
		if (!InitBackend())
		{
			TransferEnd(TransferEndReason::transfer_failure);
			return;
		}
	}
	else if (m_pTlsSocket) {
		// Re-enable Nagle algorithm
		m_pSocket->SetFlags(m_pSocket->GetFlags() & (~CSocket::flag_nodelay));
		if (CServerCapabilities::GetCapability(*m_pControlSocket->m_pCurrentServer, tls_resume) == unknown)	{
			CServerCapabilities::SetCapability(*m_pControlSocket->m_pCurrentServer, tls_resume, m_pTlsSocket->ResumedSession() ? yes : no);
		}
	}

	if (m_bActive)
		TriggerPostponedEvents();
}

void CTransferSocket::OnReceive()
{
	m_pControlSocket->LogMessage(MessageType::Debug_Debug, _T("CTransferSocket::OnReceive(), m_transferMode=%d"), m_transferMode);

	if (!m_pBackend)
	{
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Postponing receive, m_pBackend was false."));
		m_postponedReceive = true;
		return;
	}

	if (!m_bActive)
	{
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Postponing receive, m_bActive was false."));
		m_postponedReceive = true;
		return;
	}

	if (m_transferMode == TransferMode::list)
	{
		for (;;)
		{
			char *pBuffer = new char[4096];
			int error;
			int numread = m_pBackend->Read(pBuffer, 4096, error);
			if (numread < 0)
			{
				delete [] pBuffer;
				if (error != EAGAIN) {
					m_pControlSocket->LogMessage(MessageType::Error, _T("Could not read from transfer socket: %s"), CSocket::GetErrorDescription(error));
					TransferEnd(TransferEndReason::transfer_failure);
				}
				else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
					TransferEnd(TransferEndReason::successful);
				return;
			}

			if (numread > 0)
			{
				if (!m_pDirectoryListingParser->AddData(pBuffer, numread))
				{
					TransferEnd(TransferEndReason::transfer_failure);
					return;
				}

				m_pControlSocket->SetActive(CFileZillaEngine::recv);
				if (!m_madeProgress) {
					m_madeProgress = 2;
					m_pEngine->transfer_status_.SetMadeProgress();
				}
				m_pEngine->transfer_status_.Update(numread);
			}
			else
			{
				delete [] pBuffer;
				TransferEnd(TransferEndReason::successful);
				return;
			}
		}
	}
	else if (m_transferMode == TransferMode::download)
	{
		for (;;)
		{
			if (!CheckGetNextWriteBuffer())
				return;

			int error;
			int numread = m_pBackend->Read(m_pTransferBuffer, m_transferBufferLen, error);
			if (numread < 0)
			{
				if (error != EAGAIN) {
					m_pControlSocket->LogMessage(MessageType::Error, _T("Could not read from transfer socket: %s"), CSocket::GetErrorDescription(error));
					TransferEnd(TransferEndReason::transfer_failure);
				}
				else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
					TransferEnd(TransferEndReason::successful);
				return;
			}

			if (numread > 0) {
				m_pControlSocket->SetActive(CFileZillaEngine::recv);
				if (!m_madeProgress) {
					m_madeProgress = 2;
					m_pEngine->transfer_status_.SetMadeProgress();
				}
				m_pEngine->transfer_status_.Update(numread);

				m_pTransferBuffer += numread;
				m_transferBufferLen -= numread;

				if (!CheckGetNextWriteBuffer())
					return;
			}
			else //!numread
			{
				FinalizeWrite();
				break;
			}
		}
	}
	else if (m_transferMode == TransferMode::resumetest)
	{
		for (;;)
		{
			char buffer[2];
			int error;
			int numread = m_pBackend->Read(buffer, 2, error);
			if (numread < 0)
			{
				if (error != EAGAIN) {
					m_pControlSocket->LogMessage(MessageType::Error, _T("Could not read from transfer socket: %s"), CSocket::GetErrorDescription(error));
					TransferEnd(TransferEndReason::transfer_failure);
				}
				else if (m_onCloseCalled && !m_pBackend->IsWaiting(CRateLimiter::inbound))
				{
					if (m_transferBufferLen == 1)
						TransferEnd(TransferEndReason::successful);
					else
					{
						m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("Server incorrectly sent %d bytes"), m_transferBufferLen);
						TransferEnd(TransferEndReason::failed_resumetest);
					}
				}
				return;
			}

			if (!numread)
			{
				if (m_transferBufferLen == 1)
					TransferEnd(TransferEndReason::successful);
				else
				{
					m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("Server incorrectly sent %d bytes"), m_transferBufferLen);
					TransferEnd(TransferEndReason::failed_resumetest);
				}
				return;
			}
			m_transferBufferLen += numread;

			if (m_transferBufferLen > 1)
			{
				m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("Server incorrectly sent %d bytes"), m_transferBufferLen);
				TransferEnd(TransferEndReason::failed_resumetest);
				return;
			}
		}
	}
}

void CTransferSocket::OnSend()
{
	if (!m_pBackend) {
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("OnSend called without backend. Ignoring event."));
		return;
	}

	if (!m_bActive) {
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Postponing send"));
		m_postponedSend = true;
		return;
	}

	if (m_transferMode != TransferMode::upload)
		return;

	int error;
	int written;

	// Only doe a certain number of iterations in one go to keep the event loop going.
	// Otherwise this behaves like a livelock on very large files read from a very fast
	// SSD uploaded to a very fast server.
	for (int i = 0; i < 100; ++i) {
		if (!CheckGetNextReadBuffer())
			return;

		written = m_pBackend->Write(m_pTransferBuffer, m_transferBufferLen, error);
		if (written <= 0)
			break;

		m_pControlSocket->SetActive(CFileZillaEngine::send);
		if (m_madeProgress == 1) {
			m_pControlSocket->LogMessage(MessageType::Debug_Debug, _T("Made progress in CTransferSocket::OnSend()"));
			m_madeProgress = 2;
			m_pEngine->transfer_status_.SetMadeProgress();
		}
		m_pEngine->transfer_status_.Update(written);

		m_pTransferBuffer += written;
		m_transferBufferLen -= written;
	}

	if (written < 0) {
		if (error == EAGAIN) {
			if (!m_madeProgress) {
				m_pControlSocket->LogMessage(MessageType::Debug_Debug, _T("First EAGAIN in CTransferSocket::OnSend()"));
				m_madeProgress = 1;
				m_pEngine->transfer_status_.SetMadeProgress();
			}
		}
		else {
			m_pControlSocket->LogMessage(MessageType::Error, _T("Could not write to transfer socket: %s"), CSocket::GetErrorDescription(error));
			TransferEnd(TransferEndReason::transfer_failure);
		}
	}
	else if (written > 0) {
		CSocketEvent *evt = new CSocketEvent(this, m_pSocket, CSocketEvent::write);
		dispatcher_.SendEvent(evt);
	}
}

void CTransferSocket::OnClose(int error)
{
	m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("CTransferSocket::OnClose(%d)"), error);
	m_onCloseCalled = true;

	if (m_transferEndReason != TransferEndReason::none)
		return;

	if (!m_pBackend) {
		if (!InitBackend()) {
			TransferEnd(TransferEndReason::transfer_failure);
			return;
		}
	}

	if (m_transferMode == TransferMode::upload)
	{
		if (m_shutdown && m_pTlsSocket)
		{
			if (m_pTlsSocket->Shutdown() != 0)
				TransferEnd(TransferEndReason::transfer_failure);
			else
				TransferEnd(TransferEndReason::successful);
		}
		else
			TransferEnd(TransferEndReason::transfer_failure);
		return;
	}

	if (error) {
		m_pControlSocket->LogMessage(MessageType::Error, _("Transfer connection interrupted: %s"), CSocket::GetErrorDescription(error));
		TransferEnd(TransferEndReason::transfer_failure);
		return;
	}

	char buffer[100];
	int numread = m_pBackend->Peek(&buffer, 100, error);
	if (numread > 0) {
#ifndef __WXMSW__
		wxFAIL_MSG(_T("Peek isn't supposed to return data after close notification"));
#endif

		// MSDN says this:
		//   FD_CLOSE being posted after all data is read from a socket.
		//   An application should check for remaining data upon receipt
		//   of FD_CLOSE to avoid any possibility of losing data.
		// First half is actually plain wrong.
		OnReceive();

		return;
	}
	else if (numread < 0 && error != EAGAIN) {
		m_pControlSocket->LogMessage(MessageType::Error, _("Transfer connection interrupted: %s"), CSocket::GetErrorDescription(error));
		TransferEnd(TransferEndReason::transfer_failure);
		return;
	}

	if (m_transferMode == TransferMode::resumetest) {
		if (m_transferBufferLen != 1) {
			TransferEnd(TransferEndReason::failed_resumetest);
			return;
		}
	}
	TransferEnd(TransferEndReason::successful);
}

bool CTransferSocket::SetupPassiveTransfer(wxString host, int port)
{
	ResetSocket();

	m_pSocket = new CSocket(this, dispatcher_);

	if (m_pControlSocket->m_pProxyBackend)
	{
		m_pProxyBackend = new CProxySocket(this, m_pSocket, m_pControlSocket);

		int res = m_pProxyBackend->Handshake(m_pControlSocket->m_pProxyBackend->GetProxyType(),
											 host, port,
											 m_pControlSocket->m_pProxyBackend->GetUser(), m_pControlSocket->m_pProxyBackend->GetPass());

		if (res != EINPROGRESS)
		{
			ResetSocket();
			return false;
		}
		int error;
		host = m_pControlSocket->m_pSocket->GetPeerIP();
		port = m_pControlSocket->m_pSocket->GetRemotePort(error);
		if( host.empty() || port < 1 ) {
			m_pControlSocket->LogMessage(MessageType::Debug_Warning, _T("Could not get peer address of control connection."));
			ResetSocket();
			return false;
		}
	}

	SetSocketBufferSizes(m_pSocket);

	m_pSocket->SetFlags(CSocket::flag_nodelay);
	int res = m_pSocket->Connect(host, port);
	if (res && res != EINPROGRESS)
	{
		ResetSocket();
		return false;
	}

	return true;
}

void CTransferSocket::SetActive()
{
	if (m_transferEndReason != TransferEndReason::none)
		return;
	if (m_transferMode == TransferMode::download || m_transferMode == TransferMode::upload)
	{
		CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
		if (pData && pData->pIOThread)
			pData->pIOThread->SetEventHandler(this);
	}

	m_bActive = true;
	if (!m_pSocket)
		return;

	if (m_pSocket->GetState() == CSocket::connected || m_pSocket->GetState() == CSocket::closing)
		TriggerPostponedEvents();
}

void CTransferSocket::TransferEnd(TransferEndReason reason)
{
	m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("CTransferSocket::TransferEnd(%d)"), reason);

	if (m_transferEndReason != TransferEndReason::none)
		return;
	m_transferEndReason = reason;

	ResetSocket();

	m_pEngine->SendEvent<CFileZillaEngineEvent>(engineTransferEnd);
}

CSocket* CTransferSocket::CreateSocketServer(int port)
{
	CSocket* pServer = new CSocket(this, dispatcher_);
	int res = pServer->Listen(m_pControlSocket->m_pSocket->GetAddressFamily(), port);
	if (res) {
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Could not listen on port %d: %s"), port, CSocket::GetErrorDescription(res));
		delete pServer;
		return 0;
	}

	SetSocketBufferSizes(pServer);

	return pServer;
}

CSocket* CTransferSocket::CreateSocketServer()
{
	if (!m_pEngine->GetOptions().GetOptionVal(OPTION_LIMITPORTS))
	{
		// Ask the systen for a port
		CSocket* pServer = CreateSocketServer(0);
		return pServer;
	}

	// Try out all ports in the port range.
	// Upon first call, we try to use a random port fist, after that
	// increase the port step by step

	// Windows only: I think there's a bug in the socket implementation of
	// Windows: Even if using SO_REUSEADDR, using the same local address
	// twice will fail unless there are a couple of minutes between the
	// connection attempts. This may cause problems if transferring lots of
	// files with a narrow port range.

	static int start = 0;

	int low = m_pEngine->GetOptions().GetOptionVal(OPTION_LIMITPORTS_LOW);
	int high = m_pEngine->GetOptions().GetOptionVal(OPTION_LIMITPORTS_HIGH);
	if (low > high)
		low = high;

	if (start < low || start > high)
	{
		start = GetRandomNumber(low, high);
		wxASSERT(start >= low && start <= high);
	}

	CSocket* pServer = 0;

	int count = high - low + 1;
	while (count--)
	{
		pServer = CreateSocketServer(start++);
		if (pServer)
			break;
		if (start > high)
			start = low;
	}

	return pServer;
}

bool CTransferSocket::CheckGetNextWriteBuffer()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
	if (!m_transferBufferLen) {
		int res = pData->pIOThread->GetNextWriteBuffer(&m_pTransferBuffer);

		if (res == IO_Again)
			return false;
		else if (res == IO_Error) {
			wxString error = pData->pIOThread->GetError();
			if (error.empty() )
				m_pControlSocket->LogMessage(MessageType::Error, _("Can't write data to file."));
			else
				m_pControlSocket->LogMessage(MessageType::Error, _("Can't write data to file: %s"), error);
			TransferEnd(TransferEndReason::transfer_failure_critical);
			return false;
		}

		m_transferBufferLen = BUFFERSIZE;
	}

	return true;
}

bool CTransferSocket::CheckGetNextReadBuffer()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
	if (!m_transferBufferLen) {
		int res = pData->pIOThread->GetNextReadBuffer(&m_pTransferBuffer);
		if (res == IO_Again)
			return false;
		else if (res == IO_Error)
		{
			m_pControlSocket->LogMessage(MessageType::Error, _("Can't read from file"));
			TransferEnd(TransferEndReason::transfer_failure);
			return false;
		}
		else if (res == IO_Success)
		{
			if (m_pTlsSocket)
			{
				m_shutdown = true;

				int error = m_pTlsSocket->Shutdown();
				if (error != 0)
				{
					if (error != EAGAIN)
						TransferEnd(TransferEndReason::transfer_failure);
					return false;
				}
			}
			TransferEnd(TransferEndReason::successful);
			return false;
		}
		m_transferBufferLen = res;
	}

	return true;
}

void CTransferSocket::OnIOThreadEvent()
{
	if (!m_bActive || m_transferEndReason != TransferEndReason::none)
		return;

	if (m_transferMode == TransferMode::download)
		OnReceive();
	else if (m_transferMode == TransferMode::upload)
		OnSend();
}

void CTransferSocket::FinalizeWrite()
{
	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(static_cast<CRawTransferOpData *>(m_pControlSocket->m_pCurOpData)->pOldData);
	bool res = pData->pIOThread->Finalize(BUFFERSIZE - m_transferBufferLen);
	if (m_transferEndReason != TransferEndReason::none)
		return;

	if (res)
		TransferEnd(TransferEndReason::successful);
	else
	{
		wxString error = pData->pIOThread->GetError();
		if (error.empty())
			m_pControlSocket->LogMessage(MessageType::Error, _("Can't write data to file."));
		else
			m_pControlSocket->LogMessage(MessageType::Error, _("Can't write data to file: %s"), error);
		TransferEnd(TransferEndReason::transfer_failure_critical);
	}
}

bool CTransferSocket::InitTls(const CTlsSocket* pPrimaryTlsSocket)
{
	// Disable Nagle algorithm during TlS handshake
	m_pSocket->SetFlags(m_pSocket->GetFlags() | CSocket::flag_nodelay);

	wxASSERT(!m_pBackend);
	m_pTlsSocket = new CTlsSocket(this, m_pSocket, m_pControlSocket);

	if (!m_pTlsSocket->Init()) {
		delete m_pTlsSocket;
		m_pTlsSocket = 0;
		return false;
	}

	bool try_resume = CServerCapabilities::GetCapability(*m_pControlSocket->m_pCurrentServer, tls_resume) != no;

	int res = m_pTlsSocket->Handshake(pPrimaryTlsSocket, try_resume);
	if (res && res != FZ_REPLY_WOULDBLOCK)
	{
		delete m_pTlsSocket;
		m_pTlsSocket = 0;
		return false;
	}

	m_pBackend = m_pTlsSocket;

	return true;
}

void CTransferSocket::TriggerPostponedEvents()
{
	wxASSERT(m_bActive);

	if (m_postponedReceive) {
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Executing postponed receive"));
		m_postponedReceive = false;
		OnReceive();
		if (m_transferEndReason != TransferEndReason::none)
			return;
	}
	if (m_postponedSend) {
		m_pControlSocket->LogMessage(MessageType::Debug_Verbose, _T("Executing postponed send"));
		m_postponedSend = false;
		OnSend();
		if (m_transferEndReason != TransferEndReason::none)
			return;
	}
	if (m_onCloseCalled)
		OnClose(0);
}

bool CTransferSocket::InitBackend()
{
	if (m_pBackend)
		return true;

	if (m_pControlSocket->m_protectDataChannel) {
		if (!InitTls(m_pControlSocket->m_pTlsSocket))
			return false;
	}
	else
		m_pBackend = new CSocketBackend(this, m_pSocket, m_pEngine->GetRateLimiter());

	return true;
}

void CTransferSocket::SetSocketBufferSizes(CSocket* pSocket)
{
	wxCHECK_RET(pSocket, _("SetSocketBufferSize called without socket"));

	const int size_read = m_pEngine->GetOptions().GetOptionVal(OPTION_SOCKET_BUFFERSIZE_RECV);
	const int size_write = m_pEngine->GetOptions().GetOptionVal(OPTION_SOCKET_BUFFERSIZE_SEND);
	pSocket->SetBufferSizes(size_read, size_write);
}

void CTransferSocket::operator()(CEventBase const& ev)
{
	Dispatch<CIOThreadEvent>(ev, this, &CTransferSocket::OnIOThreadEvent);
}
