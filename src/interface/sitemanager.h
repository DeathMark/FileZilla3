#ifndef __SITEMANAGER_H__
#define __SITEMANAGER_H__

class CSiteManagerItemData : public wxTreeItemData
{
public:
	CSiteManagerItemData(CServer server = CServer())
	{
		m_server = server;
	}

	virtual ~CSiteManagerItemData()
	{
	}

	CServer m_server;
	wxString m_comments;
	wxString m_localDir;
	CServerPath m_remoteDir;
};

#include "dialogex.h"

class TiXmlElement;
class CInterProcessMutex;
class CSiteManagerXmlHandler;
class CWindowStateManager;
class CSiteManagerDropTarget;
class CSiteManager: public wxDialogEx
{
	friend class CSiteManagerDropTarget;

	DECLARE_EVENT_TABLE();

public:
	/// Constructors
	CSiteManager();
	virtual ~CSiteManager();

	// Creation. If pServer is set, it will cause a new item to be created.
	bool Create(wxWindow* parent, const CServer* pServer = 0);

	bool GetServer(CSiteManagerItemData& data);

	static wxMenu* GetSitesMenu();
	static void ClearIdMap();

	// This function also clears the Id map
	static CSiteManagerItemData* GetSiteById(int id);
	static CSiteManagerItemData* GetSiteByPath(wxString sitePath);

	static bool UnescapeSitePath(wxString path, std::list<wxString>& result);

protected:
	// Creates the controls and sizers
	void CreateControls(wxWindow* parent);

	bool Verify();
	bool UpdateServer();
	bool Load();
	static bool Load(TiXmlElement *pElement, CSiteManagerXmlHandler* pHandler);
	bool Save(TiXmlElement *pElement = 0, wxTreeItemId treeId = wxTreeItemId());
	bool SaveChild(TiXmlElement *pElement, wxTreeItemId child);
	void SetCtrlState();
	bool LoadDefaultSites();

	// The map maps event id's to sites
	static wxMenu* GetSitesMenu_Predefied(std::map<int, CSiteManagerItemData*> &idMap);

	bool IsPredefinedItem(wxTreeItemId item);

	static CSiteManagerItemData* ReadServerElement(TiXmlElement *pElement);

	void AddNewSite(wxTreeItemId parent, const CServer& server);
	void CopyAddServer(const CServer& server);

	void RememberLastSelected();

	virtual void OnOK(wxCommandEvent& event);
	virtual void OnCancel(wxCommandEvent& event);
	virtual void OnConnect(wxCommandEvent& event);
	virtual void OnNewSite(wxCommandEvent& event);
	virtual void OnNewFolder(wxCommandEvent& event);
	virtual void OnRename(wxCommandEvent& event);
	virtual void OnDelete(wxCommandEvent& event);
	virtual void OnBeginLabelEdit(wxTreeEvent& event);
	virtual void OnEndLabelEdit(wxTreeEvent& event);
	virtual void OnSelChanging(wxTreeEvent& event);
	virtual void OnSelChanged(wxTreeEvent& event);
	virtual void OnLogontypeSelChanged(wxCommandEvent& event);
	virtual void OnRemoteDirBrowse(wxCommandEvent& event);
	virtual void OnItemActivated(wxTreeEvent& event);
	virtual void OnLimitMultipleConnectionsChanged(wxCommandEvent& event);
	virtual void OnCharsetChange(wxCommandEvent& event);
	virtual void OnProtocolSelChanged(wxCommandEvent& event);
	virtual void OnBeginDrag(wxTreeEvent& event);
	virtual void OnChar(wxKeyEvent& event);
	void OnCopySite(wxCommandEvent& event);
	void OnContextMenu(wxTreeEvent& event);
	void OnExportSelected(wxCommandEvent& event);

	CInterProcessMutex* m_pSiteManagerMutex;

	wxTreeItemId m_predefinedSites;
	wxTreeItemId m_ownSites;

	wxTreeItemId m_dropSource;

	wxTreeItemId m_contextMenuItem;

	bool MoveItems(wxTreeItemId source, wxTreeItemId target, bool copy);

	// Initialized by GetSitesMenu
	static std::map<int, CSiteManagerItemData*> m_idMap;

	CWindowStateManager* m_pWindowStateManager;
};

#endif //__SITEMANAGER_H__
