// FileZillaEngine.cpp: Implementierung der Klasse CFileZillaEngine.
//
//////////////////////////////////////////////////////////////////////

#include <filezilla.h>
#include "ControlSocket.h"
#include "directorycache.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CFileZillaEngine::CFileZillaEngine(CFileZillaEngineContext& engine_context)
	: CFileZillaEnginePrivate(engine_context)
{
}

CFileZillaEngine::~CFileZillaEngine()
{
	// fixme: Might be too late? Two-phase shutdown perhaps?
	RemoveHandler();
	m_maySendNotificationEvent = false;
}

int CFileZillaEngine::Init(wxEvtHandler *pEventHandler)
{
	wxCriticalSectionLocker lock(mutex_);
	m_pEventHandler = pEventHandler;
	return FZ_REPLY_OK;
}

int CFileZillaEngine::Execute(const CCommand &command)
{
	wxCriticalSectionLocker lock(mutex_);
	if (command.GetId() != Command::cancel && IsBusy())
		return FZ_REPLY_BUSY;

	if (!command.valid()) {
		return FZ_REPLY_SYNTAXERROR;
	}

	m_bIsInCommand = true;

	int res = FZ_REPLY_INTERNALERROR;
	switch (command.GetId())
	{
	case Command::connect:
		res = Connect(reinterpret_cast<const CConnectCommand &>(command));
		break;
	case Command::disconnect:
		res = Disconnect(reinterpret_cast<const CDisconnectCommand &>(command));
		break;
	case Command::cancel:
		res = Cancel(reinterpret_cast<const CCancelCommand &>(command));
		break;
	case Command::list:
		res = List(reinterpret_cast<const CListCommand &>(command));
		break;
	case Command::transfer:
		res = FileTransfer(reinterpret_cast<const CFileTransferCommand &>(command));
		break;
	case Command::raw:
		res = RawCommand(reinterpret_cast<const CRawCommand&>(command));
		break;
	case Command::del:
		res = Delete(reinterpret_cast<const CDeleteCommand&>(command));
		break;
	case Command::removedir:
		res = RemoveDir(reinterpret_cast<const CRemoveDirCommand&>(command));
		break;
	case Command::mkdir:
		res = Mkdir(reinterpret_cast<const CMkdirCommand&>(command));
		break;
	case Command::rename:
		res = Rename(reinterpret_cast<const CRenameCommand&>(command));
		break;
	case Command::chmod:
		res = Chmod(reinterpret_cast<const CChmodCommand&>(command));
		break;
	default:
		return FZ_REPLY_SYNTAXERROR;
	}

	if (res != FZ_REPLY_WOULDBLOCK)
		ResetOperation(res);

	m_bIsInCommand = false;

	if (command.GetId() != Command::disconnect)
		res |= m_nControlSocketError;
	else if (res & FZ_REPLY_DISCONNECTED)
		res = FZ_REPLY_OK;
	m_nControlSocketError = 0;

	return res;
}

CNotification* CFileZillaEngine::GetNextNotification()
{
	wxCriticalSectionLocker lock(notification_mutex_);

	if (m_NotificationList.empty()) {
		m_maySendNotificationEvent = true;
		return 0;
	}
	CNotification* pNotification = m_NotificationList.front();
	m_NotificationList.pop_front();

	return pNotification;
}

bool CFileZillaEngine::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	wxCriticalSectionLocker lock(mutex_);
	if (!pNotification)
		return false;
	if (!IsBusy())
		return false;

	notification_mutex_.Enter();
	if (pNotification->requestNumber != m_asyncRequestCounter)
	{
		notification_mutex_.Leave();
		return false;
	}
	notification_mutex_.Leave();

	if (!m_pControlSocket)
		return false;

	m_pControlSocket->SetAlive();
	if (!m_pControlSocket->SetAsyncRequestReply(pNotification))
		return false;

	return true;
}

bool CFileZillaEngine::IsPendingAsyncRequestReply(const CAsyncRequestNotification *pNotification)
{
	if (!pNotification)
		return false;

	if (!IsBusy())
		return false;

	wxCriticalSectionLocker lock(notification_mutex_);
	return pNotification->requestNumber == m_asyncRequestCounter;
}

bool CFileZillaEngine::IsActive(enum CFileZillaEngine::_direction direction)
{
	wxCriticalSectionLocker lock(mutex_);

	if (m_activeStatus[direction] == 2) {
		m_activeStatus[direction] = 1;
		return true;
	}

	m_activeStatus[direction] = 0;
	return false;
}

bool CFileZillaEngine::GetTransferStatus(CTransferStatus &status, bool &changed)
{
	wxCriticalSectionLocker lock(mutex_);

	if (!m_pControlSocket) {
		changed = false;
		return false;
	}

	return m_pControlSocket->GetTransferStatus(status, changed);
}

int CFileZillaEngine::CacheLookup(const CServerPath& path, CDirectoryListing& listing)
{
	// TODO: Possible optimization: Atomically get current server. The cache has its own mutex.
	wxCriticalSectionLocker lock(mutex_);

	if (!IsConnected())
		return FZ_REPLY_ERROR;

	wxASSERT(m_pControlSocket->GetCurrentServer());

	bool is_outdated = false;
	if (!directory_cache_.Lookup(listing, *m_pControlSocket->GetCurrentServer(), path, true, is_outdated))
		return FZ_REPLY_ERROR;

	return FZ_REPLY_OK;
}
