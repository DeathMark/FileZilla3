#ifndef __SEARCH_H__
#define __SEARCH_H__

#include "filter_conditions_dialog.h"
#include "state.h"
#include <set>

class CWindowStateManager;
class CSearchDialogFileList;
class CQueueView;
class CSearchDialog : protected CFilterConditionsDialog, public CStateEventHandler
{
	friend class CSearchDialogFileList;
public:
	CSearchDialog(wxWindow* parent, CState* pState, CQueueView* pQueue);
	virtual ~CSearchDialog();

	bool Load();
	void Run();

protected:
	void ProcessDirectoryListing();

	void SetCtrlState();

	wxWindow* m_parent;
	CSearchDialogFileList *m_results;
	CQueueView* m_pQueue;

	virtual void OnStateChange(enum t_statechange_notifications notification, const wxString& data);

	CWindowStateManager* m_pWindowStateManager;

	CFilter m_search_filter;

	bool m_searching;

	CServerPath m_original_dir;

	DECLARE_EVENT_TABLE()
	void OnSearch(wxCommandEvent& event);
	void OnStop(wxCommandEvent& event);
	void OnContextMenu(wxContextMenuEvent& event);
	void OnDownload(wxCommandEvent& event);

	std::set<CServerPath> m_visited;

	CServerPath m_search_root;
};

#endif //__SEARCH_H__
