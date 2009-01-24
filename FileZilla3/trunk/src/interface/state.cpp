#include "FileZilla.h"
#include "state.h"
#include "commandqueue.h"
#include "FileZillaEngine.h"
#include "Options.h"
#include "Mainfrm.h"
#include "queue.h"
#include "filezillaapp.h"
#include "RemoteListView.h"
#include "recursive_operation.h"
#include "statusbar.h"
#include "local_filesys.h"

CState::CState(CMainFrame* pMainFrame)
{
	m_pMainFrame = pMainFrame;

	m_pDirectoryListing = 0;
	m_pServer = 0;
	m_successful_connect = 0;

	m_pEngine = 0;
	m_pCommandQueue = 0;

	m_pRecursiveOperation = new CRecursiveOperation(this);
}

CState::~CState()
{
	delete m_pDirectoryListing;
	delete m_pServer;

	delete m_pCommandQueue;
	delete m_pEngine;

	// Unregister all handlers
	for (int i = 0; i < STATECHANGE_MAX; i++)
	{
		for (std::list<CStateEventHandler*>::iterator iter = m_handlers[i].begin(); iter != m_handlers[i].end(); iter++)
			(*iter)->m_pState = 0;
	}

	delete m_pRecursiveOperation;
}

CLocalPath CState::GetLocalDir() const
{
	return m_localDir;
}

bool CState::SetLocalDir(const wxString& dir, wxString *error /*=0*/)
{
	CLocalPath p(m_localDir);
	if (!p.ChangePath(dir))
		return false;

	if (!p.Exists(error))
		return false;

	m_localDir = p.GetPath();

	COptions::Get()->SetOption(OPTION_LASTLOCALDIR, m_localDir.GetPath());

	NotifyHandlers(STATECHANGE_LOCAL_DIR);

	return true;
}

bool CState::SetRemoteDir(const CDirectoryListing *pDirectoryListing, bool modified /*=false*/)
{
	if (!pDirectoryListing)
	{
		if (modified)
			return false;

		if (m_pDirectoryListing)
		{
			const CDirectoryListing* pOldListing = m_pDirectoryListing;
			m_pDirectoryListing = 0;
			NotifyHandlers(STATECHANGE_REMOTE_DIR);
			delete pOldListing;
		}
		return true;
	}

	wxASSERT(pDirectoryListing->m_firstListTime.IsValid());

	if (modified)
	{
		if (!m_pDirectoryListing || m_pDirectoryListing->path != pDirectoryListing->path)
		{
			// We aren't interested in these listings
			delete pDirectoryListing;
			return true;
		}
	}
	else
		COptions::Get()->SetOption(OPTION_LASTSERVERPATH, pDirectoryListing->path.GetSafePath());

	if (m_pDirectoryListing && m_pDirectoryListing->path == pDirectoryListing->path &&
        pDirectoryListing->m_failed)
	{
		// We still got an old listing, no need to display the new one
		delete pDirectoryListing;
		return true;
	}

	const CDirectoryListing *pOldListing = m_pDirectoryListing;
	m_pDirectoryListing = pDirectoryListing;

	if (!modified)
		NotifyHandlers(STATECHANGE_REMOTE_DIR);
	else
		NotifyHandlers(STATECHANGE_REMOTE_DIR_MODIFIED);

	delete pOldListing;

	return true;
}

const CDirectoryListing *CState::GetRemoteDir() const
{
	return m_pDirectoryListing;
}

const CServerPath CState::GetRemotePath() const
{
	if (!m_pDirectoryListing)
		return CServerPath();

	return m_pDirectoryListing->path;
}

void CState::RefreshLocal()
{
	NotifyHandlers(STATECHANGE_LOCAL_DIR);
}

void CState::RefreshLocalFile(wxString file)
{
	wxString file_name;
	CLocalPath path(file, &file_name);
	if (path.empty())
		return;

	if (file_name.empty())
	{
		if (!path.HasParent())
			return;
		path.MakeParent(&file_name);
		wxASSERT(!file_name.empty());
	}
	if (path != m_localDir)
		return;

	NotifyHandlers(STATECHANGE_LOCAL_REFRESH_FILE, file_name);
}

void CState::SetServer(const CServer* server)
{
	if (m_pServer)
	{
		if (server &&  *server == *m_pServer)
		{
			// Nothing changes
			return;
		}

		SetRemoteDir(0);
		delete m_pServer;
	}

	m_successful_connect = false;

	CStatusBar* const pStatusBar = m_pMainFrame->GetStatusBar();
	if (pStatusBar)
	{
		pStatusBar->DisplayDataType(server);
		pStatusBar->DisplayEncrypted(server);
	}

	if (server)
		m_pServer = new CServer(*server);
	else
	{
		if (m_pServer)
			m_pMainFrame->SetTitle(_T("FileZilla"));
		m_pServer = 0;
	}

	NotifyHandlers(STATECHANGE_SERVER);
}

const CServer* CState::GetServer() const
{
	return m_pServer;
}

bool CState::Connect(const CServer& server, bool askBreak, const CServerPath& path /*=CServerPath()*/)
{
	if (!m_pEngine)
		return false;
	if (m_pEngine->IsConnected() || m_pEngine->IsBusy() || !m_pCommandQueue->Idle())
	{
		if (askBreak)
			if (wxMessageBox(_("Break current connection?"), _T("FileZilla"), wxYES_NO | wxICON_QUESTION) != wxYES)
				return false;
		m_pCommandQueue->Cancel();
	}

	m_pCommandQueue->ProcessCommand(new CConnectCommand(server));
	m_pCommandQueue->ProcessCommand(new CListCommand(path, _T(""),LIST_FLAG_FALLBACK_CURRENT));

	COptions::Get()->SetLastServer(server);
	COptions::Get()->SetOption(OPTION_LASTSERVERPATH, path.GetSafePath());

	const wxString& name = server.GetName();
	if (!name.IsEmpty())
		m_pMainFrame->SetTitle(name + _T(" - ") + server.FormatServer() + _T(" - FileZilla"));
	else
		m_pMainFrame->SetTitle(server.FormatServer() + _T(" - FileZilla"));

	return true;
}

bool CState::CreateEngine()
{
	wxASSERT(!m_pEngine);
	if (m_pEngine)
		return true;

	m_pEngine = new CFileZillaEngine();
	m_pEngine->Init(m_pMainFrame, COptions::Get());

	m_pCommandQueue = new CCommandQueue(m_pEngine, m_pMainFrame);

	return true;
}

void CState::DestroyEngine()
{
	delete m_pCommandQueue;
	m_pCommandQueue = 0;
	delete m_pEngine;
	m_pEngine = 0;
}

void CState::RegisterHandler(CStateEventHandler* pHandler, enum t_statechange_notifications notification)
{
	wxASSERT(pHandler);
	wxASSERT(notification != STATECHANGE_MAX && notification != STATECHANGE_NONE);

	std::list<CStateEventHandler*> &handlers = m_handlers[notification];
	std::list<CStateEventHandler*>::const_iterator iter;
	for (iter = handlers.begin(); iter != handlers.end(); iter++)
	{
		if (*iter == pHandler)
			return;
	}

	handlers.push_back(pHandler);
}

void CState::UnregisterHandler(CStateEventHandler* pHandler, enum t_statechange_notifications notification)
{
	wxASSERT(pHandler);
	wxASSERT(notification != STATECHANGE_MAX);

	if (notification == STATECHANGE_NONE)
	{
		for (int i = 0; i < STATECHANGE_MAX; i++)
		{
			std::list<CStateEventHandler*> &handlers = m_handlers[i];
			for (std::list<CStateEventHandler*>::iterator iter = handlers.begin(); iter != handlers.end(); iter++)
			{
				if (*iter == pHandler)
				{
					handlers.erase(iter);
					break;
				}
			}
		}
	}
	else
	{
		std::list<CStateEventHandler*> &handlers = m_handlers[notification];
		for (std::list<CStateEventHandler*>::iterator iter = handlers.begin(); iter != handlers.end(); iter++)
		{
			if (*iter == pHandler)
			{
				handlers.erase(iter);
				return;
			}
		}
	}
}

void CState::NotifyHandlers(enum t_statechange_notifications notification, const wxString& data /*=_T("")*/)
{
	wxASSERT(notification != STATECHANGE_NONE && notification != STATECHANGE_MAX);

	const std::list<CStateEventHandler*> &handlers = m_handlers[notification];
	for (std::list<CStateEventHandler*>::const_iterator iter = handlers.begin(); iter != handlers.end(); iter++)
	{
		(*iter)->OnStateChange(notification, data);
	}
}

CStateEventHandler::CStateEventHandler(CState* pState)
{
	wxASSERT(pState);

	if (!pState)
		return;

	m_pState = pState;
}

CStateEventHandler::~CStateEventHandler()
{
	if (!m_pState)
		return;
	m_pState->UnregisterHandler(this, STATECHANGE_NONE);
}

void CState::UploadDroppedFiles(const wxFileDataObject* pFileDataObject, const wxString& subdir, bool queueOnly)
{
	if (!m_pServer || !m_pDirectoryListing)
		return;

	CServerPath path = m_pDirectoryListing->path;
	if (subdir == _T("..") && path.HasParent())
		path = path.GetParent();
	else if (subdir != _T(""))
		path.AddSegment(subdir);

	UploadDroppedFiles(pFileDataObject, path, queueOnly);
}

void CState::UploadDroppedFiles(const wxFileDataObject* pFileDataObject, const CServerPath& path, bool queueOnly)
{
	if (!m_pServer)
		return;

	const wxArrayString& files = pFileDataObject->GetFilenames();

	for (unsigned int i = 0; i < files.Count(); i++)
	{
		if (wxFile::Exists(files[i]))
		{
			const wxFileName name(files[i]);
			const wxLongLong size = name.GetSize().GetValue();
			m_pMainFrame->GetQueue()->QueueFile(queueOnly, false, files[i], name.GetFullName(), path, *m_pServer, size);
			m_pMainFrame->GetQueue()->QueueFile_Finish(!queueOnly);
		}
		else if (wxDir::Exists(files[i]))
		{
			wxString dir = files[i];
			if (dir.Last() == CLocalPath::path_separator && dir.Len() > 1)
				dir.RemoveLast();
			int pos = dir.Find(CLocalPath::path_separator, true);
			if (pos != -1 && pos != (int)dir.Len() - 1)
			{
				wxString lastSegment = dir.Mid(pos + 1);
				CServerPath target = path;
				target.AddSegment(lastSegment);
				m_pMainFrame->GetQueue()->QueueFolder(queueOnly, false, dir, target, *m_pServer);
			}
		}
	}
}

void CState::HandleDroppedFiles(const wxFileDataObject* pFileDataObject, const CLocalPath& path, bool copy)
{
	const wxArrayString &files = pFileDataObject->GetFilenames();
	if (!files.Count())
		return;

#ifdef __WXMSW__
	int len = 1;

	for (unsigned int i = 0; i < files.Count(); i++)
		len += files[i].Len() + 1;

	// SHFILEOPSTRUCT's pTo and pFrom accept null-terminated lists
	// of null-terminated filenames.
	wxChar* from = new wxChar[len];
	wxChar* p = from;
	for (unsigned int i = 0; i < files.Count(); i++)
	{
		wxStrcpy(p, files[i]);
		p += files[i].Len() + 1;
	}
	*p = 0; // End of list

	wxChar* to = new wxChar[path.GetPath().Len() + 2];
	wxStrcpy(to, path.GetPath());
	to[path.GetPath().Len() + 1] = 0; // End of list

	SHFILEOPSTRUCT op = {0};
	op.pFrom = from;
	op.pTo = to;
	op.wFunc = copy ? FO_COPY : FO_MOVE;
	op.hwnd = (HWND)m_pMainFrame->GetHandle();
	SHFileOperation(&op);

	delete [] to;
	delete [] from;
#else
	for (unsigned int i = 0; i < files.Count(); i++)
	{
		const wxString& file = files[i];
		if (wxFile::Exists(file))
		{
			int pos = file.Find(CLocalPath::path_separator, true);
			if (pos == -1 || pos == (int)file.Len() - 1)
				continue;
			const wxString& name = file.Mid(pos + 1);
			if (copy)
				wxCopyFile(file, path.GetPath() + name);
			else
				wxRenameFile(file, path.GetPath() + name);
		}
		else if (wxDir::Exists(file))
		{
			if (copy)
				RecursiveCopy(file, path.GetPath());
			else
			{
				int pos = file.Find(CLocalPath::path_separator, true);
				if (pos == -1 || pos == (int)file.Len() - 1)
					continue;
				const wxString& name = file.Mid(pos + 1);
				wxRenameFile(file, path.GetPath() + name);
			}
		}
	}
#endif

	RefreshLocal();
}

bool CState::RecursiveCopy(CLocalPath source, const CLocalPath& target)
{
	if (source.empty() || target.empty())
		return false;

	if (source == target)
		return false;

	if (source.IsParentOf(target))
		return false;

	if (!source.HasParent())
		return false;

	wxString last_segment;
	if (!source.MakeParent(&last_segment))
		return false;

	std::list<wxString> dirsToVisit;
	dirsToVisit.push_back(last_segment + CLocalPath::path_separator);

	// Process any subdirs which still have to be visited
	while (!dirsToVisit.empty())
	{
		wxString dirname = dirsToVisit.front();
		dirsToVisit.pop_front();
		wxMkdir(target.GetPath() + dirname);

		CLocalFileSystem fs;
		if (!fs.BeginFindFiles(source.GetPath() + dirname, false))
			continue;

		bool is_dir, is_link;
		wxString file;
		while (fs.GetNextFile(file, is_link, is_dir, 0, 0, 0))
		{
			if (file == _T(""))
			{
				wxGetApp().DisplayEncodingWarning();
				continue;
			}

			if (is_dir)
			{
				if (is_link)
					continue;

				const wxString subDir = dirname + file + CLocalPath::path_separator;
				dirsToVisit.push_back(subDir);
			}
			else
				wxCopyFile(source.GetPath() + dirname + file, target.GetPath() + dirname + file);
		}
	}

	return true;
}

bool CState::DownloadDroppedFiles(const CRemoteDataObject* pRemoteDataObject, const CLocalPath& path, bool queueOnly /*=false*/)
{
	bool hasDirs = false;
	bool hasFiles = false;
	const std::list<CRemoteDataObject::t_fileInfo>& files = pRemoteDataObject->GetFiles();
	for (std::list<CRemoteDataObject::t_fileInfo>::const_iterator iter = files.begin(); iter != files.end(); iter++)
	{
		if (iter->dir)
			hasDirs = true;
		else
			hasFiles = true;
	}

	if (hasDirs)
	{
		if (!m_pEngine->IsConnected() || m_pEngine->IsBusy() || !m_pCommandQueue->Idle())
			return false;
	}

	if (hasFiles)
		m_pMainFrame->GetQueue()->QueueFiles(queueOnly, path.GetPath(), *pRemoteDataObject);

	if (!hasDirs)
		return true;

	return m_pMainFrame->GetRemoteListView()->DownloadDroppedFiles(pRemoteDataObject, path, queueOnly);
}

bool CState::IsRemoteConnected() const
{
	if (!m_pEngine)
		return false;

	return m_pServer != 0;
}

bool CState::IsRemoteIdle() const
{
	if (m_pRecursiveOperation->GetOperationMode() != CRecursiveOperation::recursive_none)
		return false;

	if (!m_pCommandQueue)
		return true;

	return m_pCommandQueue->Idle();
}

wxString CState::GetAsURL(const wxString& dir)
{
	// Cheap URL encode
	wxString encoded;
	const wxWX2MBbuf utf8 = dir.mb_str();

	const char* p = utf8;
	while (*p)
	{
		// List of characters that don't need to be escaped taken
		// from the BNF grammar in RFC 1738
		// Again attention seeking Windows wants special treatment...
		const unsigned char c = (unsigned char)*p++;
		if ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '$' ||
			c == '_' ||
			c == '-' ||
			c == '.' ||
			c == '+' ||
			c == '!' ||
			c == '*' ||
#ifndef __WXMSW__
			c == '\'' ||
#endif
			c == '(' ||
			c == ')' ||
			c == ',' ||
			c == '?' ||
			c == ':' ||
			c == '@' ||
			c == '&' ||
			c == '=' ||
			c == '/')
		{
			encoded += (wxChar)c;
		}
#ifdef __WXMSW__
		else if (c == '\\')
			encoded += '/';
#endif
		else
			encoded += wxString::Format(_T("%%%x"), (unsigned int)c);
	}
#ifdef __WXMSW__
	if (encoded.Left(2) == _T("//"))
	{
		// UNC path
		encoded = encoded.Mid(2);
	}
	else
		encoded = _T("/") + encoded;
#endif

	return _T("file://") + encoded;
}
