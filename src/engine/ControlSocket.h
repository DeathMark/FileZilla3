#ifndef __CONTROLSOCKET_H__
#define __CONTROLSOCKET_H__

class COpData
{
public:
	COpData();
	virtual ~COpData();

	int opState;
	enum Command opId;

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
	virtual int List(CServerPath path = CServerPath(), wxString subDir = _T(""), bool refresh = false) = 0;
	virtual int FileTransfer(const wxString localFile, const CServerPath &remotePath,
							 const wxString &remoteFile, bool download,
							 const CFileTransferCommand::t_transferSettings& transferSettings) = 0;
	virtual int RawCommand(const wxString& command = _T("")) = 0;
	virtual int Delete(const CServerPath& path = CServerPath(), const wxString& file = _T("")) = 0;
	virtual int RemoveDir(const CServerPath& path = CServerPath(), const wxString& subDir = _T("")) = 0;
	virtual int Mkdir(const CServerPath& path, CServerPath start = CServerPath()) = 0;
	virtual int Rename(const CRenameCommand& command) = 0;
	virtual int Chmod(const CChmodCommand& command) = 0;
	virtual bool Connected() const { return IsConnected(); }

	enum Command GetCurrentCommandId() const;

	virtual void TransferEnd(int reason) = 0;

	virtual bool SetAsyncRequestReply(CAsyncRequestNotification *pNotification) = 0;

	void InitTransferStatus(wxFileOffset totalSize, wxFileOffset startOffset);
	void UpdateTransferStatus(wxFileOffset transferredBytes);
	void ResetTransferStatus();
	bool GetTransferStatus(CTransferStatus &status, bool &changed);

	const CServer* GetCurrentServer() const;

	// Conversion function which convert between local and server charset.
	wxString ConvToLocal(const char* buffer);
	wxChar* ConvToLocalBuffer(const char* buffer);
	wxChar* ConvToLocalBuffer(const char* buffer, wxMBConv& conv);
	wxCharBuffer ConvToServer(const wxString& str);

protected:
	virtual int DoClose(int nErrorCode = FZ_REPLY_DISCONNECTED);

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

	DECLARE_EVENT_TABLE();
};

#endif
