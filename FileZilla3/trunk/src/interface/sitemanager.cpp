#include "FileZilla.h"
#include "sitemanager.h"
#include "Options.h"
#include "xmlfunctions.h"
#include "filezillaapp.h"
#include "ipcmutex.h"
#include "wrapengine.h"
#include "conditionaldialog.h"
#include "window_state_manager.h"
#include <wx/dnd.h>

std::map<int, CSiteManagerItemData*> CSiteManager::m_idMap;

BEGIN_EVENT_TABLE(CSiteManager, wxDialogEx)
EVT_BUTTON(XRCID("wxID_OK"), CSiteManager::OnOK)
EVT_BUTTON(XRCID("wxID_CANCEL"), CSiteManager::OnCancel)
EVT_BUTTON(XRCID("ID_CONNECT"), CSiteManager::OnConnect)
EVT_BUTTON(XRCID("ID_NEWSITE"), CSiteManager::OnNewSite)
EVT_BUTTON(XRCID("ID_NEWFOLDER"), CSiteManager::OnNewFolder)
EVT_BUTTON(XRCID("ID_RENAME"), CSiteManager::OnRename)
EVT_BUTTON(XRCID("ID_DELETE"), CSiteManager::OnDelete)
EVT_TREE_BEGIN_LABEL_EDIT(XRCID("ID_SITETREE"), CSiteManager::OnBeginLabelEdit)
EVT_TREE_END_LABEL_EDIT(XRCID("ID_SITETREE"), CSiteManager::OnEndLabelEdit)
EVT_TREE_SEL_CHANGING(XRCID("ID_SITETREE"), CSiteManager::OnSelChanging)
EVT_TREE_SEL_CHANGED(XRCID("ID_SITETREE"), CSiteManager::OnSelChanged)
EVT_CHOICE(XRCID("ID_LOGONTYPE"), CSiteManager::OnLogontypeSelChanged)
EVT_BUTTON(XRCID("ID_BROWSE"), CSiteManager::OnRemoteDirBrowse)
EVT_TREE_ITEM_ACTIVATED(XRCID("ID_SITETREE"), CSiteManager::OnItemActivated)
EVT_CHECKBOX(XRCID("ID_LIMITMULTIPLE"), CSiteManager::OnLimitMultipleConnectionsChanged)
EVT_RADIOBUTTON(XRCID("ID_CHARSET_AUTO"), CSiteManager::OnCharsetChange)
EVT_RADIOBUTTON(XRCID("ID_CHARSET_UTF8"), CSiteManager::OnCharsetChange)
EVT_RADIOBUTTON(XRCID("ID_CHARSET_CUSTOM"), CSiteManager::OnCharsetChange)
EVT_CHOICE(XRCID("ID_PROTOCOL"), CSiteManager::OnProtocolSelChanged)
EVT_BUTTON(XRCID("ID_COPY"), CSiteManager::OnCopySite)
EVT_TREE_BEGIN_DRAG(XRCID("ID_SITETREE"), CSiteManager::OnBeginDrag)
EVT_CHAR(CSiteManager::OnChar)
END_EVENT_TABLE()

class CSiteManagerDataObject : public wxDataObjectSimple
{
public:
	CSiteManagerDataObject()
		: wxDataObjectSimple(wxDataFormat(_T("FileZilla3SiteManagerObject")))
	{
	}

	// GTK doesn't like data size of 0
	virtual size_t GetDataSize() const { return 1; }

	virtual bool GetDataHere(void *buf) const { memset(buf, 0, 1); return true; }

	virtual bool SetData(size_t len, const void *buf) { return true; }
};

class CSiteManagerDropTarget : public wxDropTarget
{
public:
	CSiteManagerDropTarget(CSiteManager* pSiteManager)
		: wxDropTarget(new CSiteManagerDataObject())
	{
		m_pSiteManager = pSiteManager;
	}

	virtual wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def)
	{
		ClearDropHighlight();
		if (def == wxDragError ||
			def == wxDragNone ||
			def == wxDragCancel)
		{
			return def;
		}

		wxTreeItemId hit = GetHit(wxPoint(x, y));
		if (!hit)
			return wxDragNone;
		if (hit == m_pSiteManager->m_dropSource)
			return wxDragNone;

		const bool predefined = m_pSiteManager->IsPredefinedItem(hit);
		if (predefined)
			return wxDragNone;

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		if (pTree->GetItemData(hit))
			return wxDragNone;

		wxTreeItemId item = hit;
		while (item != pTree->GetRootItem())
		{
			if (item == m_pSiteManager->m_dropSource)
			{
				ClearDropHighlight();
				return wxDragNone;
			}
			item = pTree->GetItemParent(item);
		}

		if (def == wxDragMove && pTree->GetItemParent(m_pSiteManager->m_dropSource) == hit)
			return wxDragNone;

		if (!m_pSiteManager->MoveItems(m_pSiteManager->m_dropSource, hit, def == wxDragCopy))
			return wxDragNone;

		return def;
	}

	virtual bool OnDrop(wxCoord x, wxCoord y)
	{
		ClearDropHighlight();

		wxTreeItemId hit = GetHit(wxPoint(x, y));
		if (!hit)
			return false;
		if (hit == m_pSiteManager->m_dropSource)
			return false;

		const bool predefined = m_pSiteManager->IsPredefinedItem(hit);
		if (predefined)
			return false;

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		if (pTree->GetItemData(hit))
			return false;

		wxTreeItemId item = hit;
		while (item != pTree->GetRootItem())
		{
			if (item == m_pSiteManager->m_dropSource)
			{
				ClearDropHighlight();
				return false;
			}
			item = pTree->GetItemParent(item);
		}

		return true;
	}

	virtual void OnLeave()
	{
		ClearDropHighlight();
	}

	virtual wxDragResult OnEnter(wxCoord x, wxCoord y, wxDragResult def)
	{
		return OnDragOver(x, y, def);
	}

	wxTreeItemId GetHit(const wxPoint& point)
	{
		int flags = 0;

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		wxTreeItemId hit = pTree->HitTest(point, flags);

		if (flags & (wxTREE_HITTEST_ABOVE | wxTREE_HITTEST_BELOW | wxTREE_HITTEST_NOWHERE | wxTREE_HITTEST_TOLEFT | wxTREE_HITTEST_TORIGHT))
			return wxTreeItemId();

		return hit;
	}

	virtual wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult def)
	{
		if (def == wxDragError ||
			def == wxDragNone ||
			def == wxDragCancel)
		{
			ClearDropHighlight();
			return def;
		}

		wxTreeItemId hit = GetHit(wxPoint(x, y));
		if (!hit)
		{
			ClearDropHighlight();
			return wxDragNone;
		}
		if (hit == m_pSiteManager->m_dropSource)
		{
			ClearDropHighlight();
			return wxDragNone;
		}

		const bool predefined = m_pSiteManager->IsPredefinedItem(hit);
		if (predefined)
		{
			ClearDropHighlight();
			return wxDragNone;
		}

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(hit));
		if (data)
		{
			ClearDropHighlight();
			return wxDragNone;
		}

		wxTreeItemId item = hit;
		while (item != pTree->GetRootItem())
		{
			if (item == m_pSiteManager->m_dropSource)
			{
				ClearDropHighlight();
				return wxDragNone;
			}
			item = pTree->GetItemParent(item);
		}

		if (def == wxDragMove && pTree->GetItemParent(m_pSiteManager->m_dropSource) == hit)
		{
			ClearDropHighlight();
			return wxDragNone;
		}

		DisplayDropHighlight(hit);

		return def;
	}

	void ClearDropHighlight()
	{
		if (m_dropHighlight == wxTreeItemId())
			return;

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		pTree->SetItemDropHighlight(m_dropHighlight, false);
		m_dropHighlight = wxTreeItemId();
	}

	void DisplayDropHighlight(wxTreeItemId item)
	{
		ClearDropHighlight();

		wxTreeCtrl *pTree = XRCCTRL(*m_pSiteManager, "ID_SITETREE", wxTreeCtrl);
		pTree->SetItemDropHighlight(item, true);
		m_dropHighlight = item;
	}

protected:
	CSiteManager* m_pSiteManager;
	wxTreeItemId m_dropHighlight;
};

CSiteManager::CSiteManager()
{
	m_pSiteManagerMutex = 0;
	m_pWindowStateManager = 0;
}

CSiteManager::~CSiteManager()
{
	delete m_pSiteManagerMutex;

	if (m_pWindowStateManager)
	{
		m_pWindowStateManager->Remember(OPTION_SITEMANAGER_POSITION);
		delete m_pWindowStateManager;
	}
}

bool CSiteManager::Create(wxWindow* parent, const CServer* pServer /*=0*/)
{
	m_pSiteManagerMutex = new CInterProcessMutex(MUTEX_SITEMANAGERGLOBAL, false);
	if (!m_pSiteManagerMutex->TryLock())
	{
		int answer = wxMessageBox(_("The Site Manager is opened in another instance of FileZilla 3.\nDo you want to continue? Any changes made in the Site Manager won't be saved then."),
								  _("Site Manager already open"), wxYES_NO | wxICON_QUESTION);
		if (answer != wxYES)
			return false;

		delete m_pSiteManagerMutex;
		m_pSiteManagerMutex = 0;
	}
	CreateControls(parent);

	// Now create the imagelist for the site tree
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;
	wxImageList* pImageList = new wxImageList(16, 16);

	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDERCLOSED"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_FOLDER"),  wxART_OTHER, wxSize(16, 16)));
	pImageList->Add(wxArtProvider::GetBitmap(_T("ART_SERVER"),  wxART_OTHER, wxSize(16, 16)));

	pTree->AssignImageList(pImageList);

	Layout();
	wxGetApp().GetWrapEngine()->WrapRecursive(this, 1.33, "Site Manager");

	wxSize minSize = GetSizer()->GetMinSize();

	wxSize size = GetSize();
	wxSize clientSize = GetClientSize();
	SetMinSize(GetSizer()->GetMinSize() + size - clientSize);
	SetClientSize(minSize);

	Load();

	XRCCTRL(*this, "ID_TRANSFERMODE_DEFAULT", wxRadioButton)->Update();
	XRCCTRL(*this, "ID_TRANSFERMODE_ACTIVE", wxRadioButton)->Update();
	XRCCTRL(*this, "ID_TRANSFERMODE_PASSIVE", wxRadioButton)->Update();

	SetCtrlState();

	m_pWindowStateManager = new CWindowStateManager(this);
	m_pWindowStateManager->Restore(OPTION_SITEMANAGER_POSITION);

	pTree->SetDropTarget(new CSiteManagerDropTarget(this));

#ifdef __WXGTK__
	CSiteManagerItemData* data = 0;
	wxTreeItemId item = pTree->GetSelection();
	if (item.IsOk())
		data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		XRCCTRL(*this, "wxID_OK", wxButton)->SetFocus();
#endif

	if (pServer)
		CopyAddServer(*pServer);

	return true;
}

void CSiteManager::CreateControls(wxWindow* parent)
{
	wxXmlResource::Get()->LoadDialog(this, parent, _T("ID_SITEMANAGER"));

	wxChoice *pProtocol = XRCCTRL(*this, "ID_PROTOCOL", wxChoice);
	pProtocol->Append(CServer::GetProtocolName(FTP));
	pProtocol->Append(CServer::GetProtocolName(SFTP));
	pProtocol->Append(CServer::GetProtocolName(FTPS));
	pProtocol->Append(CServer::GetProtocolName(FTPES));

	wxChoice *pChoice = XRCCTRL(*this, "ID_SERVERTYPE", wxChoice);
	wxASSERT(pChoice);
	for (int i = 0; i < SERVERTYPE_MAX; i++)
		pChoice->Append(CServer::GetNameFromServerType((enum ServerType)i));
}

void CSiteManager::OnOK(wxCommandEvent& event)
{
	if (!Verify())
		return;

	UpdateServer();

	Save();

	RememberLastSelected();

	EndModal(wxID_OK);
}

void CSiteManager::OnCancel(wxCommandEvent& event)
{
	EndModal(wxID_CANCEL);
}

void CSiteManager::OnConnect(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
	{
		wxBell();
		return;
	}

	if (!Verify())
	{
		wxBell();
		return;
	}

	UpdateServer();

	Save();

	RememberLastSelected();

	EndModal(wxID_YES);
}

class CSiteManagerXmlHandler
{
public:
	virtual ~CSiteManagerXmlHandler() {};

	// Adds a folder and descents
	virtual bool AddFolder(const wxString& name, bool expanded) = 0;
	virtual bool AddSite(const wxString& name, CSiteManagerItemData* data) = 0;

	// Go up a level
	virtual bool LevelUp() = 0; // *Ding*
};

class CSiteManagerXmlHandler_Tree : public CSiteManagerXmlHandler
{
public:
	CSiteManagerXmlHandler_Tree(wxTreeCtrl* pTree, wxTreeItemId root, const wxString& lastSelection)
		: m_pTree(pTree), m_item(root)
	{
		if (!CSiteManager::UnescapeSitePath(lastSelection, m_lastSelection))
			m_lastSelection.clear();
		m_wrong_sel_depth = 0;
	}

	virtual ~CSiteManagerXmlHandler_Tree()
	{
		m_pTree->SortChildren(m_item);
		m_pTree->Expand(m_item);
	}

	virtual bool AddFolder(const wxString& name, bool expanded)
	{
		wxTreeItemId newItem = m_pTree->AppendItem(m_item, name, 0, 0);
		m_pTree->SetItemImage(newItem, 1, wxTreeItemIcon_Expanded);
		m_pTree->SetItemImage(newItem, 1, wxTreeItemIcon_SelectedExpanded);

		m_item = newItem;
		m_expand.push_back(expanded);

		if (!m_wrong_sel_depth && !m_lastSelection.empty())
		{
			const wxString& first = m_lastSelection.front();
			if (first == name)
			{
				m_lastSelection.pop_front();
				if (m_lastSelection.empty())
					m_pTree->SelectItem(newItem);
			}
			else
				m_wrong_sel_depth++;
		}
		else
			m_wrong_sel_depth++;

		return true;
	}

	virtual bool AddSite(const wxString& name, CSiteManagerItemData* data)
	{
		wxTreeItemId newItem = m_pTree->AppendItem(m_item, name, 2, 2, data);

		if (!m_wrong_sel_depth && !m_lastSelection.empty())
		{
			const wxString& first = m_lastSelection.front();
			if (first == name)
			{
				m_lastSelection.clear();
				m_pTree->SelectItem(newItem);
			}
		}

		return true;
	}

	virtual bool LevelUp()
	{
		if (m_wrong_sel_depth)
			m_wrong_sel_depth--;

		if (!m_expand.empty())
		{
			const bool expand = m_expand.back();
			m_expand.pop_back();
			if (expand)
				m_pTree->Expand(m_item);
		}
		m_pTree->SortChildren(m_item);

		wxTreeItemId parent = m_pTree->GetItemParent(m_item);
		if (!parent)
			return false;

		m_item = parent;
		return true;
	}

protected:
	wxTreeCtrl* m_pTree;
	wxTreeItemId m_item;

	std::list<wxString> m_lastSelection;
	int m_wrong_sel_depth;

	std::list<bool> m_expand;
};

bool CSiteManager::Load()
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;

	pTree->DeleteAllItems();

	// We have to synchronize access to sitemanager.xml so that multiple processed don't write
	// to the same file or one is reading while the other one writes.
	CInterProcessMutex mutex(MUTEX_SITEMANAGER);

	// Load default sites
	bool hasDefaultSites = LoadDefaultSites();
	if (hasDefaultSites)
		m_ownSites = pTree->AppendItem(pTree->GetRootItem(), _("My Sites"), 0, 0);
	else
		m_ownSites = pTree->AddRoot(_("My Sites"), 0, 0);

	wxTreeItemId treeId = m_ownSites;
	pTree->SetItemImage(treeId, 1, wxTreeItemIcon_Expanded);
	pTree->SetItemImage(treeId, 1, wxTreeItemIcon_SelectedExpanded);

	CXmlFile file(_T("sitemanager"));
	TiXmlElement* pDocument = file.Load();
	if (!pDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nAny changes made in the Site Manager will not be saved."), file.GetFileName().GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

		return false;
	}

	TiXmlElement* pElement = pDocument->FirstChildElement("Servers");
	if (!pElement)
		return true;

	wxString lastSelection = COptions::Get()->GetOption(OPTION_SITEMANAGER_LASTSELECTED);
	if (lastSelection[0] == '0')
	{
		if (lastSelection == _T("0"))
			pTree->SelectItem(treeId);
		else
			lastSelection = lastSelection.Mid(1);
	}
	else
		lastSelection = _T("");
	CSiteManagerXmlHandler_Tree handler(pTree, treeId, lastSelection);

	bool res = Load(pElement, &handler);

	pTree->SortChildren(treeId);
	pTree->Expand(treeId);
	if (!pTree->GetSelection())
		pTree->SelectItem(treeId);

	pTree->EnsureVisible(pTree->GetSelection());

	return res;
}

bool CSiteManager::Load(TiXmlElement *pElement, CSiteManagerXmlHandler* pHandler)
{
	wxASSERT(pElement);
	wxASSERT(pHandler);

	for (TiXmlElement* pChild = pElement->FirstChildElement(); pChild; pChild = pChild->NextSiblingElement())
	{
		TiXmlNode* pNode = pChild->FirstChild();
		while (pNode && !pNode->ToText())
			pNode = pNode->NextSibling();

		if (!pNode)
			continue;

		wxString name = ConvLocal(pNode->ToText()->Value());
		name.Trim(true);
		name.Trim(false);

		if (!strcmp(pChild->Value(), "Folder"))
		{
			const bool expand = GetTextAttribute(pChild, "expanded") != _T("0");
			if (!pHandler->AddFolder(name, expand))
				return false;
			Load(pChild, pHandler);
			if (!pHandler->LevelUp())
				return false;
		}
		else if (!strcmp(pChild->Value(), "Server"))
		{
			CSiteManagerItemData* data = ReadServerElement(pChild);

			if (data)
				pHandler->AddSite(name, data);
		}
	}

	return true;
}

CSiteManagerItemData* CSiteManager::ReadServerElement(TiXmlElement *pElement)
{
	CServer server;
	if (!::GetServer(pElement, server))
		return 0;

	CSiteManagerItemData* data = new CSiteManagerItemData(server);

	TiXmlHandle handle(pElement);

	TiXmlText* comments = handle.FirstChildElement("Comments").FirstChild().Text();
	if (comments)
		data->m_comments = ConvLocal(comments->Value());

	TiXmlText* localDir = handle.FirstChildElement("LocalDir").FirstChild().Text();
	if (localDir)
		data->m_localDir = ConvLocal(localDir->Value());

	TiXmlText* remoteDir = handle.FirstChildElement("RemoteDir").FirstChild().Text();
	if (remoteDir)
		data->m_remoteDir.SetSafePath(ConvLocal(remoteDir->Value()));

	return data;
}

bool CSiteManager::Save(TiXmlElement *pElement /*=0*/, wxTreeItemId treeId /*=wxTreeItemId()*/)
{
	if (!m_pSiteManagerMutex)
		return false;

	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;

	if (!pElement || !treeId)
	{
		// We have to synchronize access to sitemanager.xml so that multiple processed don't write
		// to the same file or one is reading while the other one writes.
		CInterProcessMutex mutex(MUTEX_SITEMANAGER);

		wxFileName file(wxGetApp().GetSettingsDir(), _T("sitemanager.xml"));
		TiXmlElement* pDocument = GetXmlFile(file);
		if (!pDocument)
		{
			wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nAny changes made in the Site Manager could not be saved."), file.GetFullPath().c_str());
			wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

			return false;
		}

		TiXmlElement *pServers = pDocument->FirstChildElement("Servers");
		while (pServers)
		{
			pDocument->RemoveChild(pServers);
			pServers = pDocument->FirstChildElement("Servers");
		}
		pElement = pDocument->InsertEndChild(TiXmlElement("Servers"))->ToElement();

		if (!pElement)
		{
			delete pDocument->GetDocument();

			return true;
		}

		bool res = Save(pElement, m_ownSites);

		wxString error;
		if (!SaveXmlFile(file, pDocument, &error))
		{
			wxString msg = wxString::Format(_("Could not write \"%s\", any changes to the Site Manager could not be saved: %s"), file.GetFullPath().c_str(), error.c_str());
			wxMessageBox(msg, _("Error writing xml file"), wxICON_ERROR);
		}

		delete pDocument->GetDocument();
		return res;
	}

	wxTreeItemId child;
	wxTreeItemIdValue cookie;
	child = pTree->GetFirstChild(treeId, cookie);
	while (child.IsOk())
	{
		wxString name = pTree->GetItemText(child);
		char* utf8 = ConvUTF8(name);

		CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(child));
		if (!data)
		{
			TiXmlNode* pNode = pElement->InsertEndChild(TiXmlElement("Folder"));
			const bool expanded = pTree->IsExpanded(child);
			SetTextAttribute(pNode->ToElement(), "expanded", expanded ? _T("1") : _T("0"));

			pNode->InsertEndChild(TiXmlText(utf8));
			delete [] utf8;

			Save(pNode->ToElement(), child);
		}
		else
		{
			TiXmlNode* pNode = pElement->InsertEndChild(TiXmlElement("Server"));
			SetServer(pNode->ToElement(), data->m_server);

			TiXmlNode* sub;

			// Save comments
			sub = pNode->InsertEndChild(TiXmlElement("Comments"));
			char* comments = ConvUTF8(data->m_comments);
			sub->InsertEndChild(TiXmlText(comments));
			delete [] comments;

			// Save local dir
			sub = pNode->InsertEndChild(TiXmlElement("LocalDir"));
			char* localDir = ConvUTF8(data->m_localDir);
			sub->InsertEndChild(TiXmlText(localDir));
			delete [] localDir;

			// Save remote dir
			sub = pNode->InsertEndChild(TiXmlElement("RemoteDir"));
			char* remoteDir = ConvUTF8(data->m_remoteDir.GetSafePath());
			sub->InsertEndChild(TiXmlText(remoteDir));
			delete [] remoteDir;

			pNode->InsertEndChild(TiXmlText(utf8));
			delete [] utf8;
		}

		child = pTree->GetNextChild(treeId, cookie);
	}

	return false;
}

void CSiteManager::OnNewFolder(wxCommandEvent&event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	if (pTree->GetItemData(item))
	{
		item = pTree->GetItemParent(item);
		wxASSERT(!pTree->GetItemData(item));
	}

	if (!Verify())
		return;

	wxString newName = _("New folder");
	int index = 2;
	while (true)
	{
		wxTreeItemId child;
		wxTreeItemIdValue cookie;
		child = pTree->GetFirstChild(item, cookie);
		bool found = false;
		while (child.IsOk())
		{
			wxString name = pTree->GetItemText(child);
			int cmp = name.CmpNoCase(newName);
			if (!cmp)
			{
				found = true;
				break;
			}

			child = pTree->GetNextChild(item, cookie);
		}
		if (!found)
			break;

		newName = _("New folder") + wxString::Format(_T(" %d"), index++);
	}

	wxTreeItemId newItem = pTree->AppendItem(item, newName, 0, 0);
	pTree->SetItemImage(newItem, 1, wxTreeItemIcon_Expanded);
	pTree->SetItemImage(newItem, 1, wxTreeItemIcon_SelectedExpanded);
	pTree->SortChildren(item);
	pTree->SelectItem(newItem);
	pTree->Expand(item);
	pTree->EditLabel(newItem);
}

static enum ServerType GetServerTypeFromName(const wxString& name)
{
	for (int i = 0; i < SERVERTYPE_MAX; i++)
	{
		enum ServerType type = (enum ServerType)i;
		if (name == CServer::GetNameFromServerType(type))
			return type;
	}

	return DEFAULT;
}

static enum LogonType GetLogonTypeFromName(const wxString& name)
{
	if (name == _("Normal"))
		return NORMAL;
	else if (name == _("Ask for password"))
		return ASK;
	else if (name == _("Interactive"))
		return INTERACTIVE;
	else if (name == _("Account"))
		return ACCOUNT;
	else
		return ANONYMOUS;
}

bool CSiteManager::Verify()
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return true;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return true;

	wxTreeItemData* data = pTree->GetItemData(item);
	if (!data)
		return true;

	const wxString& host = XRCCTRL(*this, "ID_HOST", wxTextCtrl)->GetValue();
	if (host == _T(""))
	{
		XRCCTRL(*this, "ID_HOST", wxTextCtrl)->SetFocus();
		wxMessageBox(_("You have to enter a hostname."));
		return false;
	}

	enum LogonType logon_type = GetLogonTypeFromName(XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->GetStringSelection());

	wxString protocolName = XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->GetStringSelection();
	enum ServerProtocol protocol = CServer::GetProtocolFromName(protocolName);
	if (protocol == SFTP &&
		logon_type == ACCOUNT)
	{
		XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetFocus();
		wxMessageBox(_("'Account' logontype not supported by selected protocol"));
		return false;
	}

	CServer server;

	// Set selected type
	server.SetLogonType(logon_type);

	if (protocol != UNKNOWN)
		server.SetProtocol(protocol);

	unsigned long port;
	XRCCTRL(*this, "ID_PORT", wxTextCtrl)->GetValue().ToULong(&port);
	CServerPath path;
	wxString error;
	if (!server.ParseUrl(host, port, _T(""), _T(""), error, path))
	{
		XRCCTRL(*this, "ID_HOST", wxTextCtrl)->SetFocus();
		wxMessageBox(error);
		return false;
	}

	XRCCTRL(*this, "ID_HOST", wxTextCtrl)->SetValue(server.FormatHost(true));
	XRCCTRL(*this, "ID_PORT", wxTextCtrl)->SetValue(wxString::Format(_T("%d"), server.GetPort()));

	protocolName = CServer::GetProtocolName(server.GetProtocol());
	if (protocolName == _T(""))
		CServer::GetProtocolName(FTP);
	XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->SetStringSelection(protocolName);

	if (XRCCTRL(*this, "ID_CHARSET_CUSTOM", wxRadioButton)->GetValue())
	{
		if (XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->GetValue() == _T(""))
		{
			XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->SetFocus();
			wxMessageBox(_("Need to specify a character encoding"));
			return false;
		}
	}

	// Require username for non-anonymous, non-ask logon type
	const wxString user = XRCCTRL(*this, "ID_USER", wxTextCtrl)->GetValue();
	if (logon_type != ANONYMOUS &&
		logon_type != ASK &&
		logon_type != INTERACTIVE &&
		user == _T(""))
	{
		XRCCTRL(*this, "ID_USER", wxTextCtrl)->SetFocus();
		wxMessageBox(_("You have to specify a user name"));
		return false;
	}

	// The way TinyXML handles blanks, we can't use username of only spaces
	if (user != _T(""))
	{
		bool space_only = true;
		for (unsigned int i = 0; i < user.Len(); i++)
		{
			if (user[i] != ' ')
			{
				space_only = false;
				break;
			}
		}
		if (space_only)
		{
			XRCCTRL(*this, "ID_USER", wxTextCtrl)->SetFocus();
			wxMessageBox(_("Username cannot be a series of spaces"));
			return false;
		}

	}

	// Require account for account logon type
	if (logon_type == ACCOUNT &&
		XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->GetValue() == _T(""))
	{
		XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->SetFocus();
		wxMessageBox(_("You have to enter an account name"));
		return false;
	}

	const wxString remotePathRaw = XRCCTRL(*this, "ID_REMOTEDIR", wxTextCtrl)->GetValue();
	if (remotePathRaw != _T(""))
	{
		CServerPath remotePath;
		const wxString serverType = XRCCTRL(*this, "ID_SERVERTYPE", wxChoice)->GetStringSelection();
		remotePath.SetType(GetServerTypeFromName(serverType));
		if (!remotePath.SetPath(remotePathRaw))
		{
			XRCCTRL(*this, "ID_REMOTEDIR", wxTextCtrl)->SetFocus();
			wxMessageBox(_("Default remote path cannot be parsed. Make sure it is valid and is supported by the selected servertype."));
			return false;
		}
	}


	return true;
}

void CSiteManager::OnBeginLabelEdit(wxTreeEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
	{
		event.Veto();
		return;
	}

	if (event.GetItem() != pTree->GetSelection())
	{
		if (!Verify())
		{
			event.Veto();
			return;
		}
	}

	wxTreeItemId item = event.GetItem();
	if (!item.IsOk() || item == pTree->GetRootItem() || item == m_ownSites || IsPredefinedItem(item))
	{
		event.Veto();
		return;
	}
}

void CSiteManager::OnEndLabelEdit(wxTreeEvent& event)
{
	if (event.IsEditCancelled())
		return;

	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
	{
		event.Veto();
		return;
	}

	wxTreeItemId item = event.GetItem();
	if (item != pTree->GetSelection())
	{
		if (!Verify())
		{
			event.Veto();
			return;
		}
	}

	if (!item.IsOk() || item == pTree->GetRootItem() || item == m_ownSites || IsPredefinedItem(item))
	{
		event.Veto();
		return;
	}

	wxString name = event.GetLabel();

	wxTreeItemId parent = pTree->GetItemParent(item);

	wxTreeItemId child;
	wxTreeItemIdValue cookie;
	for (wxTreeItemId child = pTree->GetFirstChild(parent, cookie); child.IsOk(); child = pTree->GetNextChild(parent, cookie))
	{
		if (child == item)
			continue;
		if (!name.CmpNoCase(pTree->GetItemText(child)))
		{
			wxMessageBox(_("Name already exists"), _("Cannot rename entry"), wxICON_EXCLAMATION, this);
			event.Veto();
			return;
		}
	}

	pTree->SortChildren(parent);
}

void CSiteManager::OnRename(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk() || item == pTree->GetRootItem() || item == m_ownSites || IsPredefinedItem(item))
		return;

	pTree->EditLabel(item);
}

void CSiteManager::OnDelete(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk() || item == pTree->GetRootItem() || item == m_ownSites || IsPredefinedItem(item))
		return;

	CConditionalDialog dlg(this, CConditionalDialog::sitemanager_confirmdelete, CConditionalDialog::yesno);
	dlg.SetTitle(_("Delete Site Manager entry"));

	dlg.AddText(_("Do you really want to delete selected entry?"));

	if (!dlg.Run())
		return;

	wxTreeItemId parent = pTree->GetItemParent(item);
	if (pTree->GetChildrenCount(parent) == 1)
		pTree->Collapse(parent);

	pTree->Delete(item);
}

void CSiteManager::OnSelChanging(wxTreeEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	if (!Verify())
		event.Veto();

	UpdateServer();
}

void CSiteManager::OnSelChanged(wxTreeEvent& event)
{
	SetCtrlState();
}

void CSiteManager::OnNewSite(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk() || IsPredefinedItem(item))
		return;

	if (pTree->GetItemData(item))
	{
		item = pTree->GetItemParent(item);
		wxASSERT(!pTree->GetItemData(item));
	}

	if (!Verify())
		return;

	CServer server;
	AddNewSite(item, server);
}

void CSiteManager::OnLogontypeSelChanged(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		return;

	XRCCTRL(*this, "ID_USER", wxTextCtrl)->Enable(event.GetString() != _("Anonymous"));
	XRCCTRL(*this, "ID_PASS", wxTextCtrl)->Enable(event.GetString() == _("Normal") || event.GetString() == _("Account"));
	XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->Enable(event.GetString() == _("Account"));
}

bool CSiteManager::UpdateServer()
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return false;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		return false;

	if (IsPredefinedItem(item))
		return true;

	unsigned long port;
	XRCCTRL(*this, "ID_PORT", wxTextCtrl)->GetValue().ToULong(&port);
	wxString host = XRCCTRL(*this, "ID_HOST", wxTextCtrl)->GetValue();
	// SetHost does not accept URL syntax
	if (host[0] == '[')
	{
		host.RemoveLast();
		host = host.Mid(1);
	}
	data->m_server.SetHost(host, port);

	const wxString& protocolName = XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->GetStringSelection();
	const enum ServerProtocol protocol = CServer::GetProtocolFromName(protocolName);
	if (protocol != UNKNOWN)
		data->m_server.SetProtocol(protocol);
	else
		data->m_server.SetProtocol(FTP);

	enum LogonType logon_type = GetLogonTypeFromName(XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->GetStringSelection());
	data->m_server.SetLogonType(logon_type);

	data->m_server.SetUser(XRCCTRL(*this, "ID_USER", wxTextCtrl)->GetValue(),
						   XRCCTRL(*this, "ID_PASS", wxTextCtrl)->GetValue());
	data->m_server.SetAccount(XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->GetValue());

	data->m_comments = XRCCTRL(*this, "ID_COMMENTS", wxTextCtrl)->GetValue();

	const wxString serverType = XRCCTRL(*this, "ID_SERVERTYPE", wxChoice)->GetStringSelection();
	data->m_server.SetType(GetServerTypeFromName(serverType));

	data->m_localDir = XRCCTRL(*this, "ID_LOCALDIR", wxTextCtrl)->GetValue();
	data->m_remoteDir = CServerPath();
	data->m_remoteDir.SetType(data->m_server.GetType());
	data->m_remoteDir.SetPath(XRCCTRL(*this, "ID_REMOTEDIR", wxTextCtrl)->GetValue());
	int hours, minutes;
	hours = XRCCTRL(*this, "ID_TIMEZONE_HOURS", wxSpinCtrl)->GetValue();
	minutes = XRCCTRL(*this, "ID_TIMEZONE_MINUTES", wxSpinCtrl)->GetValue();

	data->m_server.SetTimezoneOffset(hours * 60 + minutes);

	if (XRCCTRL(*this, "ID_TRANSFERMODE_ACTIVE", wxRadioButton)->GetValue())
		data->m_server.SetPasvMode(MODE_ACTIVE);
	else if (XRCCTRL(*this, "ID_TRANSFERMODE_PASSIVE", wxRadioButton)->GetValue())
		data->m_server.SetPasvMode(MODE_PASSIVE);
	else
		data->m_server.SetPasvMode(MODE_DEFAULT);

	if (XRCCTRL(*this, "ID_LIMITMULTIPLE", wxCheckBox)->GetValue())
	{
		data->m_server.MaximumMultipleConnections(XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->GetValue());
	}
	else
		data->m_server.MaximumMultipleConnections(0);

	if (XRCCTRL(*this, "ID_CHARSET_UTF8", wxRadioButton)->GetValue())
		data->m_server.SetEncodingType(ENCODING_UTF8);
	else if (XRCCTRL(*this, "ID_CHARSET_CUSTOM", wxRadioButton)->GetValue())
	{
		wxString encoding = XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->GetValue();
		data->m_server.SetEncodingType(ENCODING_CUSTOM, encoding);
	}
	else
		data->m_server.SetEncodingType(ENCODING_AUTO);

	if (XRCCTRL(*this, "ID_BYPASSPROXY", wxCheckBox)->GetValue())
		data->m_server.SetBypassProxy(true);
	else
		data->m_server.SetBypassProxy(false);

	data->m_server.SetName(pTree->GetItemText(item));

	return true;
}

bool CSiteManager::GetServer(CSiteManagerItemData& data)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return false;

	CSiteManagerItemData* pData = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!pData)
		return false;

	data = *pData;

	return true;
}

void CSiteManager::OnRemoteDirBrowse(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		return;

	wxDirDialog dlg(this, _("Choose the default local directory"), XRCCTRL(*this, "ID_LOCALDIR", wxTextCtrl)->GetValue(), wxDD_NEW_DIR_BUTTON);
	if (dlg.ShowModal() == wxID_OK)
	{
		XRCCTRL(*this, "ID_LOCALDIR", wxTextCtrl)->SetValue(dlg.GetPath());
	}
}

void CSiteManager::OnItemActivated(wxTreeEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		return;

	wxCommandEvent cmdEvent;
	OnConnect(cmdEvent);
}

void CSiteManager::OnLimitMultipleConnectionsChanged(wxCommandEvent& event)
{
	XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->Enable(event.IsChecked());
}

void CSiteManager::SetCtrlState()
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();

	const bool predefined = IsPredefinedItem(item);

#ifdef __WXGTK__
	wxWindow* pFocus = FindFocus();
#endif

	CSiteManagerItemData* data = 0;
	if (item.IsOk())
		data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
	{
		// Set the control states according if it's possible to use the control
		const bool root_or_predefined = (item == pTree->GetRootItem() || item == m_ownSites || predefined);

		XRCCTRL(*this, "ID_RENAME", wxWindow)->Enable(!root_or_predefined);
		XRCCTRL(*this, "ID_DELETE", wxWindow)->Enable(!root_or_predefined);
		XRCCTRL(*this, "ID_COPY", wxWindow)->Enable(false);
		XRCCTRL(*this, "ID_NOTEBOOK", wxWindow)->Enable(false);
		XRCCTRL(*this, "ID_NEWFOLDER", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_NEWSITE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_CONNECT", wxWindow)->Enable(false);

		// Empty all site information
		XRCCTRL(*this, "ID_HOST", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_PORT", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->SetStringSelection(_("FTP"));
		XRCCTRL(*this, "ID_BYPASSPROXY", wxCheckBox)->SetValue(false);
		XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Anonymous"));
		XRCCTRL(*this, "ID_USER", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_PASS", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_COMMENTS", wxTextCtrl)->SetValue(_T(""));

		XRCCTRL(*this, "ID_SERVERTYPE", wxChoice)->SetSelection(0);
		XRCCTRL(*this, "ID_LOCALDIR", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_REMOTEDIR", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_TIMEZONE_HOURS", wxSpinCtrl)->SetValue(0);
		XRCCTRL(*this, "ID_TIMEZONE_MINUTES", wxSpinCtrl)->SetValue(0);

		XRCCTRL(*this, "ID_TRANSFERMODE_DEFAULT", wxRadioButton)->SetValue(true);
		XRCCTRL(*this, "ID_LIMITMULTIPLE", wxCheckBox)->SetValue(false);
		XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->SetValue(1);

		XRCCTRL(*this, "ID_CHARSET_AUTO", wxRadioButton)->SetValue(true);
		XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->SetValue(_T(""));
#ifdef __WXGTK__
		XRCCTRL(*this, "wxID_OK", wxButton)->SetDefault();
#endif
	}
	else
	{
		// Set the control states according if it's possible to use the control
		XRCCTRL(*this, "ID_RENAME", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_DELETE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_COPY", wxWindow)->Enable(true);
		XRCCTRL(*this, "ID_NOTEBOOK", wxWindow)->Enable(true);
		XRCCTRL(*this, "ID_NEWFOLDER", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_NEWSITE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_CONNECT", wxWindow)->Enable(true);

		XRCCTRL(*this, "ID_HOST", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_HOST", wxTextCtrl)->SetValue(data->m_server.FormatHost(true));
		unsigned int port = data->m_server.GetPort();

		if (port != CServer::GetDefaultPort(data->m_server.GetProtocol()))
			XRCCTRL(*this, "ID_PORT", wxTextCtrl)->SetValue(wxString::Format(_T("%d"), port));
		else
			XRCCTRL(*this, "ID_PORT", wxTextCtrl)->SetValue(_T(""));
		XRCCTRL(*this, "ID_PORT", wxWindow)->Enable(!predefined);

		const wxString& protocolName = CServer::GetProtocolName(data->m_server.GetProtocol());
		if (protocolName != _T(""))
			XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->SetStringSelection(protocolName);
		else
			XRCCTRL(*this, "ID_PROTOCOL", wxChoice)->SetStringSelection(CServer::GetProtocolName(FTP));
		XRCCTRL(*this, "ID_PROTOCOL", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_BYPASSPROXY", wxCheckBox)->SetValue(data->m_server.GetBypassProxy());

		XRCCTRL(*this, "ID_USER", wxTextCtrl)->Enable(!predefined && data->m_server.GetLogonType() != ANONYMOUS);
		XRCCTRL(*this, "ID_PASS", wxTextCtrl)->Enable(!predefined && (data->m_server.GetLogonType() == NORMAL || data->m_server.GetLogonType() == ACCOUNT));
		XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->Enable(!predefined && data->m_server.GetLogonType() == ACCOUNT);

		switch (data->m_server.GetLogonType())
		{
		case NORMAL:
			XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Normal"));
			break;
		case ASK:
			XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Ask for password"));
			break;
		case INTERACTIVE:
			XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Interactive"));
			break;
		case ACCOUNT:
			XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Account"));
			break;
		default:
			XRCCTRL(*this, "ID_LOGONTYPE", wxChoice)->SetStringSelection(_("Anonymous"));
			break;
		}
		XRCCTRL(*this, "ID_LOGONTYPE", wxWindow)->Enable(!predefined);

		XRCCTRL(*this, "ID_USER", wxTextCtrl)->SetValue(data->m_server.GetUser());
		XRCCTRL(*this, "ID_ACCOUNT", wxTextCtrl)->SetValue(data->m_server.GetAccount());
		XRCCTRL(*this, "ID_PASS", wxTextCtrl)->SetValue(data->m_server.GetPass());
		XRCCTRL(*this, "ID_COMMENTS", wxTextCtrl)->SetValue(data->m_comments);
		XRCCTRL(*this, "ID_COMMENTS", wxWindow)->Enable(!predefined);

		XRCCTRL(*this, "ID_SERVERTYPE", wxChoice)->SetSelection(data->m_server.GetType());
		XRCCTRL(*this, "ID_SERVERTYPE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_LOCALDIR", wxTextCtrl)->SetValue(data->m_localDir);
		XRCCTRL(*this, "ID_LOCALDIR", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_REMOTEDIR", wxTextCtrl)->SetValue(data->m_remoteDir.GetPath());
		XRCCTRL(*this, "ID_REMOTEDIR", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_TIMEZONE_HOURS", wxSpinCtrl)->SetValue(data->m_server.GetTimezoneOffset() / 60);
		XRCCTRL(*this, "ID_TIMEZONE_HOURS", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_TIMEZONE_MINUTES", wxSpinCtrl)->SetValue(data->m_server.GetTimezoneOffset() % 60);
		XRCCTRL(*this, "ID_TIMEZONE_MINUTES", wxWindow)->Enable(!predefined);

		enum PasvMode pasvMode = data->m_server.GetPasvMode();
		if (pasvMode == MODE_ACTIVE)
			XRCCTRL(*this, "ID_TRANSFERMODE_ACTIVE", wxRadioButton)->SetValue(true);
		else if (pasvMode == MODE_PASSIVE)
			XRCCTRL(*this, "ID_TRANSFERMODE_PASSIVE", wxRadioButton)->SetValue(true);
		else
			XRCCTRL(*this, "ID_TRANSFERMODE_DEFAULT", wxRadioButton)->SetValue(true);
		XRCCTRL(*this, "ID_TRANSFERMODE_ACTIVE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_TRANSFERMODE_PASSIVE", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_TRANSFERMODE_DEFAULT", wxWindow)->Enable(!predefined);

		int maxMultiple = data->m_server.MaximumMultipleConnections();
		XRCCTRL(*this, "ID_LIMITMULTIPLE", wxCheckBox)->SetValue(maxMultiple != 0);
		XRCCTRL(*this, "ID_LIMITMULTIPLE", wxWindow)->Enable(!predefined);
		if (maxMultiple != 0)
		{
			XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->Enable(!predefined);
			XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->SetValue(maxMultiple);
		}
		else
		{
			XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->Enable(false);
			XRCCTRL(*this, "ID_MAXMULTIPLE", wxSpinCtrl)->SetValue(1);
		}

		switch (data->m_server.GetEncodingType())
		{
		default:
		case ENCODING_AUTO:
			XRCCTRL(*this, "ID_CHARSET_AUTO", wxRadioButton)->SetValue(true);
			break;
		case ENCODING_UTF8:
			XRCCTRL(*this, "ID_CHARSET_UTF8", wxRadioButton)->SetValue(true);
			break;
		case ENCODING_CUSTOM:
			XRCCTRL(*this, "ID_CHARSET_CUSTOM", wxRadioButton)->SetValue(true);
			break;
		}
		XRCCTRL(*this, "ID_CHARSET_AUTO", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_CHARSET_UTF8", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_CHARSET_CUSTOM", wxWindow)->Enable(!predefined);
		XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->Enable(!predefined && data->m_server.GetEncodingType() == ENCODING_CUSTOM);
		XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->SetValue(data->m_server.GetCustomEncoding());
#ifdef __WXGTK__
		XRCCTRL(*this, "ID_CONNECT", wxButton)->SetDefault();
#endif
	}
#ifdef __WXGTK__
	if (pFocus && !pFocus->IsEnabled())
	{
		for (wxWindow* pParent = pFocus->GetParent(); pParent; pParent = pParent->GetParent())
		{
			if (pParent == this)
			{
				XRCCTRL(*this, "wxID_OK", wxButton)->SetFocus();
				break;
			}
		}
	}
#endif
}

void CSiteManager::OnCharsetChange(wxCommandEvent& event)
{
	bool checked = XRCCTRL(*this, "ID_CHARSET_CUSTOM", wxRadioButton)->GetValue();
	XRCCTRL(*this, "ID_ENCODING", wxTextCtrl)->Enable(checked);
}

void CSiteManager::OnProtocolSelChanged(wxCommandEvent& event)
{
}

void CSiteManager::OnCopySite(wxCommandEvent& event)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(item));
	if (!data)
		return;

	if (!Verify())
		return;

	if (!UpdateServer())
		return;

	wxTreeItemId parent;
	if (IsPredefinedItem(item))
		parent = m_ownSites;
	else
		parent = pTree->GetItemParent(item);

	const wxString name = pTree->GetItemText(item);
	wxString newName = wxString::Format(_("Copy of %s"), name.c_str());
	int index = 2;
	while (true)
	{
		wxTreeItemId child;
		wxTreeItemIdValue cookie;
		child = pTree->GetFirstChild(parent, cookie);
		bool found = false;
		while (child.IsOk())
		{
			wxString name = pTree->GetItemText(child);
			int cmp = name.CmpNoCase(newName);
			if (!cmp)
			{
				found = true;
				break;
			}

			child = pTree->GetNextChild(parent, cookie);
		}
		if (!found)
			break;

		newName = wxString::Format(_("Copy (%d) of %s"), index++, name.c_str());
	}

	CSiteManagerItemData* newData = new CSiteManagerItemData();
	*newData = *data;
	wxTreeItemId newItem = pTree->AppendItem(parent, newName, 2, 2, newData);
	pTree->SortChildren(parent);
	pTree->SelectItem(newItem);
	pTree->Expand(parent);
	pTree->EditLabel(newItem);
}

bool CSiteManager::LoadDefaultSites()
{
	const wxString& defaultsDir = wxGetApp().GetDefaultsDir();
	if (defaultsDir == _T(""))
		return false;

	wxFileName name(defaultsDir, _T("fzdefaults.xml"));
	CXmlFile file(name);

	TiXmlElement* pDocument = file.Load();
	if (!pDocument)
		return false;

	TiXmlElement* pElement = pDocument->FirstChildElement("Servers");
	if (!pElement)
		return false;

	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return false;

	int style = pTree->GetWindowStyle();
	pTree->SetWindowStyle(style | wxTR_HIDE_ROOT);
	wxTreeItemId root = pTree->AddRoot(_T(""), 0, 0);

	m_predefinedSites = pTree->AppendItem(root, _("Predefined Sites"), 0, 0);
	pTree->SetItemImage(m_predefinedSites, 1, wxTreeItemIcon_Expanded);
	pTree->SetItemImage(m_predefinedSites, 1, wxTreeItemIcon_SelectedExpanded);

	wxString lastSelection = COptions::Get()->GetOption(OPTION_SITEMANAGER_LASTSELECTED);
	if (lastSelection[0] == '1')
	{
		if (lastSelection == _T("1"))
			pTree->SelectItem(m_predefinedSites);
		else
			lastSelection = lastSelection.Mid(1);
	}
	else
		lastSelection = _T("");
	CSiteManagerXmlHandler_Tree handler(pTree, m_predefinedSites, lastSelection);

	Load(pElement, &handler);

	return true;
}

bool CSiteManager::IsPredefinedItem(wxTreeItemId item)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	wxASSERT(pTree);
	if (!pTree)
		return false;

	while (item)
	{
		if (item == m_predefinedSites)
			return true;
		item = pTree->GetItemParent(item);
	}

	return false;
}

class CSiteManagerXmlHandler_Menu : public CSiteManagerXmlHandler
{
public:
	CSiteManagerXmlHandler_Menu(wxMenu* pMenu, std::map<int, CSiteManagerItemData*> *idMap)
		: m_pMenu(pMenu), m_idMap(idMap)
	{
	}

	unsigned int GetInsertIndex(wxMenu* pMenu, const wxString& name)
	{
		unsigned int i;
		for (i = 0; i < pMenu->GetMenuItemCount(); i++)
		{
			const wxMenuItem* const pItem = pMenu->FindItemByPosition(i);
			if (!pItem)
				continue;
			if (pItem->GetLabel() > name)
				break;
		}

		return i;
	}

	virtual bool AddFolder(const wxString& name, bool)
	{
		m_parents.push_back(m_pMenu);
		m_childNames.push_back(name);

		m_pMenu = new wxMenu;

		return true;
	}

	virtual bool AddSite(const wxString& name, CSiteManagerItemData* data)
	{
		wxString newName = name;
		int i = GetInsertIndex(m_pMenu, newName);
		newName.Replace(_T("&"), _T("&&"));
		wxMenuItem* pItem = m_pMenu->Insert(i, wxID_ANY, newName);
		(*m_idMap)[pItem->GetId()] = data;

		return true;
	}

	// Go up a level
	virtual bool LevelUp()
	{
		if (m_parents.empty() || m_childNames.empty())
			return false;

		wxMenu* pChild = m_pMenu;
		m_pMenu = m_parents.back();
		if (pChild->GetMenuItemCount())
		{
			wxString name = m_childNames.back();
			int i = GetInsertIndex(m_pMenu, name);
			name.Replace(_T("&"), _T("&&"));

			wxMenuItem* pItem = new wxMenuItem(m_pMenu, wxID_ANY, name, _T(""), wxITEM_NORMAL, pChild);
			m_pMenu->Insert(i, pItem);
		}
		else
			delete pChild;
		m_childNames.pop_back();
		m_parents.pop_back();

		return true;
	}

protected:
	wxMenu* m_pMenu;

	std::map<int, CSiteManagerItemData*> *m_idMap;

	std::list<wxMenu*> m_parents;
	std::list<wxString> m_childNames;
};

wxMenu* CSiteManager::GetSitesMenu()
{
	ClearIdMap();

	// We have to synchronize access to sitemanager.xml so that multiple processed don't write
	// to the same file or one is reading while the other one writes.
	CInterProcessMutex mutex(MUTEX_SITEMANAGER);

	wxMenu* predefinedSites = GetSitesMenu_Predefied(m_idMap);

	CXmlFile file(_T("sitemanager"));
	TiXmlElement* pDocument = file.Load();
	if (!pDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nAny changes made in the Site Manager will not be saved."), file.GetFileName().GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

		if (!predefinedSites)
			return predefinedSites;

		wxMenu *pMenu = new wxMenu;
		wxMenuItem* pItem = pMenu->Append(wxID_ANY, _("No sites available"));
		pItem->Enable(false);
		return pMenu;
	}

	TiXmlElement* pElement = pDocument->FirstChildElement("Servers");
	if (!pElement)
	{
		if (!predefinedSites)
			return predefinedSites;

		wxMenu *pMenu = new wxMenu;
		wxMenuItem* pItem = pMenu->Append(wxID_ANY, _("No sites available"));
		pItem->Enable(false);
		return pMenu;
	}

	wxMenu* pMenu = new wxMenu;
	CSiteManagerXmlHandler_Menu handler(pMenu, &m_idMap);

	bool res = Load(pElement, &handler);
	if (!res || !pMenu->GetMenuItemCount())
	{
		delete pMenu;
		pMenu = 0;
	}

	if (pMenu)
	{
		if (!predefinedSites)
			return pMenu;

		wxMenu* pRootMenu = new wxMenu;
		pRootMenu->AppendSubMenu(predefinedSites, _("Predefined Sites"));
		pRootMenu->AppendSubMenu(pMenu, _("My Sites"));

		return pRootMenu;
	}

	if (predefinedSites)
		return predefinedSites;

	pMenu = new wxMenu;
	wxMenuItem* pItem = pMenu->Append(wxID_ANY, _("No sites available"));
	pItem->Enable(false);
	return pMenu;
}

void CSiteManager::ClearIdMap()
{
	for (std::map<int, CSiteManagerItemData*>::iterator iter = m_idMap.begin(); iter != m_idMap.end(); iter++)
		delete iter->second;

	m_idMap.clear();
}

wxMenu* CSiteManager::GetSitesMenu_Predefied(std::map<int, CSiteManagerItemData*> &idMap)
{
	const wxString& defaultsDir = wxGetApp().GetDefaultsDir();
	if (defaultsDir == _T(""))
		return 0;

	wxFileName name(defaultsDir, _T("fzdefaults.xml"));
	CXmlFile file(name);

	TiXmlElement* pDocument = file.Load();
	if (!pDocument)
		return 0;

	TiXmlElement* pElement = pDocument->FirstChildElement("Servers");
	if (!pElement)
		return 0;

	wxMenu* pMenu = new wxMenu;
	CSiteManagerXmlHandler_Menu handler(pMenu, &idMap);

	if (!Load(pElement, &handler))
	{
		delete pMenu;
		return 0;
	}

	if (!pMenu->GetMenuItemCount())
	{
		delete pMenu;
		return 0;
	}

	return pMenu;
}

CSiteManagerItemData* CSiteManager::GetSiteById(int id)
{
	std::map<int, CSiteManagerItemData*>::iterator iter = m_idMap.find(id);

	CSiteManagerItemData *pData;
	if (iter != m_idMap.end())
	{
		pData = iter->second;
		iter->second = 0;
	}
	else
		pData = 0;

	ClearIdMap();

	return pData;
}

void CSiteManager::OnBeginDrag(wxTreeEvent& event)
{
	if (!Verify())
	{
		event.Veto();
		return;
	}

	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
	{
		event.Veto();
		return;
	}

	wxTreeItemId item = event.GetItem();
	if (!item.IsOk())
	{
		event.Veto();
		return;
	}

	const bool predefined = IsPredefinedItem(item);
	const bool root = item == pTree->GetRootItem() || item == m_ownSites;
	if (root)
	{
		event.Veto();
		return;
	}

	CSiteManagerDataObject obj;

	wxDropSource source(this);
	source.SetData(obj);

	m_dropSource = item;

	source.DoDragDrop(predefined ? wxDrag_CopyOnly : wxDrag_DefaultMove);

	m_dropSource = wxTreeItemId();
}

struct itempair
{
	wxTreeItemId source;
	wxTreeItemId target;
};

bool CSiteManager::MoveItems(wxTreeItemId source, wxTreeItemId target, bool copy)
{
	if (source == target)
		return false;

	if (IsPredefinedItem(target))
		return false;

	if (IsPredefinedItem(source) && !copy)
		return false;

    wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (pTree->GetItemData(target))
		return false;

	wxTreeItemId item = target;
	while (item != pTree->GetRootItem())
	{
		if (item == source)
			return false;
		item = pTree->GetItemParent(item);
	}

	if (!copy && pTree->GetItemParent(source) == target)
		return false;

	wxString sourceName = pTree->GetItemText(source);

	wxTreeItemId child;
	wxTreeItemIdValue cookie;
	child = pTree->GetFirstChild(target, cookie);

	while (child.IsOk())
	{
		wxString childName = pTree->GetItemText(child);

		if (!sourceName.CmpNoCase(childName))
		{
			wxMessageBox(_("An item with the same name as the dragged item already exists at the target location."), _("Failed to copy or move sites"), wxICON_INFORMATION);
			return false;
		}

		child = pTree->GetNextChild(target, cookie);
	}

	std::list<itempair> work;
	itempair pair;
	pair.source = source;
	pair.target = target;
	work.push_back(pair);

	std::list<wxTreeItemId> expand;

	while (!work.empty())
	{
		itempair pair = work.front();
		work.pop_front();

		wxString name = pTree->GetItemText(pair.source);

		CSiteManagerItemData* data = reinterpret_cast<CSiteManagerItemData* >(pTree->GetItemData(pair.source));

		wxTreeItemId newItem = pTree->AppendItem(pair.target, name, data ? 2 : 0);
		if (!data)
		{
			pTree->SetItemImage(newItem, 1, wxTreeItemIcon_Expanded);
			pTree->SetItemImage(newItem, 1, wxTreeItemIcon_SelectedExpanded);

			if (pTree->IsExpanded(pair.source))
				expand.push_back(newItem);
		}
		else
		{
			CSiteManagerItemData* newData = new CSiteManagerItemData;
			*newData = *data;
			pTree->SetItemData(newItem, newData);
		}

		wxTreeItemId child;
		wxTreeItemIdValue cookie;
		child = pTree->GetFirstChild(pair.source, cookie);
		while (child.IsOk())
		{
			itempair newPair;
			newPair.source = child;
			newPair.target = newItem;
			work.push_back(newPair);

			child = pTree->GetNextChild(pair.source, cookie);
		}

		pTree->SortChildren(pair.target);
	}

	if (!copy)
	{
		wxTreeItemId parent = pTree->GetItemParent(source);
		if (pTree->GetChildrenCount(parent) == 1)
			pTree->Collapse(parent);

		pTree->Delete(source);
	}

	for (std::list<wxTreeItemId>::iterator iter = expand.begin(); iter != expand.end(); iter++)
		pTree->Expand(*iter);

	pTree->Expand(target);

	return true;
}

void CSiteManager::OnChar(wxKeyEvent& event)
{
	if (event.GetKeyCode() != WXK_F2)
	{
		event.Skip();
		return;
	}

	wxCommandEvent cmdEvent;
	OnRename(cmdEvent);
}

void CSiteManager::CopyAddServer(const CServer& server)
{
	if (!Verify())
		return;

	AddNewSite(m_ownSites, server);
}

void CSiteManager::AddNewSite(wxTreeItemId parent, const CServer& server)
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxString newName = _("New site");
	int index = 2;
	while (true)
	{
		wxTreeItemId child;
		wxTreeItemIdValue cookie;
		child = pTree->GetFirstChild(parent, cookie);
		bool found = false;
		while (child.IsOk())
		{
			wxString name = pTree->GetItemText(child);
			int cmp = name.CmpNoCase(newName);
			if (!cmp)
			{
				found = true;
				break;
			}

			child = pTree->GetNextChild(parent, cookie);
		}
		if (!found)
			break;

		newName = _("New site") + wxString::Format(_T(" %d"), index++);
	}

	wxTreeItemId newItem = pTree->AppendItem(parent, newName, 2, 2, new CSiteManagerItemData(server));
	pTree->SortChildren(m_ownSites);
	pTree->SelectItem(newItem);
	pTree->Expand(m_ownSites);
	pTree->EditLabel(newItem);
}

void CSiteManager::RememberLastSelected()
{
	wxTreeCtrl *pTree = XRCCTRL(*this, "ID_SITETREE", wxTreeCtrl);
	if (!pTree)
		return;

	wxTreeItemId item = pTree->GetSelection();
	if (!item.IsOk())
		return;

	wxString path;
	while (item != pTree->GetRootItem() && item != m_ownSites && item != m_predefinedSites)
	{
		wxString name = pTree->GetItemText(item);
		name.Replace(_T("\\"), _T("\\\\"));
		name.Replace(_T("/"), _T("\\/"));

		path = name + _T("/") + path;

		item = pTree->GetItemParent(item);
	}

	if (item == m_predefinedSites)
		path = _T("1") + path;
	else
		path = _T("0") + path;

	COptions::Get()->SetOption(OPTION_SITEMANAGER_LASTSELECTED, path);
}

bool CSiteManager::UnescapeSitePath(wxString path, std::list<wxString>& result)
{
	result.clear();

	wxString name;
	const wxChar *p = path;

	// Undo escapement
	bool lastBackslash = false;
	while (*p)
	{
		const wxChar& c = *p;
		if (c == '\\')
		{
			if (lastBackslash)
			{
				name += _T("\\");
				lastBackslash = false;
			}
			else
				lastBackslash = true;
		}
		else if (c == '/')
		{
			if (lastBackslash)
			{
				name += _T("/");
				lastBackslash = 0;
			}
			else
			{
				if (!name.IsEmpty())
					result.push_back(name);
				name.clear();
			}
		}
		else
			name += *p;
		p++;
	}
	if (lastBackslash)
		return false;
	if (name != _T(""))
		result.push_back(name);

	return !result.empty();
}

class CSiteManagerXmlHandler_ByPath : public CSiteManagerXmlHandler
{
public:
	CSiteManagerXmlHandler_ByPath(const wxString& lastSelection)
	{
		m_theItemData = 0;
		if (!CSiteManager::UnescapeSitePath(lastSelection, m_lastSelection))
			m_lastSelection.clear();
		m_wrong_sel_depth = 0;
	}

	virtual ~CSiteManagerXmlHandler_ByPath()
	{
	}

	virtual bool AddFolder(const wxString& name, bool expanded)
	{
		if (m_theItemData)
			return false;

		if (!m_wrong_sel_depth && !m_lastSelection.empty())
		{
			const wxString& first = m_lastSelection.front();
			if (first == name)
				m_lastSelection.pop_front();
			else
				m_wrong_sel_depth++;
		}
		else
			m_wrong_sel_depth++;

		return true;
	}

	virtual bool AddSite(const wxString& name, CSiteManagerItemData* data)
	{
		if (m_theItemData)
		{
			delete data;
			return false;
		}

		if (m_wrong_sel_depth || m_lastSelection.empty())
		{
			delete data;
			return true;
		}

		const wxString& first = m_lastSelection.front();
		if (first != name)
		{
			delete data;
			return true;
		}

		m_lastSelection.clear();
		m_theItemData = data;

		return true;
	}

	virtual bool LevelUp()
	{
		if (m_wrong_sel_depth)
			m_wrong_sel_depth--;

		return m_theItemData == 0;
	}

	// Our result
	CSiteManagerItemData* m_theItemData;

protected:

	std::list<wxString> m_lastSelection;
	int m_wrong_sel_depth;
};

CSiteManagerItemData* CSiteManager::GetSiteByPath(wxString sitePath)
{
	wxChar c = sitePath[0];
	if (c != '0' && c != '1')
	{
		wxMessageBox(_("Site path has to begin with 0 or 1."), _("Invalid site path"));
		return 0;
	}

	sitePath = sitePath.Mid(1);

	// We have to synchronize access to sitemanager.xml so that multiple processed don't write
	// to the same file or one is reading while the other one writes.
	CInterProcessMutex mutex(MUTEX_SITEMANAGER);

	CXmlFile file;
	TiXmlElement* pDocument = 0;

	if (c == '0')
		pDocument = file.Load(_T("sitemanager"));
	else
	{
		const wxString& defaultsDir = wxGetApp().GetDefaultsDir();
		if (defaultsDir == _T(""))
		{
			wxMessageBox(_("Site does not exist."), _("Invalid site path"));
			return 0;
		}
		wxFileName name(defaultsDir, _T("fzdefaults.xml"));
		pDocument = file.Load(name);
	}

	if (!pDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", please make sure the file is valid and can be accessed.\nAny changes made in the Site Manager will not be saved."), file.GetFileName().GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);

		return 0;
	}

	TiXmlElement* pElement = pDocument->FirstChildElement("Servers");
	if (!pElement)
	{
		wxMessageBox(_("Site does not exist."), _("Invalid site path"));
		return 0;
	}


	CSiteManagerXmlHandler_ByPath handler(sitePath);

	Load(pElement, &handler);

	if (!handler.m_theItemData)
	{
		wxMessageBox(_("Site does not exist."), _("Invalid site path"));
		return 0;
	}

	return handler.m_theItemData;
}
