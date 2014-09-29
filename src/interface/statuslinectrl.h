#ifndef __STATUSLINECTRL_H__
#define __STATUSLINECTRL_H__

class CQueueView;
class CStatusLineCtrl final : public wxWindow
{
public:
	CStatusLineCtrl(CQueueView* pParent, const t_EngineData* const pEngineData, const wxRect& initialPosition);
	~CStatusLineCtrl();

	const CFileItem* GetItem() const { return m_pEngineData->pItem; }

	void SetEngineData(const t_EngineData* const pEngineData);

	void SetTransferStatus(const CTransferStatus* pStatus);
	wxLongLong GetLastOffset() const { return status_valid_ ? status_.currentOffset : m_lastOffset; }
	wxLongLong GetTotalSize() const { return status_valid_ ? status_.totalSize : -1; }
	wxFileOffset GetSpeed(int elapsedSeconds);
	wxFileOffset GetCurrentSpeed();

	virtual bool Show(bool show = true);

protected:
	void InitFieldOffsets();

	void DrawRightAlignedText(wxDC& dc, wxString text, int x, int y);
	void DrawProgressBar(wxDC& dc, int x, int y, int height, int bar_split, int permill);

	CQueueView* m_pParent;
	const t_EngineData* m_pEngineData;
	CTransferStatus status_;
	bool status_valid_{};

	wxString m_statusText;
	wxTimer m_transferStatusTimer;

	static int m_fieldOffsets[4];
	static wxCoord m_textHeight;
	static bool m_initialized;

	bool m_madeProgress;

	wxLongLong m_lastOffset{-1}; // Stores the last transfer offset so that the total queue size can be accurately calculated.

	// This is used by GetSpeed to forget about the first 10 seconds on longer transfers
	// since at the very start the speed is hardly accurate (e.g. due to TCP slow start)
	struct _past_data final
	{
		int elapsed{};
		wxFileOffset offset{};
	} m_past_data[10];
	int m_past_data_index;

	//Used by getCurrentSpeed
	wxDateTime m_gcLastTimeStamp;
	wxFileOffset m_gcLastOffset{-1};
	wxFileOffset m_gcLastSpeed{-1};

	//Used to avoid excessive redraws
	wxBitmap m_data;
	std::unique_ptr<wxMemoryDC> m_mdc;
	wxString m_previousStatusText;
	int m_last_elapsed_seconds{};
	int m_last_left{};
	wxString m_last_bytes_and_rate;
	int m_last_bar_split{-1};
	int m_last_permill{-1};

	DECLARE_EVENT_TABLE()
	void OnPaint(wxPaintEvent& event);
	void OnTimer(wxTimerEvent& event);
	void OnEraseBackground(wxEraseEvent& event);
};

#endif // __STATUSLINECTRL_H__
