#ifndef __RECURSIVE_OPERATION_H__
#define __RECURSIVE_OPERATION_H__

#include "state.h"
#include <set>
#include "filter.h"

class CChmodDialog;
class CQueueView;

class CRecursiveOperation : public CStateEventHandler
{
public:
	CRecursiveOperation(CState* pState);
	~CRecursiveOperation();

	enum OperationMode
	{
		recursive_none,
		recursive_download,
		recursive_addtoqueue,
		recursive_delete,
		recursive_chmod,
		recursive_list
	};

	void StartRecursiveOperation(enum OperationMode mode, const CServerPath& startDir, const std::list<CFilter> &filters, bool allowParent = false, const CServerPath& finalDir = CServerPath());
	void StopRecursiveOperation();

	void AddDirectoryToVisit(const CServerPath& path, const wxString& subdir, const wxString& localDir = _T(""), bool is_link = false);
	void AddDirectoryToVisitRestricted(const CServerPath& path, const wxString& restrict, bool recurse);

	enum OperationMode GetOperationMode() const { return m_operationMode; }

	// Needed for recursive_chmod
	void SetChmodDialog(CChmodDialog* pChmodDialog);

	void ListingFailed(int error);
	void LinkIsNotDir();

	void SetQueue(CQueueView* pQueue);

	bool ChangeOperationMode(enum OperationMode mode);
	
protected:
	// Processes the directory listing in case of a recursive operation
	void ProcessDirectoryListing(const CDirectoryListing* pDirectoryListing);
	bool NextOperation();

	virtual void OnStateChange(enum t_statechange_notifications notification, const wxString& data);

	enum OperationMode m_operationMode;

	CState* m_pState;

	class CNewDir
	{
	public:
		CNewDir();
		CServerPath parent;
		wxString subdir;
		wxString localDir;
		bool doVisit;

		bool recurse;
		wxString restrict;

		bool second_try;
		bool link;
	};

	CServerPath m_startDir;
	CServerPath m_finalDir;
	std::set<CServerPath> m_visitedDirs;
	std::list<CNewDir> m_dirsToVisit;

	bool m_allowParent;

	// Needed for recursive_chmod
	CChmodDialog* m_pChmodDlg;

	CQueueView* m_pQueue;

	std::list<CFilter> m_filters;
};

#endif //__RECURSIVE_OPERATION_H__
