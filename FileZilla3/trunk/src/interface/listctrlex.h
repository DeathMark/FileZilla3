#ifndef __LISTCTRLEX_H__
#define __LISTCTRLEX_H__

class wxListCtrlEx : public wxListCtrl
{
public:
	wxListCtrlEx(wxWindow *parent,
		wxWindowID id = wxID_ANY,
		const wxPoint& pos = wxDefaultPosition,
		const wxSize& size = wxDefaultSize,
		long style = wxLC_ICON,
		const wxValidator& validator = wxDefaultValidator,
		const wxString& name = wxListCtrlNameStr);
	~wxListCtrlEx();

	// Ensure that the given item is the first in the list
	void ScrollTopItem(int item);

	void EnablePrefixSearch(bool enable) { m_prefixSearch_enabled = enable; }

	// Reducing item count does not reset the focused item
 	// if using the generic list control. Work around it.
	void SaveSetItemCount(long count);

	void ShowColumnEditor();

	void ShowColumn(unsigned int col, bool show);

	// Moves column. Target position includes both hidden
	// as well as shown columns
	void MoveColumn(unsigned int col, unsigned int before);
	
	// Do not call after calling LoadColumnSettings
	void AddColumn(const wxString& name, int align, int initialWidth, bool fixed = false);
	
	// LoadColumnSettings needs to be called exactly once after adding
	// all columns
	void LoadColumnSettings(int widthsOptionId, int visibilityOptionId, int sortOptionId);
	void SaveColumnSettings(int widthsOptionId, int visibilityOptionId, int sortOptionId);

	int GetColumnVisibleIndex(int col);

	// Refresh list but not header
	void RefreshListOnly(bool eraseBackground = true);

#ifndef __WXMSW__
	wxScrolledWindow* GetMainWindow();
#endif
protected:
	virtual void OnPostScroll();
	virtual void OnPreEmitPostScrollEvent();
	void EmitPostScrollEvent();

	virtual wxString GetItemText(int item, unsigned int column) { return _T(""); }

	virtual wxString OnGetItemText(long item, long column) const;
	void ResetSearchPrefix();

	// Argument is visible column index
	int GetHeaderIconIndex(int col);
	void SetHeaderIconIndex(int col, int icon);

private:
	// Keyboard prefix search
	void HandlePrefixSearch(wxChar character);
	int FindItemWithPrefix(const wxString& searchPrefix, int start);

	DECLARE_EVENT_TABLE();
	void OnPostScrollEvent(wxCommandEvent& event);
	void OnScrollEvent(wxScrollWinEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnSelectionChanged(wxListEvent& event);
	void OnKeyDown(wxKeyEvent& event);

	bool m_prefixSearch_enabled;
	wxDateTime m_prefixSearch_lastKeyPress;
	wxString m_prefixSearch_prefix;

	bool ReadColumnWidths(unsigned int optionId);
	void SaveColumnWidths(unsigned int optionId);

	void CreateVisibleColumnMapping();

	struct t_columnInfo
	{
		wxString name;
		int align;
		int width;
		bool shown;
		unsigned int order;
		bool fixed;
	};
	std::vector<t_columnInfo> m_columnInfo;
	unsigned int *m_pVisibleColumnMapping;
};

#endif //__LISTCTRLEX_H__
