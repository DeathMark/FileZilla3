#include "filezilla.h"
#include "Options.h"
#include "queue.h"
#include "queueview_failed.h"
#include "queueview_successful.h"
#include "sizeformatting.h"

CQueueItem::CQueueItem()
{
	m_visibleOffspring = 0;
	m_parent = 0;
	m_maxCachedIndex = -1;

	m_removed_at_front = 0;
}

CQueueItem::~CQueueItem()
{
	std::vector<CQueueItem*>::iterator iter;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
		delete *iter;
}

void CQueueItem::SetPriority(enum QueuePriority priority)
{
	std::vector<CQueueItem*>::iterator iter;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
		(*iter)->SetPriority(priority);
}

void CQueueItem::AddChild(CQueueItem* item)
{
	if (m_removed_at_front)
	{
		m_children.erase(m_children.begin(), m_children.begin() + m_removed_at_front);
		m_removed_at_front = 0;
	}
	item->m_indent = m_indent + _T("  ");
	m_children.push_back(item);

	m_visibleOffspring += 1 + item->m_visibleOffspring;
	m_maxCachedIndex = -1;
	CQueueItem* parent = GetParent();
	while (parent)
	{
		parent->m_visibleOffspring += 1 + item->m_visibleOffspring;
		parent->m_maxCachedIndex = -1;
		parent = parent->GetParent();
	}
}

CQueueItem* CQueueItem::GetChild(unsigned int item, bool recursive /*=true*/)
{
	std::vector<CQueueItem*>::iterator iter;
	if (!recursive)
	{
		if (item + m_removed_at_front >= m_children.size())
			return 0;
		iter = m_children.begin();
		iter += item + m_removed_at_front;
		return *iter;
	}

	if ((int)item <= m_maxCachedIndex)
	{
		// Index is cached
		iter = m_children.begin() + m_removed_at_front;
		iter += m_lookupCache[item].child;
		item -= m_lookupCache[item].index;
		if (!item)
			return *iter;
		else
			return (*iter)->GetChild(item - 1);
	}

	int index;
	int child;
	iter = m_children.begin() + m_removed_at_front;
	if (m_maxCachedIndex == -1)
	{
		child = 0;
		index = 0;
	}
	else
	{
		// Start with loop with the last cached item index
		iter += m_lookupCache[m_maxCachedIndex].child + 1;
		item -= m_maxCachedIndex + 1;
		index = m_maxCachedIndex + 1;
		child = m_lookupCache[m_maxCachedIndex].child + 1;
	}

	for (; iter != m_children.end(); iter++, child++)
	{
		if (!item)
			return *iter;

		unsigned int count = (*iter)->GetChildrenCount(true);
		if (item > count)
		{
			if (m_maxCachedIndex == -1 && m_lookupCache.size() < (unsigned int)m_visibleOffspring)
				m_lookupCache.resize(m_visibleOffspring);
			for (unsigned int k = index; k <= index + count; k++)
			{
				m_lookupCache[k].child = child;
				m_lookupCache[k].index = index;
			}
			m_maxCachedIndex = index + count;
			item -= count + 1;
			index += count + 1;
			continue;
		}

		return (*iter)->GetChild(item - 1);
	}
	return 0;
}

unsigned int CQueueItem::GetChildrenCount(bool recursive)
{
	if (!recursive)
		return m_children.size() - m_removed_at_front;

	return m_visibleOffspring;
}

bool CQueueItem::RemoveChild(CQueueItem* pItem, bool destroy /*=true*/)
{
	int oldVisibleOffspring = m_visibleOffspring;
	std::vector<CQueueItem*>::iterator iter;
	bool deleted = false;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		if (*iter == pItem)
		{
			m_visibleOffspring -= 1;
			m_visibleOffspring -= pItem->m_visibleOffspring;
			if (destroy)
				delete pItem;

			if (iter - m_children.begin() - m_removed_at_front <= 10)
			{
				m_removed_at_front++;
				unsigned int end = iter - m_children.begin();
				int c = 0;
				for (int i = end; i >= m_removed_at_front; i--, c++)
					m_children[i] = m_children[i - 1];
			}
			else
				m_children.erase(iter);

			deleted = true;
			break;
		}

		int visibleOffspring = (*iter)->m_visibleOffspring;
		if ((*iter)->RemoveChild(pItem, destroy))
		{
			m_visibleOffspring -= visibleOffspring - (*iter)->m_visibleOffspring;
			if (!((*iter)->m_children.size() - (*iter)->m_removed_at_front))
			{
				m_visibleOffspring -= 1;
				delete *iter;

				if (iter - m_children.begin() - m_removed_at_front <= 10)
				{
					m_removed_at_front++;
					unsigned int end = iter - m_children.begin();
					for (int i = end; i >= m_removed_at_front; i--)
						m_children[i] = m_children[i - 1];
				}
				else
					m_children.erase(iter);
			}

			deleted = true;
			break;
		}
	}

	if (!deleted)
		return false;

	m_maxCachedIndex = -1;

	// Propagate new children count to parent
	CQueueItem* parent = GetParent();
	while (parent)
	{
		parent->m_maxCachedIndex = -1;
		parent->m_visibleOffspring -= oldVisibleOffspring - m_visibleOffspring;
		parent = parent->GetParent();
	}

	return true;
}

bool CQueueItem::TryRemoveAll()
{
	const int oldVisibleOffspring = m_visibleOffspring;
	std::vector<CQueueItem*>::iterator iter;
	std::vector<CQueueItem*> keepChildren;
	m_visibleOffspring = 0;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		CQueueItem* pItem = *iter;
		if (pItem->TryRemoveAll())
			delete pItem;
		else
		{
			keepChildren.push_back(pItem);
			m_visibleOffspring++;
			m_visibleOffspring += pItem->GetChildrenCount(true);
		}
	}
	m_children = keepChildren;
	m_maxCachedIndex = -1;
	m_removed_at_front = 0;

	CQueueItem* parent = GetParent();
	while (parent)
	{
		parent->m_maxCachedIndex = -1;
		parent->m_visibleOffspring -= oldVisibleOffspring - m_visibleOffspring;
		parent = parent->GetParent();
	}

	return m_children.empty();
}

CQueueItem* CQueueItem::GetTopLevelItem()
{
	if (!m_parent)
		return this;

	CQueueItem* newParent = m_parent;
	CQueueItem* parent = 0;
	while (newParent)
	{
		parent = newParent;
		newParent = newParent->GetParent();
	}

	return parent;
}

const CQueueItem* CQueueItem::GetTopLevelItem() const
{
	if (!m_parent)
		return this;

	const CQueueItem* newParent = m_parent;
	const CQueueItem* parent = 0;
	while (newParent)
	{
		parent = newParent;
		newParent = newParent->GetParent();
	}

	return parent;
}

int CQueueItem::GetItemIndex() const
{
	const CQueueItem* pParent = GetParent();
	if (!pParent)
		return 0;

	int index = 1;
	for (std::vector<CQueueItem*>::const_iterator iter = pParent->m_children.begin() + pParent->m_removed_at_front; iter != pParent->m_children.end(); iter++)
	{
		if (*iter == this)
			break;

		index += (*iter)->GetChildrenCount(true) + 1;
	}

	return index + pParent->GetItemIndex();
}

CFileItem::CFileItem(CServerItem* parent, bool queued, bool download, const wxString& localFile,
					 const wxString& remoteFile, const CServerPath& remotePath, wxLongLong size)
	: m_localFile(localFile), m_remoteFile(remoteFile), m_remotePath(remotePath), m_size(size)
{
	m_parent = parent;
	m_priority = priority_normal;

	m_download = download;
	m_queued = queued;
	m_active = false;
	m_errorCount = 0;
	m_remove = false;
	m_pEngineData = 0;
	m_defaultFileExistsAction = CFileExistsNotification::unknown;
	m_edit = CEditHandler::none;
	m_madeProgress = false;
	m_onetime_action = CFileExistsNotification::unknown;
}

CFileItem::~CFileItem()
{
}

void CFileItem::SetPriority(enum QueuePriority priority)
{
	if (priority == m_priority)
		return;

	CServerItem* parent = (CServerItem*)m_parent;
	parent->SetChildPriority(this, m_priority, priority);
	m_priority = priority;
}

void CFileItem::SetPriorityRaw(enum QueuePriority priority)
{
	m_priority = priority;
}

enum QueuePriority CFileItem::GetPriority() const
{
	return m_priority;
}

void CFileItem::SetActive(const bool active)
{
	if (active && !m_active)
	{
		AddChild(new CStatusItem);
	}
	else if (!active && m_active)
	{
		CQueueItem* pItem = GetChild(0, false);
		RemoveChild(pItem);
	}
	m_active = active;
}

void CFileItem::SaveItem(TiXmlElement* pElement) const
{
	if (m_edit != CEditHandler::none)
		return;

	TiXmlElement *file = pElement->LinkEndChild(new TiXmlElement("File"))->ToElement();

	AddTextElement(file, "LocalFile", m_localFile);
	AddTextElement(file, "RemoteFile", m_remoteFile);
	AddTextElement(file, "RemotePath", m_remotePath.GetSafePath());
	AddTextElementRaw(file, "Download", m_download ? "1" : "0");
	if (m_size != -1)
		AddTextElement(file, "Size", m_size.ToString());
	if (m_errorCount)
		AddTextElement(file, "ErrorCount", m_errorCount);
	if (m_priority != priority_normal)
		AddTextElement(file, "Priority", m_priority);
	AddTextElementRaw(file, "TransferMode", m_transferSettings.binary ? "1" : "0");
	if (m_defaultFileExistsAction != CFileExistsNotification::unknown)
		AddTextElement(file, "OverwriteAction", m_defaultFileExistsAction);
}

bool CFileItem::TryRemoveAll()
{
	if (!IsActive())
		return true;

	m_remove = true;
	return false;
}

void CFileItem::SetLocalFile(const wxString &file)
{
	wxASSERT(!file.empty());
	m_localFile = file;
}

void CFileItem::SetRemoteFile(const wxString &file)
{
	wxASSERT(!file.empty());
	m_remoteFile = file;
}

CFolderItem::CFolderItem(CServerItem* parent, bool queued, const wxString& localFile)
	: CFileItem(parent, queued, true, localFile, _T(""), CServerPath(), -1)
{
}

CFolderItem::CFolderItem(CServerItem* parent, bool queued, const CServerPath& remotePath, const wxString& remoteFile)
	: CFileItem(parent, queued, false, _T(""), remoteFile, remotePath, -1)
{
}

void CFolderItem::SaveItem(TiXmlElement* pElement) const
{
	TiXmlElement *file = new TiXmlElement("Folder");

	if (m_download)
		AddTextElement(file, "LocalFile", m_localFile);
	else
	{
		AddTextElement(file, "RemoteFile", m_remoteFile);
		AddTextElement(file, "RemotePath", m_remotePath.GetSafePath());
	}
	AddTextElementRaw(file, "Download", m_download ? "1" : "0");

	pElement->LinkEndChild(file);
}

void CFolderItem::SetActive(const bool active)
{
	m_active = active;
}

CServerItem::CServerItem(const CServer& server)
{
	m_server = server;
	m_activeCount = 0;
}

CServerItem::~CServerItem()
{
}

const CServer& CServerItem::GetServer() const
{
	return m_server;
}

wxString CServerItem::GetName() const
{
	return m_server.FormatServer();
}

void CServerItem::AddChild(CQueueItem* pItem)
{
	CQueueItem::AddChild(pItem);
	if (pItem->GetType() == QueueItemType_File ||
		pItem->GetType() == QueueItemType_Folder)
		AddFileItemToList((CFileItem*)pItem);
}

void CServerItem::AddFileItemToList(CFileItem* pItem)
{
	if (!pItem)
		return;

	m_fileList[pItem->m_queued ? 0 : 1][pItem->GetPriority()].push_back(pItem);
}

void CServerItem::RemoveFileItemFromList(CFileItem* pItem)
{
	std::list<CFileItem*>& fileList = m_fileList[pItem->m_queued ? 0 : 1][pItem->GetPriority()];
	for (std::list<CFileItem*>::iterator iter = fileList.begin(); iter != fileList.end(); iter++)
	{
		if (*iter == pItem)
		{
			fileList.erase(iter);
			return;
		}
	}
	wxFAIL_MSG(_T("File item not deleted from m_fileList"));
}

void CServerItem::SetDefaultFileExistsAction(enum CFileExistsNotification::OverwriteAction action, const enum TransferDirection direction)
{
	for (std::vector<CQueueItem*>::iterator iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		CQueueItem *pItem = *iter;
		if (pItem->GetType() == QueueItemType_File)
		{
			CFileItem* pFileItem = ((CFileItem *)pItem);
			if (direction == upload && pFileItem->Download())
				continue;
			else if (direction == download && !pFileItem->Download())
				continue;
			pFileItem->m_defaultFileExistsAction = action;
		}
		else if (pItem->GetType() == QueueItemType_FolderScan)
		{
			if (direction == download)
				continue;
			((CFolderScanItem *)pItem)->m_defaultFileExistsAction = action;
		}
	}
}

CFileItem* CServerItem::GetIdleChild(bool immediateOnly, enum TransferDirection direction)
{
	int i = 0;
	for (i = (PRIORITY_COUNT - 1); i >= 0; i--)
	{
		std::list<CFileItem*>& fileList = m_fileList[1][i];
		for (std::list<CFileItem*>::iterator iter = fileList.begin(); iter != fileList.end(); iter++)
		{
			CFileItem* item = *iter;
			if (item->IsActive())
				continue;

			if (direction == both)
				return item;

			if (direction == download)
			{
				if (item->Download())
					return item;
			}
			else if (!item->Download())
				return item;
		}
	}
	if (immediateOnly)
		return 0;

	for (i = (PRIORITY_COUNT - 1); i >= 0; i--)
	{
		std::list<CFileItem*>& fileList = m_fileList[0][i];
		for (std::list<CFileItem*>::iterator iter = fileList.begin(); iter != fileList.end(); iter++)
		{
			CFileItem* item = *iter;
			if (item->IsActive())
				continue;

			if (direction == both)
				return item;

			if (direction == download)
			{
				if (item->Download())
					return item;
			}
			else if (!item->Download())
				return item;
		}
	}

	return 0;
}

bool CServerItem::RemoveChild(CQueueItem* pItem, bool destroy /*=true*/)
{
	if (!pItem)
		return false;

	if (pItem->GetType() == QueueItemType_File || pItem->GetType() == QueueItemType_Folder)
	{
		CFileItem* pFileItem = reinterpret_cast<CFileItem*>(pItem);
		RemoveFileItemFromList(pFileItem);
	}

	return CQueueItem::RemoveChild(pItem, destroy);
}

void CServerItem::QueueImmediateFiles()
{
	for (int i = 0; i < PRIORITY_COUNT; i++)
	{
		std::list<CFileItem*> activeList;
		std::list<CFileItem*>& fileList = m_fileList[1][i];
		for (std::list<CFileItem*>::reverse_iterator iter = fileList.rbegin(); iter != fileList.rend(); iter++)
		{
			CFileItem* item = *iter;
			wxASSERT(!item->m_queued);
			if (item->IsActive())
				activeList.push_front(item);
			else
			{
				item->m_queued = true;
				m_fileList[0][i].push_front(item);
			}
		}
		fileList = activeList;
	}
}

void CServerItem::QueueImmediateFile(CFileItem* pItem)
{
	if (pItem->m_queued)
		return;

	std::list<CFileItem*>& fileList = m_fileList[1][pItem->GetPriority()];
	for (std::list<CFileItem*>::iterator iter = fileList.begin(); iter != fileList.end(); iter++)
	{
		if (*iter != pItem)
			continue;

		pItem->m_queued = true;
		fileList.erase(iter);
		m_fileList[0][pItem->GetPriority()].push_front(pItem);
		return;
	}
	wxASSERT(false);
}

void CServerItem::SaveItem(TiXmlElement* pElement) const
{
	TiXmlElement *server = new TiXmlElement("Server");
	SetServer(server, m_server);

	for (std::vector<CQueueItem*>::const_iterator iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
		(*iter)->SaveItem(server);

	pElement->LinkEndChild(server);
}

wxLongLong CServerItem::GetTotalSize(int& filesWithUnknownSize, int& queuedFiles, int& folderScanCount) const
{
	wxLongLong totalSize = 0;
	for (int i = 0; i < PRIORITY_COUNT; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			const std::list<CFileItem*>& fileList = m_fileList[j][i];
			for (std::list<CFileItem*>::const_iterator iter = fileList.begin(); iter != fileList.end(); iter++)
			{
				const CFileItem* item = *iter;
				wxLongLong size = item->GetSize();
				if (size >= 0)
					totalSize += size;
				else
					filesWithUnknownSize++;
			}
		}
	}

	for (std::vector<CQueueItem*>::const_iterator iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		if ((*iter)->GetType() == QueueItemType_File ||
			(*iter)->GetType() == QueueItemType_Folder)
			queuedFiles++;
		else if ((*iter)->GetType() == QueueItemType_FolderScan)
			folderScanCount++;
	}

	return totalSize;
}

bool CServerItem::TryRemoveAll()
{
	const int oldVisibleOffspring = m_visibleOffspring;
	std::vector<CQueueItem*>::iterator iter;
	std::vector<CQueueItem*> keepChildren;
	m_visibleOffspring = 0;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		CQueueItem* pItem = *iter;
		if (pItem->TryRemoveAll())
		{
			if (pItem->GetType() == QueueItemType_File || pItem->GetType() == QueueItemType_Folder)
			{
				CFileItem* pFileItem = reinterpret_cast<CFileItem*>(pItem);
				RemoveFileItemFromList(pFileItem);
			}
			delete pItem;
		}
		else
		{
			keepChildren.push_back(pItem);
			m_visibleOffspring++;
			m_visibleOffspring += pItem->GetChildrenCount(true);
		}
	}
	m_children = keepChildren;
	m_removed_at_front = 0;

	CQueueItem* parent = GetParent();
	while (parent)
	{
		parent->m_visibleOffspring -= oldVisibleOffspring - m_visibleOffspring;
		parent = parent->GetParent();
	}

	return m_children.empty();
}

void CServerItem::DetachChildren()
{
	wxASSERT(!m_activeCount);

	m_children.clear();
	m_visibleOffspring = 0;
	m_maxCachedIndex = -1;
	m_removed_at_front = 0;

	for (int i = 0; i < 2; i++)
		for (int j = 0; j < PRIORITY_COUNT; j++)
			m_fileList[i][j].clear();
}

void CServerItem::SetPriority(enum QueuePriority priority)
{
	std::vector<CQueueItem*>::iterator iter;
	for (iter = m_children.begin() + m_removed_at_front; iter != m_children.end(); iter++)
	{
		if ((*iter)->GetType() == QueueItemType_File)
			((CFileItem*)(*iter))->SetPriorityRaw(priority);
		else
			(*iter)->SetPriority(priority);
	}

	for (int i = 0; i < 2; i++)
		for (int j = 0; j < PRIORITY_COUNT; j++)
			if (j != priority)
				m_fileList[i][priority].splice(m_fileList[i][priority].end(), m_fileList[i][j]);
}

void CServerItem::SetChildPriority(CFileItem* pItem, enum QueuePriority oldPriority, enum QueuePriority newPriority)
{
	int i = pItem->Queued() ? 0 : 1;

	for (std::list<CFileItem*>::iterator iter = m_fileList[i][oldPriority].begin(); iter != m_fileList[i][oldPriority].end(); iter++)
	{
		if (*iter != pItem)
			continue;

		m_fileList[i][oldPriority].erase(iter);
		m_fileList[i][newPriority].push_back(pItem);
		return;
	}

	wxFAIL;
}

CFolderScanItem::CFolderScanItem(CServerItem* parent, bool queued, bool download, const wxString& localPath, const CServerPath& remotePath)
{
	m_parent = parent;

	m_download = download;
	m_localPath = localPath;
	m_remotePath = remotePath;
	m_queued = queued;
	m_remove = false;
	m_active = false;
	m_count = 0;
	m_dir_is_empty = false;

	m_defaultFileExistsAction = CFileExistsNotification::unknown;
}

bool CFolderScanItem::TryRemoveAll()
{
	if (!m_active)
		return true;

	m_remove = true;
	return false;
}

// --------------
// CQueueViewBase
// --------------

BEGIN_EVENT_TABLE(CQueueViewBase, wxListCtrlEx)
EVT_ERASE_BACKGROUND(CQueueViewBase::OnEraseBackground)
EVT_NAVIGATION_KEY(CQueueViewBase::OnNavigationKey)
EVT_CHAR(CQueueViewBase::OnChar)
EVT_LIST_COL_END_DRAG(wxID_ANY, CQueueViewBase::OnEndColumnDrag)
EVT_TIMER(wxID_ANY, CQueueViewBase::OnTimer)
EVT_KEY_DOWN(CQueueViewBase::OnKeyDown)
END_EVENT_TABLE()

CQueueViewBase::CQueueViewBase(CQueue* parent, int index, const wxString& title)
	: wxListCtrlEx(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxCLIP_CHILDREN | wxLC_REPORT | wxLC_VIRTUAL | wxSUNKEN_BORDER | wxTAB_TRAVERSAL),
	  m_pageIndex(index), m_title(title)
{
	m_pQueue = parent;
	m_insertionStart = -1;
	m_insertionCount = 0;
	m_itemCount = 0;
	m_allowBackgroundErase = true;

	m_fileCount = 0;
	m_folderScanCount = 0;
	m_fileCountChanged = false;
	m_folderScanCountChanged = false;

	// Create and assign the image list for the queue
	wxImageList* pImageList = new wxImageList(16, 16);

	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_SERVER"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FILE"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDERCLOSED"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDER"),  wxART_OTHER, wxSize(16, 16)));

	AssignImageList(pImageList, wxIMAGE_LIST_SMALL);

	m_filecount_delay_timer.SetOwner(this);
}

CQueueViewBase::~CQueueViewBase()
{
	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
		delete *iter;
}

CQueueItem* CQueueViewBase::GetQueueItem(unsigned int item)
{
	std::vector<CServerItem*>::iterator iter;
	for (iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		if (!item)
			return *iter;

		unsigned int count = (*iter)->GetChildrenCount(true);
		if (item > count)
		{
			item -= count + 1;
			continue;
		}

		return (*iter)->GetChild(item - 1);
	}
	return 0;
}

int CQueueViewBase::GetItemIndex(const CQueueItem* item)
{
	const CQueueItem* pTopLevelItem = item->GetTopLevelItem();

	int index = 0;
	for (std::vector<CServerItem*>::const_iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		if (pTopLevelItem == *iter)
			break;

		index += (*iter)->GetChildrenCount(true) + 1;
	}

	return index + item->GetItemIndex();
}

void CQueueViewBase::OnEraseBackground(wxEraseEvent& event)
{
	if (m_allowBackgroundErase)
		event.Skip();
}

wxString CQueueViewBase::OnGetItemText(long item, long column) const
{
	CQueueViewBase* pThis = const_cast<CQueueViewBase*>(this);

	CQueueItem* pItem = pThis->GetQueueItem(item);
	if (!pItem)
		return _T("");

	switch (pItem->GetType())
	{
	case QueueItemType_Server:
		{
			CServerItem* pServerItem = reinterpret_cast<CServerItem*>(pItem);
			if (!column)
				return pServerItem->GetName();
		}
		break;
	case QueueItemType_File:
		{
			CFileItem* pFileItem = reinterpret_cast<CFileItem*>(pItem);
			switch (column)
			{
			case 0:
				return pFileItem->GetIndent() + pFileItem->GetLocalFile();
			case 1:
				if (pFileItem->Download())
					if (pFileItem->Queued())
						return _T("<--");
					else
						return _T("<<--");
				else
					if (pFileItem->Queued())
						return _T("-->");
					else
						return _T("-->>");
				break;
			case 2:
				return pFileItem->GetRemotePath().FormatFilename(pFileItem->GetRemoteFile());
			case 3:
				{
					const wxLongLong& size = pFileItem->GetSize();
					if (size >= 0)
						return CSizeFormat::Format(size);
					else
						return _T("?");
				}
			case 4:
				switch (pFileItem->GetPriority())
				{
				case 0:
					return _("Lowest");
				case 1:
					return _("Low");
				default:
				case 2:
					return _("Normal");
				case 3:
					return _("High");
				case 4:
					return _("Highest");
				}
				break;
			case 5:
				return pFileItem->m_statusMessage;
			default:
				break;
			}
		}
		break;
	case QueueItemType_FolderScan:
		{
			CFolderScanItem* pFolderItem = reinterpret_cast<CFolderScanItem*>(pItem);
			switch (column)
			{
			case 0:
				return _T("  ") + pFolderItem->GetLocalPath();
			case 1:
				if (pFolderItem->Download())
					if (pFolderItem->Queued())
						return _T("<--");
					else
						return _T("<<--");
				else
					if (pFolderItem->Queued())
						return _T("-->");
					else
						return _T("-->>");
				break;
			case 2:
				return pFolderItem->GetRemotePath().GetPath();
			case 5:
				return pFolderItem->m_statusMessage;
			default:
				break;
			}
		}
		break;
	case QueueItemType_Folder:
		{
			CFileItem* pFolderItem = reinterpret_cast<CFolderItem*>(pItem);
			switch (column)
			{
			case 0:
				if (pFolderItem->Download())
					return pFolderItem->GetIndent() + pFolderItem->GetLocalFile();
				break;
			case 1:
				if (pFolderItem->Download())
					if (pFolderItem->Queued())
						return _T("<--");
					else
						return _T("<<--");
				else
					if (pFolderItem->Queued())
						return _T("-->");
					else
						return _T("-->>");
				break;
			case 2:
				if (!pFolderItem->Download())
				{
					if (pFolderItem->GetRemoteFile() == _T(""))
						return pFolderItem->GetRemotePath().GetPath();
					else
						return pFolderItem->GetRemotePath().FormatFilename(pFolderItem->GetRemoteFile());
				}
				break;
			case 4:
				switch (pFolderItem->GetPriority())
				{
				case 0:
					return _("Lowest");
				case 1:
					return _("Low");
				default:
				case 2:
					return _("Normal");
				case 3:
					return _("High");
				case 4:
					return _("Highest");
				}
				break;
			case 5:
				return pFolderItem->m_statusMessage;
			default:
				break;
			}
		}
		break;
	default:
		break;
	}

	return _T("");
}

int CQueueViewBase::OnGetItemImage(long item) const
{
	CQueueViewBase* pThis = const_cast<CQueueViewBase*>(this);

	CQueueItem* pItem = pThis->GetQueueItem(item);
	if (!pItem)
		return -1;

	switch (pItem->GetType())
	{
	case QueueItemType_Server:
		return 0;
	case QueueItemType_File:
		return 1;
	case QueueItemType_FolderScan:
	case QueueItemType_Folder:
			return 3;
	default:
		return -1;
	}

	return -1;
}

void CQueueViewBase::UpdateSelections_ItemAdded(int added)
{
	// This is the fastest algorithm I can think of to move all
	// selections. Though worst case is still O(n), as with every algorithm to
	// move selections.

#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	const int selection_count = GetSelectedItemCount();
	if (!selection_count)
		return;
#endif

	// Go through all items, keep record of the previous selected item
	int item = GetNextItem(added - 1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	int prevItem = -1;
	while (item != -1)
	{
		if (prevItem != -1)
		{
			if (prevItem + 1 != item)
			{
				// Previous selected item was not the direct predecessor
				// That means we have to select the successor of prevItem
				// and unselect current item
				SetItemState(prevItem + 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
				SetItemState(item, 0, wxLIST_STATE_SELECTED);
			}
		}
		else
		{
			// First selected item, no predecessor yet. We have to unselect
			SetItemState(item, 0, wxLIST_STATE_SELECTED);
		}
		prevItem = item;

		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}
	if (prevItem != -1 && prevItem < m_itemCount - 1)
	{
		// Move the very last selected item
		SetItemState(prevItem + 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
	}

	SetItemState(added, 0, wxLIST_STATE_SELECTED);
}

void CQueueViewBase::UpdateSelections_ItemRangeAdded(int added, int count)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	const int selection_count = GetSelectedItemCount();
	if (!selection_count)
		return;
#endif

	std::list<int> itemsToSelect;

	// Go through all selected items
	int item = GetNextItem(added - 1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	while (item != -1)
	{
		// Select new items preceding to current one
		while (!itemsToSelect.empty() && itemsToSelect.front() < item)
		{
			SetItemState(itemsToSelect.front(), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
			itemsToSelect.pop_front();
		}
		if (itemsToSelect.empty())
			SetItemState(item, 0, wxLIST_STATE_SELECTED);
		else if (itemsToSelect.front() == item)
			itemsToSelect.pop_front();

		itemsToSelect.push_back(item + count);

		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}
	for (std::list<int>::const_iterator iter = itemsToSelect.begin(); iter != itemsToSelect.end(); iter++)
		SetItemState(*iter, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

void CQueueViewBase::UpdateSelections_ItemRemoved(int removed)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	const int selection_count = GetSelectedItemCount();
	if (!selection_count)
		return;
#endif

	SetItemState(removed, 0, wxLIST_STATE_SELECTED);

	int item = GetNextItem(removed - 1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	int prevItem = -1;
	while (item != -1)
	{
		if (prevItem != -1)
		{
			if (prevItem + 1 != item)
			{
				// Previous selected item was not the direct predecessor
				// That means we have to select our predecessor and unselect
				// prevItem
				SetItemState(prevItem, 0, wxLIST_STATE_SELECTED);
				SetItemState(item - 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
			}
		}
		else
		{
			// First selected item, no predecessor yet. We have to unselect
			SetItemState(item - 1, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
		}
		prevItem = item;

		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}
	if (prevItem != -1)
	{
		SetItemState(prevItem, 0, wxLIST_STATE_SELECTED);
	}
}

void CQueueViewBase::UpdateSelections_ItemRangeRemoved(int removed, int count)
{
#ifndef __WXMSW__
	// GetNextItem is O(n) if nothing is selected, GetSelectedItemCount() is O(1)
	const int selection_count = GetSelectedItemCount();
	if (!selection_count)
		return;
#endif

	SetItemState(removed, 0, wxLIST_STATE_SELECTED);

	std::list<int> itemsToUnselect;

	int item = GetNextItem(removed - 1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);

	while (item != -1)
	{
		// Unselect new items preceding to current one
		while (!itemsToUnselect.empty() && itemsToUnselect.front() < item - count)
		{
			SetItemState(itemsToUnselect.front(), 0, wxLIST_STATE_SELECTED);
			itemsToUnselect.pop_front();
		}

		if (itemsToUnselect.empty())
			SetItemState(item - count, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
		else if (itemsToUnselect.front() == item - count)
			itemsToUnselect.pop_front();
		else
			SetItemState(item - count, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);

		itemsToUnselect.push_back(item);

		item = GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	}
	for (std::list<int>::const_iterator iter = itemsToUnselect.begin(); iter != itemsToUnselect.end(); iter++)
		SetItemState(*iter, 0, wxLIST_STATE_SELECTED);
}

void CQueueViewBase::CreateColumns(const wxString& lastColumnName)
{
	const unsigned long widths[6] = { 180, 60, 180, 80, 60, 150 };

	AddColumn(_("Server/Local file"), wxLIST_FORMAT_LEFT, widths[0]);
	AddColumn(_("Direction"), wxLIST_FORMAT_CENTER, widths[1]);
	AddColumn(_("Remote file"), wxLIST_FORMAT_LEFT, widths[2]);
	AddColumn(_("Size"), wxLIST_FORMAT_RIGHT, widths[3]);
	AddColumn(_("Priority"), wxLIST_FORMAT_LEFT, widths[4]);
	if (lastColumnName != _T(""))
		AddColumn(lastColumnName, wxLIST_FORMAT_LEFT, widths[5]);

	LoadColumnSettings(OPTION_QUEUE_COLUMN_WIDTHS, -1, -1);
}

CServerItem* CQueueViewBase::GetServerItem(const CServer& server)
{
	for (std::vector<CServerItem*>::iterator iter = m_serverList.begin(); iter != m_serverList.end(); iter++)
	{
		if ((*iter)->GetServer() == server)
			return *iter;
	}
	return NULL;
}

CServerItem* CQueueViewBase::CreateServerItem(const CServer& server)
{
	CServerItem* pItem = GetServerItem(server);

	if (!pItem)
	{
		pItem = new CServerItem(server);
		m_serverList.push_back(pItem);
		m_itemCount++;

		wxASSERT(m_insertionStart == -1);
		wxASSERT(m_insertionCount == 0);

		m_insertionStart = GetItemIndex(pItem);
		m_insertionCount = 1;
	}

	return pItem;
}

void CQueueViewBase::CommitChanges()
{
	SaveSetItemCount(m_itemCount);

	if (m_insertionStart != -1)
	{
		wxASSERT(m_insertionCount != 0);
		if (m_insertionCount == 1)
			UpdateSelections_ItemAdded(m_insertionStart);
		else
			UpdateSelections_ItemRangeAdded(m_insertionStart, m_insertionCount);

		m_insertionStart = -1;
		m_insertionCount = 0;
	}

	if (m_fileCountChanged || m_folderScanCountChanged)
		DisplayNumberQueuedFiles();
}

void CQueueViewBase::DisplayNumberQueuedFiles()
{
	if (m_filecount_delay_timer.IsRunning())
	{
		m_fileCountChanged = true;
		return;
	}

	wxString str;
	if (m_fileCount > 0)
	{
		if (!m_folderScanCount)
			str.Printf(m_title + _T(" (%d)"), m_fileCount);
		else
			str.Printf(m_title + _T(" (%d+)"), m_fileCount);
	}
	else
	{
		if (m_folderScanCount)
			str.Printf(m_title + _T(" (0+)"), m_fileCount);
		else
			str = m_title;
	}
	m_pQueue->SetPageText(m_pageIndex, str);

	m_fileCountChanged = false;
	m_folderScanCountChanged = false;

	m_filecount_delay_timer.Start(200, true);
}

void CQueueViewBase::InsertItem(CServerItem* pServerItem, CQueueItem* pItem)
{
	const int newIndex = GetItemIndex(pServerItem) + pServerItem->GetChildrenCount(true) + 1;

	pServerItem->AddChild(pItem);
	m_itemCount++;

	if (m_insertionStart == -1)
		m_insertionStart = newIndex;
	m_insertionCount++;

	if (pItem->GetType() == QueueItemType_File || pItem->GetType() == QueueItemType_Folder)
	{
		m_fileCount++;
		m_fileCountChanged = true;
	}
	else if (pItem->GetType() == QueueItemType_FolderScan)
	{
		m_folderScanCount++;
		m_folderScanCountChanged = true;
	}
}

bool CQueueViewBase::RemoveItem(CQueueItem* pItem, bool destroy, bool updateItemCount /*=true*/, bool updateSelections /*=true*/)
{
	if (pItem->GetType() == QueueItemType_File || pItem->GetType() == QueueItemType_Folder)
	{
		wxASSERT(m_fileCount > 0);
		m_fileCount--;
		m_fileCountChanged = true;
	}
	else if (pItem->GetType() == QueueItemType_FolderScan)
	{
		wxASSERT(m_folderScanCount > 0);
		m_folderScanCount--;
		m_folderScanCountChanged = true;

	}

	int index = 0;
	if (updateSelections)
		index = GetItemIndex(pItem);

	CQueueItem* topLevelItem = pItem->GetTopLevelItem();

	int count = topLevelItem->GetChildrenCount(true);
	topLevelItem->RemoveChild(pItem, destroy);

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
		if (updateItemCount)
			SaveSetItemCount(m_itemCount);

		didRemoveParent = true;
	}
	else
	{
		count -= topLevelItem->GetChildrenCount(true);

		if (updateSelections)
			UpdateSelections_ItemRangeRemoved(index, count);

		m_itemCount -= count;
		if (updateItemCount)
			SaveSetItemCount(m_itemCount);

		didRemoveParent = false;
	}

	if (updateItemCount)
	{
		if (m_fileCountChanged || m_folderScanCountChanged)
			DisplayNumberQueuedFiles();
		if (oldCount > m_itemCount)
		{
			bool eraseBackground = GetTopItem() + GetCountPerPage() + 1 >= m_itemCount;
			RefreshListOnly(eraseBackground);
			if (eraseBackground)
				Update();
		}
	}

	return didRemoveParent;
}

void CQueueViewBase::RefreshItem(const CQueueItem* pItem)
{
	wxASSERT(pItem);
	int index = GetItemIndex(pItem);

#ifdef __WXMSW__
	wxRect rect;
	GetItemRect(index, rect);
	RefreshRect(rect, false);
#else
	wxListCtrl::RefreshItem(index);
#endif
}

void CQueueViewBase::OnNavigationKey(wxNavigationKeyEvent& event)
{
	event.SetEventObject(m_pQueue);
	m_pQueue->ProcessEvent(event);
}

void CQueueViewBase::OnChar(wxKeyEvent& event)
{
	const int code = event.GetKeyCode();
	if (code != WXK_LEFT && code != WXK_RIGHT)
	{
		event.Skip();
		return;
	}

	bool forward;
	if (GetLayoutDirection() != wxLayout_RightToLeft)
		forward = code == WXK_RIGHT;
	else
		forward = code == WXK_LEFT;

	int selection = m_pQueue->GetSelection();
	if (selection > 0 && !forward)
		selection--;
	else if (selection < (int)m_pQueue->GetPageCount() - 1 && forward)
		selection++;
	else
		return;

	m_pQueue->SetSelection(selection);
}

void CQueueViewBase::OnEndColumnDrag(wxListEvent& event)
{
	for (unsigned int i = 0; i < m_pQueue->GetPageCount(); i++)
	{
		wxWindow *page = m_pQueue->GetPage(i);

		wxListCtrl* queue_page = wxDynamicCast(page, wxListCtrl);
		if (!queue_page || queue_page == this)
			continue;

		for (int col = 0; col < wxMin(GetColumnCount(), queue_page->GetColumnCount()); col++)
			queue_page->SetColumnWidth(col, GetColumnWidth(col));
	}
}

void CQueueViewBase::OnTimer(wxTimerEvent& event)
{
	if (event.GetId() != m_filecount_delay_timer.GetId())
	{
		event.Skip();
		return;
	}

	if (m_fileCountChanged || m_folderScanCountChanged)
		DisplayNumberQueuedFiles();
}

void CQueueViewBase::OnKeyDown(wxKeyEvent& event)
{
	const int code = event.GetKeyCode();
	const int mods = event.GetModifiers();
	if (code == 'A' && (mods == wxMOD_CMD || mods == (wxMOD_CONTROL | wxMOD_META)))
	{
		for (unsigned int i = 0; i < (unsigned int)GetItemCount(); i++)
			SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
	}
	else
		event.Skip();
}

// ------
// CQueue
// ------

CQueue::CQueue(wxWindow* parent, CMainFrame *pMainFrame, CAsyncRequestQueue *pAsyncRequestQueue)
{
	Create(parent, -1, wxDefaultPosition, wxDefaultSize, wxNO_BORDER | wxAUI_NB_BOTTOM);
	SetExArtProvider();

	m_pQueueView = new CQueueView(this, 0, pMainFrame, pAsyncRequestQueue);
	AddPage(m_pQueueView, m_pQueueView->GetTitle());

	m_pQueueView_Failed = new CQueueViewFailed(this, 1);
	AddPage(m_pQueueView_Failed, m_pQueueView_Failed->GetTitle());
	m_pQueueView_Successful = new CQueueViewSuccessful(this, 2);
	AddPage(m_pQueueView_Successful, m_pQueueView_Successful->GetTitle());

	RemoveExtraBorders();

	m_pQueueView->LoadQueue();
}

void CQueue::SetFocus()
{
	GetPage(GetSelection())->SetFocus();
}
