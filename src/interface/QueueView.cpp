#include "FileZilla.h"
#include "queue.h"
#include "Mainfrm.h"
#include "Options.h"
#include "StatusView.h"
#include "statuslinectrl.h"
#include "../tinyxml/tinyxml.h"
#include "xmlfunctions.h"
#include "filezillaapp.h"
#include "ipcmutex.h"
#include "state.h"
#include "asyncrequestqueue.h"
#include "defaultfileexistsdlg.h"
#include "filter.h"
#include <wx/dnd.h>
#include "dndobjects.h"
#include "loginmanager.h"
#include "aui_notebook_ex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

class CQueueViewDropTarget : public wxDropTarget
{
public:
	CQueueViewDropTarget(CQueueView* pQueueView)
		: m_pQueueView(pQueueView), m_pFileDataObject(new wxFileDataObject()),
		m_pRemoteDataObject(new CRemoteDataObject())
	{
		m_pDataObject = new wxDataObjectComposite;
		m_pDataObject->Add(m_pRemoteDataObject, true);
		m_pDataObject->Add(m_pFileDataObject, false);
		SetDataObject(m_pDataObject);
	}

	virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def)
	{
		if (def == wxDragError ||
			def == wxDragNone ||
			def == wxDragCancel)
			return def;

		if (!GetData())
			return wxDragError;

		if (m_pDataObject->GetReceivedFormat() == m_pFileDataObject->GetFormat())
		{
			CState* const pState = m_pQueueView->m_pMainFrame->GetState();
			const CServer* const pServer = pState->GetServer();
			if (!pServer)
				return wxDragNone;

			const CServerPath& path = pState->GetRemotePath();
			if (path.IsEmpty())
				return wxDragNone;

			pState->UploadDroppedFiles(m_pFileDataObject, path, true);
		}
		else
		{
			if (m_pRemoteDataObject->GetProcessId() != wxGetProcessId())
			{
				wxMessageBox(_("Drag&drop between different instances of FileZilla has not been implemented yet."));
				return wxDragNone;
			}

			CState* const pState = m_pQueueView->m_pMainFrame->GetState();
			const CServer* const pServer = pState->GetServer();
			if (!pServer)
				return wxDragNone;

			if (*pServer != m_pRemoteDataObject->GetServer())
			{
				wxMessageBox(_("Drag&drop between different servers has not been implemented yet."));
				return wxDragNone;
			}

			const wxString& target = pState->GetLocalDir();
#ifdef __WXMSW__
			if (target == _T("\\"))
			{
				wxBell();
				return wxDragNone;
			}
#endif

			if (!pState->DownloadDroppedFiles(m_pRemoteDataObject, target, true))
				return wxDragNone;
		}

		return def;
	}

	virtual bool OnDrop(wxCoord x, wxCoord y)
	{
		return true;
	}

	virtual wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult def)
	{
		if (def == wxDragError ||
			def == wxDragNone ||
			def == wxDragCancel)
		{
			return def;
		}

		def = wxDragCopy;

		return def;
	}

	virtual void OnLeave()
	{
	}

	virtual wxDragResult OnEnter(wxCoord x, wxCoord y, wxDragResult def)
	{
		return OnDragOver(x, y, def);
	}

protected:
	CQueueView *m_pQueueView;
	wxFileDataObject* m_pFileDataObject;
	CRemoteDataObject* m_pRemoteDataObject;
	wxDataObjectComposite* m_pDataObject;
};

DECLARE_EVENT_TYPE(fzEVT_FOLDERTHREAD_COMPLETE, -1)
DEFINE_EVENT_TYPE(fzEVT_FOLDERTHREAD_COMPLETE)

DECLARE_EVENT_TYPE(fzEVT_FOLDERTHREAD_FILES, -1)
DEFINE_EVENT_TYPE(fzEVT_FOLDERTHREAD_FILES)

DECLARE_EVENT_TYPE(fzEVT_UPDATE_STATUSLINES, -1)
DEFINE_EVENT_TYPE(fzEVT_UPDATE_STATUSLINES)

DECLARE_EVENT_TYPE(fzEVT_ASKFORPASSWORD, -1)
DEFINE_EVENT_TYPE(fzEVT_ASKFORPASSWORD)

BEGIN_EVENT_TABLE(CQueueView, CQueueViewBase)
EVT_FZ_NOTIFICATION(wxID_ANY, CQueueView::OnEngineEvent)
EVT_COMMAND(wxID_ANY, fzEVT_FOLDERTHREAD_COMPLETE, CQueueView::OnFolderThreadComplete)
EVT_COMMAND(wxID_ANY, fzEVT_FOLDERTHREAD_FILES, CQueueView::OnFolderThreadFiles)
EVT_SCROLLWIN(CQueueView::OnScrollEvent)
EVT_COMMAND(wxID_ANY, fzEVT_UPDATE_STATUSLINES, CQueueView::OnUpdateStatusLines)
EVT_MOUSEWHEEL(CQueueView::OnMouseWheel)

EVT_CONTEXT_MENU(CQueueView::OnContextMenu)
EVT_MENU(XRCID("ID_PROCESSQUEUE"), CQueueView::OnProcessQueue)
EVT_MENU(XRCID("ID_REMOVEALL"), CQueueView::OnStopAndClear)
EVT_MENU(XRCID("ID_REMOVE"), CQueueView::OnRemoveSelected)
EVT_MENU(XRCID("ID_DEFAULT_FILEEXISTSACTION"), CQueueView::OnSetDefaultFileExistsAction)

EVT_COMMAND(wxID_ANY, fzEVT_ASKFORPASSWORD, CQueueView::OnAskPassword)

EVT_LIST_ITEM_FOCUSED(wxID_ANY, CQueueView::OnFocusItemChanged)

END_EVENT_TABLE()

class CFolderProcessingThread : public wxThread
{
public:
	CFolderProcessingThread(CQueueView* pOwner, CFolderScanItem* pFolderItem)
		: wxThread(wxTHREAD_JOINABLE), m_condition(m_sync) {
		m_pOwner = pOwner;
		m_pFolderItem = pFolderItem;

		m_didSendEvent = false;
		m_threadWaiting = false;
	}
	~CFolderProcessingThread() { }

	void GetFiles(std::list<t_newEntry> &entryList)
	{
		wxASSERT(entryList.empty());
		wxMutexLocker locker(m_sync);
		entryList.swap(m_entryList);

		m_didSendEvent = false;

		if (m_threadWaiting)
		{
			m_threadWaiting = false;
			m_condition.Signal();
		}
	}

protected:

	void AddEntry(const t_newEntry& entry)
	{
		m_sync.Lock();
		m_entryList.push_back(entry);

		// Wait if there are more than 100 items to queue,
		// don't send notification if there are less than 10.
		// This reduces overhead
		bool send;

		if (m_didSendEvent)
		{
			send = false;
			if (m_entryList.size() >= 100)
			{
				m_threadWaiting = true;
				m_condition.Wait();
			}
		}
		else if (m_entryList.size() < 10)
			send = false;
		else
			send = true;

		m_sync.Unlock();

		if (send)
		{
			// We send the notification after leaving the critical section, else we
			// could get into a deadlock. wxWidgets event system does internal
			// locking.
			wxCommandEvent evt(fzEVT_FOLDERTHREAD_FILES, wxID_ANY);
			wxPostEvent(m_pOwner, evt);
		}
	}

	ExitCode Entry()
	{
		wxMutexGuiEnter();
		wxASSERT(m_pFolderItem->GetTopLevelItem() && m_pFolderItem->GetTopLevelItem()->GetType() == QueueItemType_Server);
		wxMutexGuiLeave();

		wxASSERT(!m_pFolderItem->Download());

		while (!TestDestroy())
		{
			if (m_pFolderItem->m_remove)
			{
				wxCommandEvent evt(fzEVT_FOLDERTHREAD_COMPLETE, wxID_ANY);
				wxPostEvent(m_pOwner, evt);
				return 0;
			}
			bool found;
			wxString file;
			if (!m_pFolderItem->m_pDir)
			{
				if (!m_pFolderItem->m_dirsToCheck.empty())
				{
					const CFolderScanItem::t_dirPair& pair = m_pFolderItem->m_dirsToCheck.front();

					wxLogNull nullLog;

					m_pFolderItem->m_pDir = new wxDir(pair.localPath);

					if (m_pFolderItem->m_pDir->IsOpened())
					{
						m_pFolderItem->m_currentLocalPath = pair.localPath;
						m_pFolderItem->m_currentRemotePath = pair.remotePath;

						found = m_pFolderItem->m_pDir->GetFirst(&file);

						if (!found)
						{
							// Empty directory

							t_newEntry entry;
							entry.remotePath = pair.remotePath;

							AddEntry(entry);
						}
					}
					else
						found = false;
					m_pFolderItem->m_dirsToCheck.pop_front();
				}
				else
				{
					wxCommandEvent evt(fzEVT_FOLDERTHREAD_COMPLETE, wxID_ANY);
					wxPostEvent(m_pOwner, evt);
					return 0;
				}
			}
			else
				found = m_pFolderItem->m_pDir->GetNext(&file);

			while (found && !TestDestroy() && !m_pFolderItem->m_remove)
			{
				const wxString& fullName = m_pFolderItem->m_currentLocalPath + wxFileName::GetPathSeparator() + file;

				if (wxDir::Exists(fullName))
				{
#ifdef S_ISLNK
					wxStructStat buf;
					const int result = wxLstat(fullName, &buf);
					if (!result && S_ISLNK(buf.st_mode))
					{
						found = m_pFolderItem->m_pDir->GetNext(&file);
						continue;
					}
#endif

					if (!m_filters.FilenameFiltered(file, true, -1, true))
					{
						CFolderScanItem::t_dirPair pair;
						pair.localPath = fullName;
						pair.remotePath = m_pFolderItem->m_currentRemotePath;
						pair.remotePath.AddSegment(file);
						m_pFolderItem->m_dirsToCheck.push_back(pair);
					}
				}
				else
				{
					wxStructStat buf;
					int result;
					result = wxStat(fullName, &buf);

					if (!m_filters.FilenameFiltered(file, false, result ? -1 : buf.st_size, true))
					{
						t_newEntry entry;
						entry.localFile = fullName;
						entry.remoteFile = file;
						entry.remotePath = m_pFolderItem->m_currentRemotePath;
						entry.size = result ? -1 : buf.st_size;

						AddEntry(entry);
					}
				}

				found = m_pFolderItem->m_pDir->GetNext(&file);
			}

			bool send;
			m_sync.Lock();
			if (!m_didSendEvent && m_entryList.size())
				send = true;
			else
				send = false;
			m_sync.Unlock();

			if (send)
			{
				// We send the notification after leaving the critical section, else we
				// could get into a deadlock. wxWidgets event system does internal
				// locking.
				wxCommandEvent evt(fzEVT_FOLDERTHREAD_FILES, wxID_ANY);
				wxPostEvent(m_pOwner, evt);
			}

			delete m_pFolderItem->m_pDir;
			m_pFolderItem->m_pDir = 0;
		}

		return 0;
	}

	// Access has to be guarded by m_sync
	std::list<t_newEntry> m_entryList;

	CQueueView* m_pOwner;
	CFolderScanItem* m_pFolderItem;

	CFilterDialog m_filters;

	wxMutex m_sync;
	wxCondition m_condition;
	bool m_threadWaiting;
	bool m_didSendEvent;
};

CQueueView::CQueueView(wxAuiNotebookEx* parent, wxWindowID id, CMainFrame* pMainFrame, CAsyncRequestQueue *pAsyncRequestQueue)
	: CQueueViewBase(parent, id),
	  m_pMainFrame(pMainFrame),
	  m_pAsyncRequestQueue(pAsyncRequestQueue),
	  m_pAuiNotebook(parent)
{
	if (m_pAsyncRequestQueue)
		m_pAsyncRequestQueue->SetQueue(this);

	m_activeCount = 0;
	m_activeCountDown = 0;
	m_activeCountUp = 0;
	m_activeMode = 0;
	m_quit = false;
	m_waitStatusLineUpdate = false;
	m_lastTopItem = -1;
	m_pFolderProcessingThread = 0;

	m_totalQueueSize = 0;
	m_filesWithUnknownSize = 0;
	m_queuedFiles = 0;

	CreateColumns(_("Status"));

	// Create and assign the image list for the queue
	wxImageList* pImageList = new wxImageList(16, 16);

	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_SERVER"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FILE"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDERCLOSED"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDER"),  wxART_OTHER, wxSize(16, 16)));

	AssignImageList(pImageList, wxIMAGE_LIST_SMALL);

	//Initialize the engine data
	t_EngineData *pData = new t_EngineData;
	pData->active = false;
	pData->pItem = 0;
	pData->pStatusLineCtrl = 0;
	pData->state = t_EngineData::none;
	pData->pEngine = 0; // TODO: Primary transfer engine data
	m_engineData.push_back(pData);

	int engineCount = COptions::Get()->GetOptionVal(OPTION_NUMTRANSFERS);
	for (int i = 0; i < engineCount; i++)
	{
		t_EngineData *pData = new t_EngineData;
		pData->active = false;
		pData->pItem = 0;
		pData->pStatusLineCtrl = 0;
		pData->state = t_EngineData::none;

		pData->pEngine = new CFileZillaEngine();
		pData->pEngine->Init(this, COptions::Get());

		m_engineData.push_back(pData);
	}

	SettingsChanged();
	LoadQueue();

	SetDropTarget(new CQueueViewDropTarget(this));
}

CQueueView::~CQueueView()
{
	if (m_pFolderProcessingThread)
	{
		m_pFolderProcessingThread->Delete();
		m_pFolderProcessingThread->Wait();
		delete m_pFolderProcessingThread;
	}

	for (unsigned int engineIndex = 1; engineIndex < m_engineData.size(); engineIndex++)
		delete m_engineData[engineIndex]->pEngine;
	for (unsigned int engineIndex = 1; engineIndex < m_engineData.size(); engineIndex++)
		delete m_engineData[engineIndex];
}

bool CQueueView::QueueFile(const bool queueOnly, const bool download, const wxString& localFile,
						   const wxString& remoteFile, const CServerPath& remotePath,
						   const CServer& server, const wxLongLong size)
{
	CServerItem* pServerItem = CreateServerItem(server);

	CFileItem* fileItem;
	if (localFile == _T("") || remotePath.IsEmpty())
	{
		if (download)
			fileItem = new CFolderItem(pServerItem, queueOnly, localFile);
		else
			fileItem = new CFolderItem(pServerItem, queueOnly, remotePath, remoteFile);
	}
	else
	{
		fileItem = new CFileItem(pServerItem, queueOnly, download, localFile, remoteFile, remotePath, size);
		fileItem->m_transferSettings.binary = ShouldUseBinaryMode(download ? remoteFile : wxFileName(localFile).GetFullName());

		if (size < 0)
		{
			m_filesWithUnknownSize++;
			if (m_filesWithUnknownSize == 1)
				DisplayQueueSize();
		}
		else if (size > 0)
		{
			m_totalQueueSize += size;
			DisplayQueueSize();
		}
	}

	InsertItem(pServerItem, fileItem);

	CommitChanges();

	m_queuedFiles++;
	DisplayNumberQueuedFiles();

	if (!m_activeMode && !queueOnly)
		m_activeMode = 1;

	m_waitStatusLineUpdate = true;
	AdvanceQueue();
	m_waitStatusLineUpdate = false;
	UpdateStatusLinePositions();

	Refresh(false);

	return true;
}

bool CQueueView::QueueFiles(const bool queueOnly, const wxString& localPath, const CRemoteDataObject& dataObject)
{
	CServerItem* pServerItem = CreateServerItem(dataObject.GetServer());

	const std::list<CRemoteDataObject::t_fileInfo>& files = dataObject.GetFiles();

	for (std::list<CRemoteDataObject::t_fileInfo>::const_iterator iter = files.begin(); iter != files.end(); iter++)
	{
		if (iter->dir)
			continue;

		CFileItem* fileItem;
		fileItem = new CFileItem(pServerItem, queueOnly, true, localPath + iter->name, iter->name, dataObject.GetServerPath(), iter->size);
		fileItem->m_transferSettings.binary = ShouldUseBinaryMode(iter->name);
		
		if (iter->size < 0)
			m_filesWithUnknownSize++;
		else if (iter->size > 0)
			m_totalQueueSize += iter->size;

		InsertItem(pServerItem, fileItem);

		m_queuedFiles++;
	}

	CommitChanges();

	if (!m_activeMode && !queueOnly)
		m_activeMode = 1;

	m_waitStatusLineUpdate = true;
	AdvanceQueue();
	m_waitStatusLineUpdate = false;
	UpdateStatusLinePositions();

	DisplayQueueSize();
	DisplayNumberQueuedFiles();

	Refresh(false);

	return true;
}

void CQueueView::OnEngineEvent(wxEvent &event)
{
	std::vector<t_EngineData*>::iterator iter;
	for (iter = m_engineData.begin(); iter != m_engineData.end(); iter++)
	{
		if ((wxObject *)(*iter)->pEngine == event.GetEventObject())
			break;
	}
	if (iter == m_engineData.end())
		return;

	t_EngineData* const pEngineData = *iter;

	CNotification *pNotification = pEngineData->pEngine->GetNextNotification();
	while (pNotification)
	{
		switch (pNotification->GetID())
		{
		case nId_logmsg:
			m_pMainFrame->GetStatusView()->AddToLog(reinterpret_cast<CLogmsgNotification *>(pNotification));
			delete pNotification;
			break;
		case nId_operation:
			ProcessReply(*pEngineData, reinterpret_cast<COperationNotification*>(pNotification));
			delete pNotification;
			break;
		case nId_asyncrequest:
			if (!pEngineData->pItem)
			{
				delete pNotification;
				break;
			}
			switch (reinterpret_cast<CAsyncRequestNotification *>(pNotification)->GetRequestID())
			{
			case reqId_fileexists:
				{
					// Set default file exists action
					CFileExistsNotification *pFileExistsNotification = reinterpret_cast<CFileExistsNotification *>(pNotification);
					pFileExistsNotification->overwriteAction = (enum CFileExistsNotification::OverwriteAction)pEngineData->pItem->m_defaultFileExistsAction;
				}
				break;
			default:
				break;
			}

			m_pAsyncRequestQueue->AddRequest(pEngineData->pEngine, reinterpret_cast<CAsyncRequestNotification *>(pNotification));
			break;
		case nId_active:
			{
				CActiveNotification *pActiveNotification = reinterpret_cast<CActiveNotification *>(pNotification);
				if (pActiveNotification->IsRecv())
					m_pMainFrame->UpdateRecvLed();
				else
					m_pMainFrame->UpdateSendLed();
				delete pNotification;
			}
			break;
		case nId_transferstatus:
			if (pEngineData->pItem && pEngineData->pStatusLineCtrl)
			{
				CTransferStatusNotification *pTransferStatusNotification = reinterpret_cast<CTransferStatusNotification *>(pNotification);
				const CTransferStatus *pStatus = pTransferStatusNotification->GetStatus();

				if (pEngineData->active)
					pEngineData->pStatusLineCtrl->SetTransferStatus(pStatus);
			}
			delete pNotification;
			break;
		default:
			delete pNotification;
			break;
		}

		if (m_engineData.empty() || !pEngineData->pEngine)
			break;

		pNotification = pEngineData->pEngine->GetNextNotification();
	}
}

bool CQueueView::TryStartNextTransfer()
{
	if (m_quit || !m_activeMode)
		return false;

	// Check transfer limit
	if (m_activeCount >= COptions::Get()->GetOptionVal(OPTION_NUMTRANSFERS))
		return false;

	// Check limits for concurrent up/downloads
	const int maxDownloads = COptions::Get()->GetOptionVal(OPTION_CONCURRENTDOWNLOADLIMIT);
	const int maxUploads = COptions::Get()->GetOptionVal(OPTION_CONCURRENTUPLOADLIMIT);
	enum TransferDirection wantedDirection;
	if (maxDownloads && m_activeCountDown >= maxDownloads)
	{
		if (maxUploads && m_activeCountUp >= maxUploads)
			return false;
		else
			wantedDirection = both;
	}
	else if (maxUploads && m_activeCountUp >= maxUploads)
		wantedDirection = download;
	else
		wantedDirection = both;

	// Find inactive file. Check all servers for
	// the file with the highest priority
	CFileItem* fileItem = 0;
	CServerItem* serverItem = 0;
	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		CServerItem* currentServerItem = *iter;
		int maxCount = currentServerItem->GetServer().MaximumMultipleConnections();
		if (maxCount && currentServerItem->m_activeCount >= maxCount)
			continue;

		CFileItem* newFileItem = currentServerItem->GetIdleChild(m_activeMode == 1, wantedDirection);

		while (newFileItem && newFileItem->Download() && newFileItem->GetType() == QueueItemType_Folder)
		{
			wxFileName fn(newFileItem->GetLocalFile(), _T(""));
			wxFileName::Mkdir(fn.GetPath(), 0777, wxPATH_MKDIR_FULL);
			if (RemoveItem(newFileItem))
			{
				// Server got deleted. Unfortunately we have to start over now
				if (m_serverList.empty())
					return false;
				iter = m_serverList.begin();
			}
			newFileItem = currentServerItem->GetIdleChild(m_activeMode == 1, wantedDirection);
		}

		if (!newFileItem)
			continue;

		if (!fileItem || newFileItem->GetPriority() > fileItem->GetPriority())
		{
			serverItem = currentServerItem;
			fileItem = newFileItem;
			if (fileItem->GetPriority() == priority_highest)
				break;
		}
	}
	if (!fileItem)
		return false;

	// Find idle engine
	t_EngineData* pEngineData = GetIdleEngine(&serverItem->GetServer());
	if (!pEngineData)
		return false;

	// Now we have both inactive engine and file.
	// Assign the file to the engine.

	fileItem->SetActive(true);

	pEngineData->pItem = fileItem;
	fileItem->m_pEngineData = pEngineData;
	pEngineData->active = true;
	serverItem->m_activeCount++;
	m_activeCount++;
	if (fileItem->Download())
		m_activeCountDown++;
	else
		m_activeCountUp++;

	const CServer oldServer = pEngineData->lastServer;
	pEngineData->lastServer = serverItem->GetServer();

	if (!pEngineData->pEngine->IsConnected())
	{
		if (pEngineData->lastServer.GetLogonType() == ASK)
		{
			if (CLoginManager::Get().GetPassword(pEngineData->lastServer, true))
				pEngineData->state = t_EngineData::connect;
			else
				pEngineData->state = t_EngineData::askpassword;
		}
		else
			pEngineData->state = t_EngineData::connect;
	}
	else if (oldServer != serverItem->GetServer())
		pEngineData->state = t_EngineData::disconnect;
	else if (pEngineData->pItem->GetType() == QueueItemType_File)
		pEngineData->state = t_EngineData::transfer;
	else
		pEngineData->state = t_EngineData::mkdir;

	if (fileItem->GetType() == QueueItemType_File)
	{
		// Create status line

		m_itemCount++;
		SetItemCount(m_itemCount);
		int lineIndex = GetItemIndex(fileItem);
		UpdateSelections_ItemAdded(lineIndex + 1);

		wxRect rect;
		GetItemRect(lineIndex + 1, rect);
		m_allowBackgroundErase = false;
		if (!pEngineData->pStatusLineCtrl)
			pEngineData->pStatusLineCtrl = new CStatusLineCtrl(this, pEngineData, rect);
		else
		{
			pEngineData->pStatusLineCtrl->SetTransferStatus(0);
			pEngineData->pStatusLineCtrl->SetSize(rect);
			pEngineData->pStatusLineCtrl->Show();
		}
		m_allowBackgroundErase = true;
		m_statusLineList.push_back(pEngineData->pStatusLineCtrl);
	}

	SendNextCommand(*pEngineData);

	return true;
}

void CQueueView::ProcessReply(t_EngineData& engineData, COperationNotification* pNotification)
{
	if (pNotification->nReplyCode & FZ_REPLY_DISCONNECTED &&
		pNotification->commandId == cmd_none)
	{
		// Queue is not interested in disconnect notifications
		return;
	}

	// Cancel pending requests
	m_pAsyncRequestQueue->ClearPending(engineData.pEngine);

	// Process reply from the engine
	int replyCode = pNotification->nReplyCode;

	if ((replyCode & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED)
	{
		ResetEngine(engineData, engineData.pItem ? engineData.pItem->m_remove : false);
		return;
	}

	// Cycle through queue states
	switch (engineData.state)
	{
	case t_EngineData::disconnect:
		engineData.state = t_EngineData::connect;
		if (engineData.active)
			engineData.pStatusLineCtrl->SetTransferStatus(0);
		break;
	case t_EngineData::connect:
		if (replyCode == FZ_REPLY_OK)
		{
			if (engineData.pItem->GetType() == QueueItemType_File)
				engineData.state = t_EngineData::transfer;
			else
				engineData.state = t_EngineData::mkdir;
			if (engineData.active && engineData.pStatusLineCtrl)
				engineData.pStatusLineCtrl->SetTransferStatus(0);
		}
		else
		{
			if (replyCode & FZ_REPLY_PASSWORDFAILED)
				CLoginManager::Get().CachedPasswordFailed(engineData.lastServer);

			if (!IncreaseErrorCount(engineData))
				return;
		}
		break;
	case t_EngineData::transfer:
		if (replyCode == FZ_REPLY_OK)
		{
			ResetEngine(engineData, true);
			return;
		}
		else if (!IncreaseErrorCount(engineData))
			return;
		if (replyCode & FZ_REPLY_DISCONNECTED)
			engineData.state = t_EngineData::connect;
		break;
	case t_EngineData::mkdir:
		if (replyCode == FZ_REPLY_OK)
		{
			ResetEngine(engineData, true);
			return;
		}
		if (replyCode & FZ_REPLY_DISCONNECTED)
		{
			if (!IncreaseErrorCount(engineData))
				return;
			engineData.state = t_EngineData::connect;
		}
		else
		{
			// Cannot retry
			ResetEngine(engineData, true);
			return;
		}

		break;
	case t_EngineData::list:
		ResetEngine(engineData, false);
		return;
	default:
		return;
	}

	if (engineData.state == t_EngineData::connect && engineData.lastServer.GetLogonType() == ASK)
	{
		if (!CLoginManager::Get().GetPassword(engineData.lastServer, true))
			engineData.state = t_EngineData::askpassword;
	}

	if (!m_activeMode)
	{
		ResetEngine(engineData, engineData.pItem ? engineData.pItem->m_remove : false);
		return;
	}

	SendNextCommand(engineData);
}

void CQueueView::ResetEngine(t_EngineData& data, const bool removeFileItem)
{
	if (!data.active)
		return;

	m_waitStatusLineUpdate = true;

	if (data.pItem)
	{
		if (data.pItem->GetType() == QueueItemType_File)
		{
			wxASSERT(data.pStatusLineCtrl);
			for (std::list<CStatusLineCtrl*>::iterator iter = m_statusLineList.begin(); iter != m_statusLineList.end(); iter++)
			{
				if (*iter == data.pStatusLineCtrl)
				{
					m_statusLineList.erase(iter);
					break;
				}
			}
			m_allowBackgroundErase = false;
			data.pStatusLineCtrl->Hide();
			m_allowBackgroundErase = true;

			UpdateSelections_ItemRemoved(GetItemIndex(data.pItem) + 1);

			m_itemCount--;
			SetItemCount(m_itemCount);
		}

		wxASSERT(data.pItem->IsActive());
		wxASSERT(data.pItem->m_pEngineData == &data);
		if (data.pItem->IsActive())
			data.pItem->SetActive(false);
		if (data.pItem->Download())
		{
			wxASSERT(m_activeCountDown > 0);
			if (m_activeCountDown > 0)
				m_activeCountDown--;
		}
		else
		{
			wxASSERT(m_activeCountUp > 0);
			if (m_activeCountUp > 0)
				m_activeCountUp--;
		}

		if (removeFileItem)
			RemoveItem(data.pItem);
		else
			ResetItem(data.pItem);
		data.pItem = 0;

		wxASSERT(m_activeCount > 0);
		if (m_activeCount > 0)
			m_activeCount--;
	}
	data.active = false;
	data.state = t_EngineData::none;
	CServerItem* item = GetServerItem(data.lastServer);
	if (item)
	{
		wxASSERT(item->m_activeCount > 0);
		if (item->m_activeCount > 0)
			item->m_activeCount--;
	}

	AdvanceQueue();

	m_waitStatusLineUpdate = false;
	UpdateStatusLinePositions();

	CheckQueueState();
}

bool CQueueView::RemoveItem(CQueueItem* item)
{
	// RemoveItem assumes that the item has already been removed from all engines

	if (item->GetType() == QueueItemType_File)
	{
		// Update size information
		const CFileItem* const pFileItem = (const CFileItem* const)item;
		const wxLongLong& size = pFileItem->GetSize();
		if (size < 0)
		{
			m_filesWithUnknownSize--;
			wxASSERT(m_filesWithUnknownSize >= 0);
			if (!m_filesWithUnknownSize)
				DisplayQueueSize();
		}
		else if (size > 0)
		{
			m_totalQueueSize -= size;
			DisplayQueueSize();
			wxASSERT(m_totalQueueSize >= 0);
		}
	}
	if (item->GetType() == QueueItemType_File || item->GetType() == QueueItemType_Folder)
	{
		wxASSERT(m_queuedFiles > 0);
		m_queuedFiles--;
		DisplayNumberQueuedFiles();
	}

	const int index = GetItemIndex(item);

	CQueueItem* topLevelItem = item->GetTopLevelItem();

	int count = topLevelItem->GetChildrenCount(true);
	topLevelItem->RemoveChild(item);

	bool didRemoveParent;

	int oldCount = m_itemCount;
	if (!topLevelItem->GetChild(0))
	{
		std::vector<CServerItem*>::iterator iter;
		for (iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		{
			if (*iter == topLevelItem)
				break;
		}
		if (iter != m_serverList.end())
			m_serverList.erase(iter);

		UpdateSelections_ItemRangeRemoved(GetItemIndex(topLevelItem), count + 1);

		delete topLevelItem;

		m_itemCount -= count + 1;
		SetItemCount(m_itemCount);

		didRemoveParent = true;
	}
	else
	{
		count -= topLevelItem->GetChildrenCount(true);

		UpdateSelections_ItemRangeRemoved(index, count);

		m_itemCount -= count;
		SetItemCount(m_itemCount);

		didRemoveParent = false;
	}

	if (oldCount > m_itemCount)
	{
		bool eraseBackground = GetTopItem() + GetCountPerPage() + 1 >= m_itemCount;
		Refresh(eraseBackground);
		if (eraseBackground)
			Update();
	}

	UpdateStatusLinePositions();

	return didRemoveParent;
}

void CQueueView::SendNextCommand(t_EngineData& engineData)
{
	while (true)
	{
		if (engineData.state == t_EngineData::disconnect)
		{
			engineData.pItem->m_statusMessage = _("Disconnecting from previous server");
			if (engineData.pEngine->Command(CDisconnectCommand()) == FZ_REPLY_WOULDBLOCK)
				return;

			engineData.state = t_EngineData::connect;
			if (engineData.active && engineData.pStatusLineCtrl)
				engineData.pStatusLineCtrl->SetTransferStatus(0);
		}

		if (engineData.state == t_EngineData::askpassword)
		{
			if (m_waitingForPassword.empty())
			{
				wxCommandEvent evt(fzEVT_ASKFORPASSWORD);
				wxPostEvent(this, evt);
			}
			m_waitingForPassword.push_back(engineData.pEngine);
			return;
		}

		if (engineData.state == t_EngineData::connect)
		{
			engineData.pItem->m_statusMessage = _("Connecting");

			int res = engineData.pEngine->Command(CConnectCommand(engineData.lastServer));
			
			wxASSERT((res & FZ_REPLY_BUSY) != FZ_REPLY_BUSY);
			if (res == FZ_REPLY_WOULDBLOCK)
				return;

			if (res == FZ_REPLY_ALREADYCONNECTED)
			{
				engineData.state = t_EngineData::disconnect;
				continue;
			}

			if (res == FZ_REPLY_OK)
			{
				if (engineData.pItem->GetType() == QueueItemType_File)
				{
					engineData.state = t_EngineData::transfer;
					if (engineData.active)
						engineData.pStatusLineCtrl->SetTransferStatus(0);
				}
				else
					engineData.state = t_EngineData::mkdir;
				break;
			}

			if (!IncreaseErrorCount(engineData))
				return;
			continue;
		}

		if (engineData.state == t_EngineData::transfer)
		{
			CFileItem* fileItem = engineData.pItem;

			fileItem->m_statusMessage = _("Transferring");

			int res = engineData.pEngine->Command(CFileTransferCommand(fileItem->GetLocalFile(), fileItem->GetRemotePath(),
												fileItem->GetRemoteFile(), fileItem->Download(), fileItem->m_transferSettings));
			wxASSERT((res & FZ_REPLY_BUSY) != FZ_REPLY_BUSY);
			if (res == FZ_REPLY_WOULDBLOCK)
				return;

			if (res == FZ_REPLY_NOTCONNECTED)
			{
				engineData.state = t_EngineData::connect;
				continue;
			}

			if (res == FZ_REPLY_OK)
			{
				ResetEngine(engineData, true);
				return;
			}

			if (!IncreaseErrorCount(engineData))
				return;
			continue;
		}

		if (engineData.state == t_EngineData::mkdir)
		{
			CFileItem* fileItem = engineData.pItem;

			fileItem->m_statusMessage = _("Creating directory");

			int res = engineData.pEngine->Command(CMkdirCommand(fileItem->GetRemotePath()));

			wxASSERT((res & FZ_REPLY_BUSY) != FZ_REPLY_BUSY);
			if (res == FZ_REPLY_WOULDBLOCK)
				return;

			if (res == FZ_REPLY_NOTCONNECTED)
			{
				engineData.state = t_EngineData::connect;
				continue;
			}

			if (res == FZ_REPLY_OK)
			{
				ResetEngine(engineData, true);
				return;
			}

			// Pointless to retry
			ResetEngine(engineData, true);
			return;
		}
	}
}

bool CQueueView::SetActive(bool active /*=true*/)
{
	if (!active)
	{
		m_activeMode = 0;
		for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
			(*iter)->QueueImmediateFiles();

		UpdateStatusLinePositions();

		// Send active engines the cancel command
		unsigned int engineIndex;
		for (engineIndex = 1; engineIndex < m_engineData.size(); engineIndex++)
		{
			t_EngineData* const pEngineData = m_engineData[engineIndex];
			if (pEngineData->active)
				pEngineData->pEngine->Command(CCancelCommand());
		}

		return m_activeCount == 0;
	}
	else
	{
		m_activeMode = 2;

		m_waitStatusLineUpdate = true;
		AdvanceQueue();
		m_waitStatusLineUpdate = false;
		UpdateStatusLinePositions();

		CheckQueueState();
	}

	return true;
}

bool CQueueView::Quit()
{
	m_quit = true;

	bool canQuit = true;
	if (!SetActive(false))
		canQuit = false;

	for (unsigned int i = 0; i < 2; i++)
	{
		if (!m_queuedFolders[i].empty())
		{
			canQuit = false;
			for (std::list<CFolderScanItem*>::iterator iter = m_queuedFolders[i].begin(); iter != m_queuedFolders[i].end(); iter++)
				(*iter)->m_remove = true;
		}
	}
	if (m_pFolderProcessingThread)
		canQuit = false;

	if (!canQuit)
		return false;

	for (unsigned int engineIndex = 1; engineIndex < m_engineData.size(); engineIndex++)
	{
		t_EngineData* const data = m_engineData[engineIndex];
		delete data->pEngine;
		data->pEngine = 0;
	}
	for (unsigned int engineIndex = 0; engineIndex < m_engineData.size(); engineIndex++)
		delete m_engineData[engineIndex];
	m_engineData.clear();

	SaveQueue();

	return true;
}

void CQueueView::CheckQueueState()
{
	if (!m_activeCount)
		m_activeMode = 0;

	if (m_quit)
		m_pMainFrame->Close();
}

bool CQueueView::IncreaseErrorCount(t_EngineData& engineData)
{
	engineData.pItem->m_errorCount++;
	if (engineData.pItem->m_errorCount <= COptions::Get()->GetOptionVal(OPTION_TRANSFERRETRYCOUNT))
		return true;

	ResetEngine(engineData, true);

	//TODO: Error description?

	return false;
}

void CQueueView::ResetItem(CFileItem* item)
{
	if (!item->Queued())
		reinterpret_cast<CServerItem*>(item->GetTopLevelItem())->QueueImmediateFiles();

	UpdateStatusLinePositions();
}

void CQueueView::UpdateStatusLinePositions()
{
	if (m_waitStatusLineUpdate)
		return;

	m_lastTopItem = GetTopItem();
	int bottomItem = m_lastTopItem + GetCountPerPage();

	for (std::list<CStatusLineCtrl*>::iterator iter = m_statusLineList.begin(); iter != m_statusLineList.end(); iter++)
	{
		CStatusLineCtrl *pCtrl = *iter;
		int index = GetItemIndex(pCtrl->GetItem()) + 1;
		if (index < m_lastTopItem || index > bottomItem)
		{
			pCtrl->Show(false);
			continue;
		}

		wxRect rect;
		if (!GetItemRect(index, rect))
		{
			pCtrl->Show(false);
			continue;
		}
		m_allowBackgroundErase = bottomItem + 1 >= m_itemCount;
		pCtrl->SetSize(rect);
		m_allowBackgroundErase = false;
		pCtrl->Show();
		m_allowBackgroundErase = true;
	}
}

void CQueueView::CalculateQueueSize()
{
	// Collect total queue size
	m_totalQueueSize = 0;
	m_queuedFiles = 0;

	m_filesWithUnknownSize = 0;
	for (std::vector<CServerItem*>::const_iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		m_totalQueueSize += (*iter)->GetTotalSize(m_filesWithUnknownSize, m_queuedFiles);

	DisplayQueueSize();
	DisplayNumberQueuedFiles();
}

void CQueueView::DisplayQueueSize()
{
	wxStatusBar *pStatusBar = m_pMainFrame->GetStatusBar();
	if (!pStatusBar)
		return;

	wxString queueSize;
	wxLongLong totalSize = m_totalQueueSize;
	if (totalSize == 0 && m_filesWithUnknownSize == 0)
		queueSize = _("Queue: empty");
	if (totalSize > (1000 * 1000))
	{
		totalSize /= 1000 * 1000;
		queueSize.Printf(_("Queue: %s%d MB"), (m_filesWithUnknownSize > 0) ? _T(">") : _T(""), totalSize.GetLo());
	}
	else if (totalSize > 1000)
	{
		totalSize /= 1000;
		queueSize.Printf(_("Queue: %s%d KB"), (m_filesWithUnknownSize > 0) ? _T(">") : _T(""), totalSize.GetLo());
	}
	else
		queueSize.Printf(_("Queue: %s%d bytes"), (m_filesWithUnknownSize > 0) ? _T(">") : _T(""), totalSize.GetLo());
	pStatusBar->SetStatusText(queueSize, 4);
}

void CQueueView::DisplayNumberQueuedFiles()
{
	const wxString& name = _("Queued files");
	wxString str;
	if (m_queuedFiles > 0)
	{
		if (m_queuedFolders[0].empty() && m_queuedFolders[1].empty())
			str.Printf(name + _T(" (%d)"), m_queuedFiles);
		else
			str.Printf(name + _T(" (%d+)"), m_queuedFiles);
	}
	else
		str = name;
	m_pAuiNotebook->SetPageText(0, str);
}

bool CQueueView::QueueFolder(bool queueOnly, bool download, const wxString& localPath, const CServerPath& remotePath, const CServer& server)
{
	CServerItem* pServerItem = CreateServerItem(server);

	CFolderScanItem* folderItem = new CFolderScanItem(pServerItem, queueOnly, download, localPath, remotePath);
	InsertItem(pServerItem, folderItem);

	folderItem->m_statusMessage = _("Waiting");

	CommitChanges();

	m_queuedFolders[download ? 0 : 1].push_back(folderItem);
	if ((m_queuedFolders[0].size() + m_queuedFolders[1].size()) == 1)
		DisplayNumberQueuedFiles();

	ProcessFolderItems();

	Refresh(false);

	return true;
}

bool CQueueView::ProcessFolderItems(int type /*=-1*/)
{
	if (type == -1)
	{
		while (ProcessFolderItems(0));
		ProcessUploadFolderItems();

		return true;
	}

	return false;
}

void CQueueView::ProcessUploadFolderItems()
{
	if (m_queuedFolders[1].empty())
	{
		if (m_quit)
			m_pMainFrame->Close();

		return;
	}

	if (m_pFolderProcessingThread)
		return;

	CFolderScanItem* pItem = m_queuedFolders[1].front();

	if (pItem->Queued())
		pItem->m_statusMessage = _("Scanning for files to add to queue");
	else
		pItem->m_statusMessage = _("Scanning for files to upload");
	pItem->m_active = true;
	m_pFolderProcessingThread = new CFolderProcessingThread(this, pItem);
	m_pFolderProcessingThread->Create();
	m_pFolderProcessingThread->Run();

	Refresh(false);
}

void CQueueView::OnFolderThreadComplete(wxCommandEvent& event)
{
	if (!m_pFolderProcessingThread)
		return;

	wxASSERT(!m_queuedFolders[1].empty());
	CFolderScanItem* pItem = m_queuedFolders[1].front();
	m_queuedFolders[1].pop_front();

	RemoveItem(pItem);

	if (m_queuedFolders[0].empty() && m_queuedFolders[1].empty())
		DisplayNumberQueuedFiles();

	m_pFolderProcessingThread->Wait();
	delete m_pFolderProcessingThread;
	m_pFolderProcessingThread = 0;

	ProcessUploadFolderItems();
}

bool CQueueView::QueueFiles(const std::list<t_newEntry> &entryList, bool queueOnly, bool download, CServerItem* pServerItem, const int defaultFileExistsAction)
{
	wxASSERT(pServerItem);

	for (std::list<t_newEntry>::const_iterator iter = entryList.begin(); iter != entryList.end(); iter++)
	{
		const t_newEntry& entry = *iter;

		CFileItem* fileItem;
		if (entry.localFile != _T(""))
		{
			fileItem = new CFileItem(pServerItem, queueOnly, download, entry.localFile, entry.remoteFile, entry.remotePath, entry.size);
			fileItem->m_transferSettings.binary = ShouldUseBinaryMode(download ? entry.remoteFile : wxFileName(entry.localFile).GetFullName());
			fileItem->m_defaultFileExistsAction = defaultFileExistsAction;

			if (entry.size < 0)
				m_filesWithUnknownSize++;
			else if (entry.size > 0)
				m_totalQueueSize += entry.size;
		}
		else
			fileItem = new CFolderItem(pServerItem, queueOnly, entry.remotePath, _T(""));

		m_queuedFiles++;

		InsertItem(pServerItem, fileItem);
	}

	CommitChanges();
	
	if (!m_activeMode && !queueOnly)
		m_activeMode = 1;

	m_waitStatusLineUpdate = true;
	AdvanceQueue();
	m_waitStatusLineUpdate = false;
	UpdateStatusLinePositions();

	DisplayQueueSize();
	DisplayNumberQueuedFiles();

	Refresh(false);

	return true;
}

void CQueueView::SaveQueue()
{
	// We have to synchronize access to queue.xml so that multiple processed don't write
	// to the same file or one is reading while the other one writes.
	CInterProcessMutex mutex(MUTEX_QUEUE);

	wxFileName file(wxGetApp().GetSettingsDir(), _T("queue.xml"));
	TiXmlElement* pDocument = GetXmlFile(file);
	if (!pDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nThe queue will not be saved."), file.GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

		return;
	}

	TiXmlElement* pQueue = pDocument->FirstChildElement("Queue");
	if (!pQueue)
	{
		pQueue = pDocument->InsertEndChild(TiXmlElement("Queue"))->ToElement();
	}

	wxASSERT(pQueue);

	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		(*iter)->SaveItem(pQueue);

	if (!pDocument->GetDocument()->SaveFile(file.GetFullPath().mb_str()))
	{
		wxString msg = wxString::Format(_("Could not write \"%s\", the queue could not be saved."), file.GetFullPath().c_str());
		wxMessageBox(msg, _("Error writing xml file"), wxICON_ERROR);
	}

	delete pDocument->GetDocument();
}

void CQueueView::LoadQueue()
{
	// We have to synchronize access to queue.xml so that multiple processed don't write
	// to the same file or one is reading while the other one writes.
	CInterProcessMutex mutex(MUTEX_QUEUE);

	wxFileName file(wxGetApp().GetSettingsDir(), _T("queue.xml"));
	TiXmlElement* pDocument = GetXmlFile(file);
	if (!pDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nThe queue will not be saved."), file.GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

		return;
	}

	TiXmlElement* pQueue = pDocument->FirstChildElement("Queue");
	if (!pQueue)
	{
		delete pDocument->GetDocument();
		return;
	}

	TiXmlElement* pServer = pQueue->FirstChildElement("Server");
	while (pServer)
	{
		CServer server;
		if (GetServer(pServer, server))
		{
			CServerItem *pServerItem = GetServerItem(server);
			bool newServer;
			if (!pServerItem)
			{
				newServer = true;
				pServerItem = new CServerItem(server);
			}
			else
				newServer = false;

			for (TiXmlElement* pFile = pServer->FirstChildElement("File"); pFile; pFile = pFile->NextSiblingElement("File"))
			{
				wxString localFile = GetTextElement(pFile, "LocalFile");
				wxString remoteFile = GetTextElement(pFile, "RemoteFile");
				wxString safeRemotePath = GetTextElement(pFile, "RemotePath");
				bool download = GetTextElementInt(pFile, "Download") != 0;
				wxLongLong size = GetTextElementLongLong(pFile, "Size", -1);
				unsigned int errorCount = GetTextElementInt(pFile, "ErrorCount");
				unsigned int priority = GetTextElementInt(pFile, "Priority", priority_normal);
				unsigned int itemState = GetTextElementInt(pFile, "ItemState", ItemState_Wait);
				bool binary = GetTextElementInt(pFile, "TransferMode", 1) != 0;

				CServerPath remotePath;
				if (localFile != _T("") && remoteFile != _T("") && remotePath.SetSafePath(safeRemotePath) &&
					size >= -1 && priority < PRIORITY_COUNT &&
					(itemState == ItemState_Wait || itemState == ItemState_Error))
				{
					CFileItem* fileItem = new CFileItem(pServerItem, true, download, localFile, remoteFile, remotePath, size);
					fileItem->m_transferSettings.binary = binary;
					fileItem->SetPriority((enum QueuePriority)priority);
					fileItem->SetItemState((enum ItemState)itemState);
					fileItem->m_errorCount = errorCount;
					pServerItem->AddChild(fileItem);
					m_itemCount++;

					if (size < 0)
						m_filesWithUnknownSize++;
					else if (size > 0)
						m_totalQueueSize += size;
					m_queuedFiles++;
				}
			}
			for (TiXmlElement* pFolder = pServer->FirstChildElement("Folder"); pFolder; pFolder = pFolder->NextSiblingElement("Folder"))
			{
				CFolderItem* folderItem;

				bool download = GetTextElementInt(pFolder, "Download") != 0;
				if (download)
				{
					wxString localFile = GetTextElement(pFolder, "LocalFile");
					if (localFile == _T(""))
						continue;
					folderItem = new CFolderItem(pServerItem, true, localFile);
				}
				else
				{
					wxString remoteFile = GetTextElement(pFolder, "RemoteFile");
					if (remoteFile == _T(""))
						continue;

					wxString safeRemotePath = GetTextElement(pFolder, "RemotePath");
					if (safeRemotePath == _T(""))
						continue;

					CServerPath remotePath;
					if (!remotePath.SetSafePath(safeRemotePath))
						continue;
					folderItem = new CFolderItem(pServerItem, true, remotePath, remoteFile);
				}

				unsigned int priority = GetTextElementInt(pFolder, "Priority", priority_normal);
				if (priority >= PRIORITY_COUNT)
				{
					delete folderItem;
					continue;
				}
				folderItem->SetPriority((enum QueuePriority)priority);

				pServerItem->AddChild(folderItem);
				m_itemCount++;
			}

			if (newServer)
			{
				if (pServerItem->GetChild(0))
				{
					m_serverList.push_back(pServerItem);
					m_itemCount++;
				}
				else
					delete pServerItem;
			}
		}

		pServer = pServer->NextSiblingElement("Server");
	}
	pDocument->RemoveChild(pQueue);

	if (!pDocument->GetDocument()->SaveFile(file.GetFullPath().mb_str()))
	{
		wxString msg = wxString::Format(_("Could not write \"%s\", the queue could not be saved."), file.GetFullPath().c_str());
		wxMessageBox(msg, _("Error writing xml file"), wxICON_ERROR);
	}

	delete pDocument->GetDocument();

	SetItemCount(m_itemCount);
	DisplayQueueSize();
	DisplayNumberQueuedFiles();
}

void CQueueView::SettingsChanged()
{
	m_asciiFiles.clear();
	wxString extensions = COptions::Get()->GetOption(OPTION_ASCIIFILES);
	wxString ext;
	int pos = extensions.Find(_T("|"));
	while (pos != -1)
	{
		if (!pos)
		{
			if (ext != _T(""))
			{
				ext.Replace(_T("\\\\"), _T("\\"));
				m_asciiFiles.push_back(ext);
				ext = _T("");
			}
		}
		else if (extensions.c_str()[pos - 1] != '\\')
		{
			ext += extensions.Left(pos);
			ext.Replace(_T("\\\\"), _T("\\"));
			m_asciiFiles.push_back(ext);
			ext = _T("");
		}
		else
		{
			ext += extensions.Left(pos - 1) + _T("|");
		}
		extensions = extensions.Mid(pos + 1);
		pos = extensions.Find(_T("|"));
	}
	ext += extensions;
	ext.Replace(_T("\\\\"), _T("\\"));
	m_asciiFiles.push_back(ext);
}

bool CQueueView::ShouldUseBinaryMode(wxString filename)
{
	int mode = COptions::Get()->GetOptionVal(OPTION_ASCIIBINARY);
	if (mode == 1)
		return false;
	else if (mode == 2)
		return true;

	int pos = filename.Find('.');
	if (pos == -1)
		return COptions::Get()->GetOptionVal(OPTION_ASCIINOEXT) != 0;
	else if (!pos)
		return COptions::Get()->GetOptionVal(OPTION_ASCIIDOTFILE) != 0;

	wxString ext = filename;
	do
	{
		ext = ext.Mid(pos + 1);
	}
	while ((pos = ext.Find('.')) != -1);

	if (ext == _T(""))
		return true;

	for (std::list<wxString>::const_iterator iter = m_asciiFiles.begin(); iter != m_asciiFiles.end(); iter++)
		if (!ext.CmpNoCase(*iter))
			return false;

	return true;
}

void CQueueView::OnScrollEvent(wxScrollWinEvent& event)
{
	event.Skip();
	wxCommandEvent evt(fzEVT_UPDATE_STATUSLINES, wxID_ANY);
	AddPendingEvent(evt);
}

void CQueueView::OnUpdateStatusLines(wxCommandEvent& event)
{
	if (GetTopItem() != m_lastTopItem)
		UpdateStatusLinePositions();
}

void CQueueView::OnMouseWheel(wxMouseEvent& event)
{
	event.Skip();
	wxCommandEvent evt(fzEVT_UPDATE_STATUSLINES, wxID_ANY);
	AddPendingEvent(evt);
}

void CQueueView::OnContextMenu(wxContextMenuEvent& event)
{
	wxMenu* pMenu = wxXmlResource::Get()->LoadMenu(_T("ID_MENU_QUEUE"));
	if (!pMenu)
		return;

	pMenu->Check(XRCID("ID_PROCESSQUEUE"), IsActive() ? true : false);
	pMenu->Enable(XRCID("ID_REMOVE"), GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) != -1);

	const bool hasSelection = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) != -1;
	pMenu->Enable(XRCID("ID_PRIORITY"), hasSelection);
	pMenu->Enable(XRCID("ID_DEFAULT_FILEEXISTSACTION"), hasSelection);

	PopupMenu(pMenu);
	delete pMenu;
}

void CQueueView::OnProcessQueue(wxCommandEvent& event)
{
	SetActive(event.IsChecked());
}

void CQueueView::OnStopAndClear(wxCommandEvent& event)
{
	SetActive(false);
	RemoveAll();
}

void CQueueView::RemoveAll()
{
	// This function removes all inactive items and queues active items
	// for removal

	// First, clear all selections
	int item;
	while ((item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
		SetItemState(item, 0, wxLIST_STATE_SELECTED);

	std::vector<CServerItem*> newServerList;
	m_itemCount = 0;
	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		if ((*iter)->TryRemoveAll())
			delete *iter;
		else
		{
			newServerList.push_back(*iter);
			m_itemCount += 1 + (*iter)->GetChildrenCount(true);
		}
	}
	SetItemCount(m_itemCount);

	m_serverList = newServerList;
	UpdateStatusLinePositions();

	CalculateQueueSize();

	CheckQueueState();
	Refresh();
}

void CQueueView::OnRemoveSelected(wxCommandEvent& event)
{
	std::list<long> selectedItems;
	long item = -1;
	while (true)
	{
		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1)
			break;

		selectedItems.push_front(item);
		SetItemState(item, 0, wxLIST_STATE_SELECTED);
	}

	m_waitStatusLineUpdate = true;

	long skip = -1;
	for (std::list<long>::const_iterator iter = selectedItems.begin(); iter != selectedItems.end(); iter++)
	{
		if (*iter == skip)
			continue;

		CQueueItem* pItem = GetQueueItem(*iter);
		if (!pItem)
			continue;

		if (pItem->GetType() == QueueItemType_Status)
			continue;
		else if (pItem->GetType() == QueueItemType_FolderScan)
		{
			CFolderScanItem* pFolder = (CFolderScanItem*)pItem;
			if (pFolder->m_active)
			{
				pFolder->m_remove = true;
				continue;
			}
		}
		else if (pItem->GetType() == QueueItemType_Server)
		{
			CServerItem* pServer = (CServerItem*)pItem;
			StopItem(pServer);

			// Server items get deleted automatically if all children are gone
			continue;
		}
		else if (pItem->GetType() == QueueItemType_File ||
				 pItem->GetType() == QueueItemType_Folder)
		{
			CFileItem* pFile = (CFileItem*)pItem;
			if (pFile->IsActive())
			{
				StopItem(pFile);
				pFile->m_remove = true;
				continue;
			}
		}

		CQueueItem* pTopLevelItem = pItem->GetTopLevelItem();
		if (!pTopLevelItem->GetChild(1))
			// Parent will get deleted, skip it so it doesn't get deleted twice.
			skip = GetItemIndex(pTopLevelItem);
		RemoveItem(pItem);
	}

	m_waitStatusLineUpdate = false;
	UpdateStatusLinePositions();
}

bool CQueueView::StopItem(CFileItem* item)
{
	if (!item->IsActive())
		return true;

	((CServerItem*)item->GetTopLevelItem())->QueueImmediateFile(item);
	item->m_pEngineData->pEngine->Command(CCancelCommand());
	return false;
}

bool CQueueView::StopItem(CServerItem* pServerItem)
{
	for (unsigned int i = 0; i < pServerItem->GetChildrenCount(false); i++)
	{
		CQueueItem* pItem = pServerItem->GetChild(i, false);
		if (pItem->GetType() == QueueItemType_FolderScan)
		{
			CFolderScanItem* pFolder = (CFolderScanItem*)pItem;
			if (pFolder->m_active)
			{
				pFolder->m_remove = true;
				continue;
			}
		}
		else if (pItem->GetType() == QueueItemType_File ||
				 pItem->GetType() == QueueItemType_Folder)
		{
			CFileItem* pFile = (CFileItem*)pItem;
			if (pFile->IsActive())
			{
				StopItem(pFile);
				pFile->m_remove = true;
				continue;
			}
		}
		else
			// Unknown type, shouldn't be here.
			wxASSERT(false);

		if (pServerItem->GetChildrenCount(false) == 1)
		{
			RemoveItem(pItem);
			return true;
		}

		RemoveItem(pItem);
		i--;
	}

	return pServerItem->GetChildrenCount(false) == 0;
}

void CQueueView::OnFolderThreadFiles(wxCommandEvent& event)
{
	if (!m_pFolderProcessingThread)
		return;

	wxASSERT(!m_queuedFolders[1].empty());
	CFolderScanItem* pItem = m_queuedFolders[1].front();

	std::list<t_newEntry> entryList;
	m_pFolderProcessingThread->GetFiles(entryList);
	QueueFiles(entryList, pItem->Queued(), false, (CServerItem*)pItem->GetTopLevelItem(), pItem->m_defaultFileExistsAction);

	pItem->m_count += entryList.size();
	pItem->m_statusMessage = wxString::Format(_("%d files added to queue"), pItem->GetCount());
	RefreshItem(GetItemIndex(pItem));
}

void CQueueView::SetDefaultFileExistsAction(int action, const enum TransferDirection direction)
{
	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		(*iter)->SetDefaultFileExistsAction(action, direction);
}

void CQueueView::OnSetDefaultFileExistsAction(wxCommandEvent &event)
{
	if (GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) == -1)
		return;

	CDefaultFileExistsDlg dlg;
	if (!dlg.Load(this, true))
		return;

	int downloadAction, uploadAction;
	if (!dlg.Run(&downloadAction, &uploadAction))
		return;

	std::list<long> selectedItems;
	long item = -1;
	while (true)
	{
		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1)
			break;

		CQueueItem* pItem = GetQueueItem(item);
		if (!pItem)
			continue;

		switch (pItem->GetType())
		{
		case QueueItemType_FolderScan:
			((CFolderScanItem*)pItem)->m_defaultFileExistsAction = uploadAction;
			break;
		case QueueItemType_File:
			{
				CFileItem *pFileItem = (CFileItem*)pItem;
				if (pFileItem->Download())
					pFileItem->m_defaultFileExistsAction = downloadAction;
				else
					pFileItem->m_defaultFileExistsAction = uploadAction;
			}
			break;
		case QueueItemType_Server:
			{
				CServerItem *pServerItem = (CServerItem*)pItem;
				pServerItem->SetDefaultFileExistsAction(downloadAction, download);
				pServerItem->SetDefaultFileExistsAction(uploadAction, upload);
			}
			break;
		default:
			break;
		}
	}
}

t_EngineData* CQueueView::GetIdleEngine(const CServer* pServer /*=0*/)
{
	t_EngineData* pFirstIdle = 0;

	// We start with 1, since 0 is the primary connection
	for (unsigned int i = 1; i < m_engineData.size(); i++)
	{
		if (m_engineData[i]->active)
			continue;

		if (!pServer)
			return m_engineData[i];

		if (m_engineData[i]->pEngine->IsConnected() && m_engineData[i]->lastServer == *pServer)
			return m_engineData[i];

		if (!pFirstIdle)
			pFirstIdle = m_engineData[i];
	}

	return pFirstIdle;
}

void CQueueView::TryRefreshListings()
{
	// TODO: This function is currently unused

	if (m_quit)
		return;

	const CState* const pState = m_pMainFrame->GetState();
	const CServer* const pServer = pState->GetServer();
	if (!pServer)
		return;

	const CDirectoryListing* const pListing = pState->GetRemoteDir();
	if (!pListing || !pListing->m_hasUnsureEntries)
		return;

	// See if there's an engine with is already listing
	for (unsigned int i = 1; i < m_engineData.size(); i++)
	{
		if (!m_engineData[i]->active)
			continue;

		if (m_engineData[i]->pItem)
			continue;

		if (m_engineData[i]->pEngine->IsConnected() && m_engineData[i]->lastServer == *pServer)
		{
			// This engine is already listing a directory on the current server
			return;
		}
	}

	t_EngineData* pEngineData = GetIdleEngine(pServer);
	if (!pEngineData)
		return;

	if (!pEngineData->pEngine->IsConnected() || pEngineData->lastServer != *pServer)
		return;

	CListCommand command(pListing->path);
	int res = pEngineData->pEngine->Command(command);
	if (res != FZ_REPLY_WOULDBLOCK)
		return;

	pEngineData->active = true;
	pEngineData->state = t_EngineData::list;
}

void CQueueView::OnAskPassword(wxCommandEvent& event)
{
	while (!m_waitingForPassword.empty())
	{
		const CFileZillaEngine* const pEngine = m_waitingForPassword.front();

		std::vector<t_EngineData*>::iterator iter;
		for (iter = m_engineData.begin(); iter != m_engineData.end(); iter++)
		{
			if ((*iter)->pEngine == pEngine)
				break;
		}
		if (iter == m_engineData.end())
		{
			m_waitingForPassword.pop_front();
			continue;
		}
		
		t_EngineData* pEngineData = *iter;

		if (pEngineData->state != t_EngineData::askpassword)
		{
			m_waitingForPassword.pop_front();
			continue;
		}
		
		static std::list<void*> AskPasswordBusyList;
		if (CLoginManager::Get().GetPassword(pEngineData->lastServer, false))
		{
			pEngineData->state = t_EngineData::connect;
			SendNextCommand(*pEngineData);
		}
		else
			ResetEngine(*pEngineData, true);

		m_waitingForPassword.pop_front();
	}
}

void CQueueView::OnFocusItemChanged(wxListEvent& event)
{
	event.Skip();
	wxCommandEvent evt(fzEVT_UPDATE_STATUSLINES, wxID_ANY);
	AddPendingEvent(evt);
}

void CQueueView::UpdateItemSize(CFileItem* pItem, wxLongLong size)
{
	wxASSERT(pItem);

	const wxLongLong oldSize = pItem->GetSize();
	if (size == oldSize)
		return;

	if (oldSize == -1)
	{
		wxASSERT(m_filesWithUnknownSize);
		if (m_filesWithUnknownSize)
			m_filesWithUnknownSize--;
	}
	else
	{
		wxASSERT(m_totalQueueSize > oldSize);
		if (m_totalQueueSize > oldSize)
			m_totalQueueSize -= oldSize;
		else
			m_totalQueueSize = 0;
	}
	
	if (size == -1)
		m_filesWithUnknownSize++;
	else
		m_totalQueueSize += size;

	pItem->SetSize(size);

	DisplayQueueSize();
}

void CQueueView::AdvanceQueue()
{
	static bool insideAdvanceQueue = false;
	if (insideAdvanceQueue)
		return;

	insideAdvanceQueue = true;
	while (TryStartNextTransfer())
	{
	}
	insideAdvanceQueue = false;
}
