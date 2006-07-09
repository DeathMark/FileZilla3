#ifndef __CONTROLSOCKET_H__
#define __CONTROLSOCKET_H__

class COpData
{
public:
	COpData(enum Command op_Id);
	virtual ~COpData();

	int opState;
	const enum Command opId;

	bool waitForAsyncRequest;

	COpData *pNextOpData;
};

class CFileTransferOpData : public COpData
{
public:
	CFileTransferOpData();
	virtual ~CFileTransferOpData();
	// Transfer data
	wxString localFile, remoteFile;
	CServerPath remotePath;
	bool download;

	wxDateTime fileTime;
	wxFileOffset localFileSize;
	wxFileOffset remoteFileSize;

	bool tryAbsolutePath;
	bool resume;

	// Set to true when sending the command which
	// starts the actual transfer
	bool transferInitiated;
};

class CMkdirOpData : public COpData
{
public:
	CMkdirOpData()
		: COpData(cmd_mkdir)
	{
	}

	virtual ~CMkdirOpData()
	{
	}

	CServerPath path;
	CServerPath currentPath;
	CServerPath commonParent;
	std::list<wxString> segments;
};

#include "logging_private.h"

class CTransferStatus;
class CControlSocket : public wxEvtHandler, public wxSocketClient, public CLogging
{
public:
	CControlSocket(CFileZillaEnginePrivate *pEngine);
	virtual ~CControlSocket();

	virtual int Connect(const CServer &server);
	virtual int ContinueConnect(const wxIPV4address *address);
	virtual int Disconnect();
	virtual void Cancel();
	virtual int List(CServerPath path = CServerPath(), wxString subDir = _T(""), bool refresh = false) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int FileTransfer(const wxString localFile, const CServerPath &remotePath,
							 const wxString &remoteFile, bool download,
							 const CFileTransferCommand::t_transferSettings& transferSettings) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int RawCommand(const wxString& command = _T("")) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int Delete(const CServerPath& path = CServerPath(), const wxString& file = _T("")) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int RemoveDir(const CServerPath& path = CServerPath(), const wxString& subDir = _T("")) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int Mkdir(const CServerPath& path) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int Rename(const CRenameCommand& command) { return FZ_REPLY_NOTSUPPORTED; }
	virtual int Chmod(const CChmodCommand& command) { return FZ_REPLY_NOTSUPPORTED; }
	virtual bool Connected() const { return IsConnected(); }

	// If m_pCurrentOpData is zero, this function returns the current command
	// from the engine.
	enum Command GetCurrentCommandId() const;

	virtual void TransferEnd(int reason) { }

	virtual bool SetAsyncRequestReply(CAsyncRequestNotification *pNotification) = 0;

	void InitTransferStatus(wxFileOffset totalSize, wxFileOffset startOffset);
	void SetTransferStatusStartTime();
	void UpdateTransferStatus(wxFileOffset transferredBytes);
	void ResetTransferStatus();
	bool GetTransferStatus(CTransferStatus &status, bool &changed);

	const CServer* GetCurrentServer() const;

	// Conversion function which convert between local and server charset.
	wxString ConvToLocal(const char* buffer);
	wxChar* ConvToLocalBuffer(const char* buffer);
	wxChar* ConvToLocalBuffer(const char* buffer, wxMBConv& conv);
	wxCharBuffer ConvToServer(const wxString& str);

	// ---
	// The following two functions control the timeout behaviour:
	// ---
    
	// Call this if data could be sent or retrieved
	void SetAlive();
	
	// Set to true if waiting for data
	void SetWait(bool waiting);


protected:
	virtual int DoClose(int nErrorCode = FZ_REPLY_DISCONNECTED);
	void ResetSocket();

	virtual void OnSocketEvent(wxSocketEvent &event);
	virtual void OnConnect(wxSocketEvent &event);
	virtual void OnReceive(wxSocketEvent &event);
	virtual void OnSend(wxSocketEvent &event);
	virtual void OnClose(wxSocketEvent &event);
	virtual int ResetOperation(int nErrorCode);
	virtual bool Send(const char *buffer, int len);

	// Called by ResetOperation if there's a queued operation
	virtual int SendNextCommand(int prevResult = FZ_REPLY_OK) = 0;

	// Send the directory listing to the interface.
	// If the listing is a freshly received one, it should be stored
	// in the cache before calling SendDirectoryListing, else
	// list update logic will fail.
	void SendDirectoryListing(CDirectoryListing* pListing);

	// Easier functions to get the IP addresses
	virtual wxString GetLocalIP() const;
	virtual wxString GetPeerIP() const;

	bool IsIpAddress(const wxString& address);

	wxString ConvertDomainName(wxString domain);

	int CheckOverwriteFile();

	bool ParsePwdReply(wxString reply, bool unquoted = false);

	COpData *m_pCurOpData;
	int m_nOpState;
	CFileZillaEnginePrivate *m_pEngine;
	CServer *m_pCurrentServer;

	CServerPath m_CurrentPath;
	
	char *m_pSendBuffer;
	int m_nSendBufferLen;

	CTransferStatus *m_pTransferStatus;
	int m_transferStatusSendState;
	
	bool m_onConnectCalled;

	wxCSConv *m_pCSConv;
	bool m_useUTF8;

	// Timeout data
	wxTimer m_timer;
	wxStopWatch m_stopWatch;

	// Initialized with 0, incremented each time
	// a new connection is created from the same instance.
	// Mainly needed for CHttpControlSockets if server sends a redirects.
	// Otherwise, lingering events from the previous connection will cause
	// problems
	int m_socketId;

	DECLARE_EVENT_TABLE();
	void OnTimer(wxTimerEvent& event);
};

#endif
