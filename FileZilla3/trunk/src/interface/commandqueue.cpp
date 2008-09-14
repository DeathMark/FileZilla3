#include "FileZilla.h"
#include "commandqueue.h"
#include "Mainfrm.h"
#include "state.h"
#include "recursive_operation.h"
#include "loginmanager.h"
#include "queue.h"

DEFINE_EVENT_TYPE(fzEVT_GRANTEXCLUSIVEENGINEACCESS)

CCommandQueue::CCommandQueue(CFileZillaEngine *pEngine, CMainFrame* pMainFrame)
{
	m_pEngine = pEngine;
	m_pMainFrame = pMainFrame;
	m_exclusiveEngineRequest = false;
	m_exclusiveEngineLock = false;
	m_requestId = 0;
	m_inside_commandqueue = false;
}

CCommandQueue::~CCommandQueue()
{
	for (std::list<CCommand *>::iterator iter = m_CommandList.begin(); iter != m_CommandList.end(); iter++)
		delete *iter;
}

bool CCommandQueue::Idle() const
{
	return m_CommandList.empty() && !m_exclusiveEngineLock;
}

void CCommandQueue::ProcessCommand(CCommand *pCommand)
{
	m_CommandList.push_back(pCommand);
	if (m_CommandList.size() == 1)
	{
		m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
		ProcessNextCommand();
	}
}

void CCommandQueue::ProcessNextCommand()
{
	if (m_inside_commandqueue)
		return;

	if (m_exclusiveEngineLock)
		return;

	if (m_pEngine->IsBusy())
		return;

	m_inside_commandqueue = true;

	while (!m_CommandList.empty())
	{
		CCommand *pCommand = m_CommandList.front();

		int res = m_pEngine->Command(*pCommand);
		
		if (pCommand->GetId() == cmd_connect)
		{
			if (res == FZ_REPLY_WOULDBLOCK || res == FZ_REPLY_OK)
			{
				const CConnectCommand* pConnectCommand = (const CConnectCommand *)pCommand;
				const CServer& server = pConnectCommand->GetServer();
				m_pMainFrame->GetState()->SetServer(&server);
			}
		}
		else if (pCommand->GetId() == cmd_disconnect)
		{
			m_pMainFrame->GetState()->SetServer(0);
		}
		else if (pCommand->GetId() != cmd_cancel)
		{
			if (res == FZ_REPLY_NOTCONNECTED)
			{
				// Try automatic reconnect
				const CServer* pServer = m_pMainFrame->GetState()->GetServer();
				if (pServer)
				{
					CCommand *pCommand = new CConnectCommand(*pServer);
					m_CommandList.push_front(pCommand);
					continue;
				}
			}
		}

		if (res == FZ_REPLY_WOULDBLOCK)
			break;
		else if (res == FZ_REPLY_OK)
		{
			delete pCommand;
			m_CommandList.pop_front();
			continue;
		}
		else if (res == FZ_REPLY_ALREADYCONNECTED)
		{
			m_pMainFrame->GetState()->SetServer(0);
			res = m_pEngine->Command(CDisconnectCommand());
			if (res == FZ_REPLY_WOULDBLOCK)
			{
				m_CommandList.push_front(new CDisconnectCommand);
				break;
			}
			else if (res != FZ_REPLY_OK)
			{
				wxBell();
				delete pCommand;
				m_CommandList.pop_front();
				continue;
			}
		}
		else
		{
			wxBell();
			
			// Let the remote list view know if a LIST command failed,
			// so that it may issue the next command in recursive operations.
			if (pCommand->GetId() == cmd_list)
				m_pMainFrame->GetState()->GetRecursiveOperationHandler()->ListingFailed(res);

			m_CommandList.pop_front();
			delete pCommand;
		}
	}

	m_inside_commandqueue = false;

	if (m_CommandList.empty())
	{
		if (m_exclusiveEngineRequest)
			GrantExclusiveEngineRequest();
		else
			m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
	}
}

bool CCommandQueue::Cancel()
{
	if (m_exclusiveEngineLock)
		return false;

	if (m_CommandList.empty())
		return true;
	
	std::list<CCommand *>::iterator iter = m_CommandList.begin();
	CCommand *pCommand = *(iter++);

	for (; iter != m_CommandList.end(); iter++)
		delete *iter;

	m_CommandList.clear();
	m_CommandList.push_back(pCommand);

	if (!m_pEngine)
	{
		delete pCommand;
		m_CommandList.clear();
		m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
		return true;
	}

	int res = m_pEngine->Command(CCancelCommand());
	if (res == FZ_REPLY_WOULDBLOCK)
		return false;
	else
	{
		delete pCommand;
		m_CommandList.clear();
		m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
		return true;
	}
}

void CCommandQueue::Finish(COperationNotification *pNotification)
{
	if (pNotification->nReplyCode & FZ_REPLY_DISCONNECTED)
	{
		if (pNotification->commandId == cmd_none && !m_CommandList.empty())
		{
			// Pending event, has no relevance during command execution
			delete pNotification;
			return;
		}
		if (pNotification->nReplyCode & FZ_REPLY_PASSWORDFAILED)
			CLoginManager::Get().CachedPasswordFailed(*m_pMainFrame->GetState()->GetServer());
	}

	if (m_exclusiveEngineLock)
	{
		m_pMainFrame->GetQueue()->ProcessNotification(pNotification);
		return;
	}

	if (m_CommandList.empty())
	{
		delete pNotification;
		return;
	}

	wxASSERT(!m_inside_commandqueue);
	m_inside_commandqueue = true;

	CCommand* pCommand = m_CommandList.front();

	// Let the remote list view know if a LIST command failed,
	// so that it may issue the next command in recursive operations.
	if (pCommand->GetId() == cmd_list && pNotification->nReplyCode != FZ_REPLY_OK)
	{
		m_pMainFrame->GetState()->GetRecursiveOperationHandler()->ListingFailed(pNotification->nReplyCode);
		m_CommandList.pop_front();
	}
	else if (pCommand->GetId() == cmd_connect && pNotification->nReplyCode != FZ_REPLY_OK)
	{
		// Remove pending events
		m_CommandList.pop_front();
		while (!m_CommandList.empty())
		{
			CCommand* pPendingCommand = m_CommandList.front();
			if (pPendingCommand->GetId() == cmd_connect)
				break;
			m_CommandList.pop_front();
			delete pPendingCommand;
		}

		// If this was an automatic reconnect during a recursive
		// operation, stop the recursive operation
		m_pMainFrame->GetState()->GetRecursiveOperationHandler()->StopRecursiveOperation();
	}
	else
		m_CommandList.pop_front();
	
	delete pCommand;
	
	delete pNotification;

	m_inside_commandqueue = false;

	ProcessNextCommand();
}

void CCommandQueue::RequestExclusiveEngine(bool requestExclusive)
{
	wxASSERT(!m_exclusiveEngineLock || !requestExclusive);

	if (!m_exclusiveEngineRequest && requestExclusive)
	{
		m_requestId++;
		if (m_requestId < 0)
			m_requestId = 0;
		if (m_CommandList.empty())
		{
			m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
			GrantExclusiveEngineRequest();
			return;
		}
	}
	if (!requestExclusive)
		m_exclusiveEngineLock = false;
	m_exclusiveEngineRequest = requestExclusive;
	m_pMainFrame->GetState()->NotifyHandlers(STATECHANGE_REMOTE_IDLE);
}

void CCommandQueue::GrantExclusiveEngineRequest()
{
	wxASSERT(!m_exclusiveEngineLock);
	m_exclusiveEngineLock = true;
	m_exclusiveEngineRequest = false;

	wxCommandEvent evt(fzEVT_GRANTEXCLUSIVEENGINEACCESS);
	evt.SetId(m_requestId);
	m_pMainFrame->GetQueue()->AddPendingEvent(evt);
}

CFileZillaEngine* CCommandQueue::GetEngineExclusive(int requestId)
{
	if (!m_exclusiveEngineLock)
		return 0;

	if (requestId != m_requestId)
		return 0;

	return m_pEngine;
}


void CCommandQueue::ReleaseEngine()
{
	m_exclusiveEngineLock = false;

	ProcessNextCommand();
}
