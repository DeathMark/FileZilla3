#include "FileZilla.h"
#include "queue.h"
#include "queueview_failed.h"
#include "edithandler.h"

BEGIN_EVENT_TABLE(CQueueViewFailed, CQueueViewBase)
EVT_CONTEXT_MENU(CQueueViewFailed::OnContextMenu)
EVT_MENU(XRCID("ID_REMOVEALL"), CQueueViewFailed::OnRemoveAll)
EVT_MENU(XRCID("ID_REMOVE"), CQueueViewFailed::OnRemoveSelected)
EVT_MENU(XRCID("ID_REQUEUE"), CQueueViewFailed::OnRequeueSelected)
EVT_CHAR(CQueueViewFailed::OnChar)
END_EVENT_TABLE()

CQueueViewFailed::CQueueViewFailed(CQueue* parent, int index)
	: CQueueViewBase(parent, index, _("Failed transfers"))
{
	CreateColumns(_("Reason"));
}

CQueueViewFailed::CQueueViewFailed(CQueue* parent, int index, const wxString& title)
	: CQueueViewBase(parent, index, title)
{
}

void CQueueViewFailed::OnContextMenu(wxContextMenuEvent& event)
{
	wxMenu* pMenu = wxXmlResource::Get()->LoadMenu(_T("ID_MENU_QUEUE_FAILED"));
	if (!pMenu)
		return;

#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	const bool has_selection = GetSelectedItemCount() != 0;
#else
	const bool has_selection = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) != -1;
#endif

	pMenu->Enable(XRCID("ID_REMOVE"), has_selection);
	pMenu->Enable(XRCID("ID_REQUEUE"), has_selection);

	PopupMenu(pMenu);
	delete pMenu;
}

void CQueueViewFailed::OnRemoveAll(wxCommandEvent& event)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	if (!GetSelectedItemCount())
		return;
#endif

	// First, clear all selections
	int item;
	while ((item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
		SetItemState(item, 0, wxLIST_STATE_SELECTED);

	CEditHandler* pEditHandler = CEditHandler::Get();
	if (pEditHandler)
		pEditHandler->RemoveAll(CEditHandler::upload_and_remove_failed);

	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		delete *iter;
	m_serverList.clear();

	m_itemCount = 0;
	SaveSetItemCount(0);
	m_fileCount = 0;
	m_folderScanCount = 0;

	DisplayNumberQueuedFiles();

	RefreshListOnly();

	if (!m_itemCount && m_pQueue->GetQueueView()->GetItemCount())
		m_pQueue->SetSelection(0);
}

void CQueueViewFailed::OnRemoveSelected(wxCommandEvent& event)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	if (!GetSelectedItemCount())
		return;
#endif

	std::list<CQueueItem*> selectedItems;
	long item = -1;
	while (true)
	{
		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1)
			break;

		selectedItems.push_front(GetQueueItem(item));
		SetItemState(item, 0, wxLIST_STATE_SELECTED);
	}

	CEditHandler* pEditHandler = CEditHandler::Get();

	while (!selectedItems.empty())
	{
		CQueueItem* pItem = selectedItems.front();
		selectedItems.pop_front();

		CQueueItem* pTopLevelItem = pItem->GetTopLevelItem();

		if (pItem->GetType() == QueueItemType_Server)
		{
			CServerItem* pServerItem = (CServerItem*)pItem;
			if (pEditHandler && pEditHandler->GetFileCount(CEditHandler::remote, CEditHandler::upload_and_remove_failed, &pServerItem->GetServer()))
				pEditHandler->RemoveAll(CEditHandler::upload_and_remove_failed, &pServerItem->GetServer());
		}
		else if (pItem->GetType() == QueueItemType_File)
		{
			CFileItem* pFileItem = (CFileItem*)pItem;
			if (pFileItem->m_edit == CEditHandler::remote && pEditHandler)
			{
				wxFileName fn(pFileItem->GetLocalFile());
				enum CEditHandler::fileState state = pEditHandler->GetFileState(CEditHandler::remote, fn.GetFullName());
				if (state == CEditHandler::upload_and_remove_failed)
					pEditHandler->Remove(pFileItem->m_edit, fn.GetFullName());
			}
		}

		if (!pTopLevelItem->GetChild(1))
		{
			// Parent will get deleted
			// If next selected item is parent, remove it from list
			if (!selectedItems.empty() && selectedItems.front() == pTopLevelItem)
				selectedItems.pop_front();
		}
		RemoveItem(pItem, true, false, false);
	}
	DisplayNumberQueuedFiles();
	SaveSetItemCount(m_itemCount);
	RefreshListOnly();

	if (!m_itemCount && m_pQueue->GetQueueView()->GetItemCount())
		m_pQueue->SetSelection(0);
}

void CQueueViewFailed::OnRequeueSelected(wxCommandEvent& event)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	if (!GetSelectedItemCount())
		return;
#endif

	bool failedToRequeueAll = false;
	std::list<CQueueItem*> selectedItems;
	long item = -1;
	long skipTo = -1;
	while (true)
	{
		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
		if (item == -1)
			break;
		SetItemState(item, 0, wxLIST_STATE_SELECTED);
		if (item < skipTo)
			continue;

		CQueueItem* pItem = GetQueueItem(item);
		if (pItem->GetType() == QueueItemType_Server)
			skipTo = item + pItem->GetChildrenCount(true) + 1;
		selectedItems.push_back(GetQueueItem(item));
	}

	if (selectedItems.empty())
		return;

	CQueueViewBase* pQueueView = m_pQueue->GetQueueView();

	while (!selectedItems.empty())
	{
		CQueueItem* pItem = selectedItems.front();
		selectedItems.pop_front();

		if (pItem->GetType() == QueueItemType_Server)
		{
			CServerItem* pOldServerItem = (CServerItem*)pItem;
			CServerItem* pServerItem = pQueueView->CreateServerItem(pOldServerItem->GetServer());

			unsigned int childrenCount = pOldServerItem->GetChildrenCount(false);
			for (unsigned int i = 0; i < childrenCount; i++)
			{
				CFileItem* pFileItem = (CFileItem*)pItem->GetChild(i, false);
				pFileItem->m_errorCount = 0;
				pFileItem->m_statusMessage.Clear();

				if (!pFileItem->Download() && !wxFileName::FileExists(pFileItem->GetLocalFile()))
				{
					failedToRequeueAll = true;
					RemoveItem(pItem, true, false, false);
					continue;
				}

				if (pFileItem->m_edit == CEditHandler::remote)
				{
					CEditHandler* pEditHandler = CEditHandler::Get();
					if (!pEditHandler)
					{
						failedToRequeueAll = true;
						delete pFileItem;
						continue;
					}
					wxFileName fn(pFileItem->GetLocalFile());
					enum CEditHandler::fileState state = pEditHandler->GetFileState(CEditHandler::remote, fn.GetFullName());
					if (state == CEditHandler::unknown)
					{
						wxASSERT(pFileItem->Download());
						if (!pEditHandler->AddFile(CEditHandler::remote, fn.GetFullName(), pFileItem->GetRemotePath(), pServerItem->GetServer()))
						{
							failedToRequeueAll = true;
							delete pFileItem;
							continue;
						}
					}
					else if (state == CEditHandler::upload_and_remove_failed)
					{
						wxASSERT(!pFileItem->Download());
						if (!pEditHandler->UploadFile(pFileItem->m_edit, fn.GetFullName(), true))
							failedToRequeueAll = true;
						delete pFileItem;
						continue;
					}
					else
					{
						failedToRequeueAll = true;
						delete pFileItem;
						continue;
					}
				}

				pFileItem->SetParent(pServerItem);
				pQueueView->InsertItem(pServerItem, pFileItem);
			}

			m_fileCount -= childrenCount;
			m_itemCount -= childrenCount + 1;
			pOldServerItem->DetachChildren();
			delete pOldServerItem;

			std::vector<CServerItem*>::iterator iter;
			for (iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
			{
				if (*iter == pOldServerItem)
					break;
			}
			if (iter != m_serverList.end())
				m_serverList.erase(iter);

			if (!pServerItem->GetChildrenCount(false))
			{
				pQueueView->CommitChanges();
				pQueueView->RemoveItem(pServerItem, true, true, true);
			}
		}
		else
		{
			CFileItem* pFileItem = (CFileItem*)pItem;
			pFileItem->m_errorCount = 0;
			pFileItem->m_statusMessage.Clear();

			if (!pFileItem->Download() && !wxFileName::FileExists(pFileItem->GetLocalFile()))
			{
				failedToRequeueAll = true;
				RemoveItem(pItem, true, false, false);
				continue;
			}

			CServerItem* pOldServerItem = (CServerItem*)pItem->GetTopLevelItem();
			CServerItem* pServerItem = pQueueView->CreateServerItem(pOldServerItem->GetServer());
			RemoveItem(pItem, false, false, false);

			if (pFileItem->m_edit == CEditHandler::remote)
			{
				CEditHandler* pEditHandler = CEditHandler::Get();
				if (!pEditHandler)
				{
					if (!pServerItem->GetChildrenCount(false))
					{
						pQueueView->CommitChanges();
						pQueueView->RemoveItem(pServerItem, true, true, true);
					}

					failedToRequeueAll = true;
					delete pItem;
					continue;
				}
				wxFileName fn(pFileItem->GetLocalFile());
				enum CEditHandler::fileState state = pEditHandler->GetFileState(CEditHandler::remote, fn.GetFullName());
				if (state == CEditHandler::unknown)
				{
					wxASSERT(pFileItem->Download());
					if (!pEditHandler->AddFile(CEditHandler::remote, fn.GetFullName(), pFileItem->GetRemotePath(), pServerItem->GetServer()))
					{
						if (!pServerItem->GetChildrenCount(false))
						{
							pQueueView->CommitChanges();
							pQueueView->RemoveItem(pServerItem, true, true, true);
						}

						failedToRequeueAll = true;
						delete pItem;
						continue;
					}
				}
				else if (state == CEditHandler::upload_and_remove_failed)
				{
					wxASSERT(!pFileItem->Download());
					if (!pEditHandler->UploadFile(pFileItem->m_edit, fn.GetFullName(), true))
						failedToRequeueAll = true;

					if (!pServerItem->GetChildrenCount(false))
					{
						pQueueView->CommitChanges();
						pQueueView->RemoveItem(pServerItem, true, true, true);
					}

					delete pItem;
					continue;
				}
				else
				{
					if (!pServerItem->GetChildrenCount(false))
					{
						pQueueView->CommitChanges();
						pQueueView->RemoveItem(pServerItem, true, true, true);
					}

					failedToRequeueAll = true;
					delete pItem;
					continue;
				}
			}

			pItem->SetParent(pServerItem);
			pQueueView->InsertItem(pServerItem, pItem);
		}
	}
	m_fileCountChanged = true;

	pQueueView->CommitChanges();

	DisplayNumberQueuedFiles();
	SaveSetItemCount(m_itemCount);
	RefreshListOnly();

	if (!m_itemCount && m_pQueue->GetQueueView()->GetItemCount())
		m_pQueue->SetSelection(0);

	if (failedToRequeueAll)
		wxMessageBox(_("Not all items could be requeued for viewing / editing."));
}

void CQueueViewFailed::OnChar(wxKeyEvent& event)
{
	if (event.GetKeyCode() == WXK_DELETE)
	{
		wxCommandEvent cmdEvt;
		OnRemoveSelected(cmdEvt);
	}
	else
		event.Skip();
}
