#ifndef __QUICKCONNECTBAR_H__
#define __QUICKCONNECTBAR_H__

class CState;
class CQuickconnectBar : public wxPanel
{
public:
	CQuickconnectBar();
	virtual~CQuickconnectBar();

	bool Create(wxWindow* pParent, CState* pState);

	void ClearFields();

protected:
	// Only valid while menu is being displayed
	std::list<CServer> m_recentServers;

	CState* m_pState;

	DECLARE_EVENT_TABLE();
	void OnQuickconnect(wxCommandEvent& event);
	void OnQuickconnectDropdown(wxCommandEvent& event);
	void OnMenu(wxCommandEvent& event);
	void OnKeyboardNavigation(wxNavigationKeyEvent& event);

	wxTextCtrl* m_pHost;
	wxTextCtrl* m_pUser;
	wxTextCtrl* m_pPass;
	wxTextCtrl* m_pPort;
};


#endif //__QUICKCONNECTBAR_H__
