#ifndef __FTPCONTROLSOCKET_H__
#define __FTPCONTROLSOCKET_H__

#include "logging_private.h"
#include "ControlSocket.h"
#include "externalipresolver.h"
#include "rtt.h"

#include <regex>

#define RECVBUFFERSIZE 4096
#define MAXLINELEN 2000

class CTransferSocket;
class CFtpTransferOpData;
class CRawTransferOpData;
class CTlsSocket;

class CFtpControlSocket final : public CRealControlSocket
{
	friend class CTransferSocket;
public:
	CFtpControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CFtpControlSocket();
	virtual void TransferEnd();

	virtual bool SetAsyncRequestReply(CAsyncRequestNotification *pNotification);

protected:

	virtual int ResetOperation(int nErrorCode);

	virtual int Connect(const CServer &server);
	virtual int List(CServerPath path = CServerPath(), std::wstring const& subDir = std::wstring(), int flags = 0);
	int ListParseResponse();
	int ListSubcommandResult(int prevResult);
	int ListSend();
	int ListCheckTimezoneDetection(CDirectoryListing& listing);

	int ChangeDir(CServerPath path = CServerPath(), wxString subDir = _T(""), bool link_discovery = false);
	int ChangeDirParseResponse();
	int ChangeDirSubcommandResult(int prevResult);
	int ChangeDirSend();

	virtual int FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
							 std::wstring const& remoteFile, bool download,
							 CFileTransferCommand::t_transferSettings const& transferSettings);
	int FileTransferParseResponse();
	int FileTransferSubcommandResult(int prevResult);
	int FileTransferSend();
	int FileTransferTestResumeCapability();

	virtual int RawCommand(std::wstring const& command);
	int RawCommandSend();
	int RawCommandParseResponse();

	virtual int Delete(const CServerPath& path, std::deque<std::wstring>&& files);
	int DeleteSubcommandResult(int prevResult);
	int DeleteSend();
	int DeleteParseResponse();

	virtual int RemoveDir(CServerPath const& path, std::wstring const& subDir);
	int RemoveDirSubcommandResult(int prevResult);
	int RemoveDirSend();
	int RemoveDirParseResponse();

	virtual int Mkdir(const CServerPath& path);
	virtual int MkdirParseResponse();
	virtual int MkdirSend();

	virtual int Rename(const CRenameCommand& command);
	virtual int RenameParseResponse();
	virtual int RenameSubcommandResult(int prevResult);
	virtual int RenameSend();

	virtual int Chmod(const CChmodCommand& command);
	virtual int ChmodParseResponse();
	virtual int ChmodSubcommandResult(int prevResult);
	virtual int ChmodSend();

	virtual int Transfer(const wxString& cmd, CFtpTransferOpData* oldData);
	virtual int TransferParseResponse();
	virtual int TransferSend();

	virtual void OnConnect();
	virtual void OnReceive();

	bool SendCommand(wxString const& str, bool maskArgs = false, bool measureRTT = true);

	// Parse the latest reply line from the server
	void ParseLine(wxString line);

	// Parse the actual response and delegate it to the handlers.
	// It's the last line in a multi-line response.
	void ParseResponse();

	void ParseFeat(wxString line);

	virtual int SendNextCommand();
	virtual int ParseSubcommandResult(int prevResult);

	int GetReplyCode() const;

	int Logon();
	int LogonParseResponse();
	int LogonSend();

	wxString GetPassiveCommand(CRawTransferOpData& data);
	bool ParsePasvResponse(CRawTransferOpData* pData);
	bool ParseEpsvResponse(CRawTransferOpData* pData);

	// Some servers are broken. Instead of an empty listing, some MVS servers
	// for example they return "550 no members found"
	// Other servers return "550 No files found."
	bool IsMisleadingListResponse() const;

	int GetExternalIPAddress(std::string& address);

	// Checks if listing2 is a subset of listing1. Compares only filenames.
	bool CheckInclusion(const CDirectoryListing& listing1, const CDirectoryListing& listing2);

	void StartKeepaliveTimer();

	bool GetLoginSequence(const CServer& server);

	wxString m_Response;
	wxString m_MultilineResponseCode;
	std::vector<wxString> m_MultilineResponseLines;

	CTransferSocket *m_pTransferSocket;

	// Some servers keep track of the offset specified by REST between sessions
	// So we always sent a REST 0 for a normal transfer following a restarted one
	bool m_sentRestartOffset;

	char m_receiveBuffer[RECVBUFFERSIZE];
	int m_bufferLen;
	int m_repliesToSkip; // Set to the amount of pending replies if cancelling an action

	int m_pendingReplies;

	CExternalIPResolver* m_pIPResolver;

	CTlsSocket* m_pTlsSocket;
	bool m_protectDataChannel;

	int m_lastTypeBinary;

	// Used by keepalive code so that we're not using keep alive
	// till the end of time. Stop after a couple of minutes.
	fz::monotonic_clock m_lastCommandCompletionTime;

	fz::timer_id m_idleTimer{};

	CLatencyMeasurement m_rtt;

	virtual void operator()(fz::event_base const& ev);

	void OnExternalIPAddress();
	void OnTimer(fz::timer_id id);

	std::unique_ptr<std::wregex> m_pasvReplyRegex; // Have it as class member to avoid recompiling the regex on each transfer or listing
};

class CIOThread;

class CFtpTransferOpData
{
public:
	CFtpTransferOpData() = default;
	virtual ~CFtpTransferOpData() {}

	TransferEndReason transferEndReason{TransferEndReason::successful};
	bool tranferCommandSent{};

	int64_t resumeOffset{};
	bool binary{true};
};

class CFtpFileTransferOpData final : public CFileTransferOpData, public CFtpTransferOpData
{
public:
	CFtpFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, CServerPath const& remote_path);
	virtual ~CFtpFileTransferOpData();

	CIOThread *pIOThread{};
	bool fileDidExist{true};
};

class CRawTransferOpData final : public COpData
{
public:
	CRawTransferOpData();
	std::wstring cmd;

	CFtpTransferOpData* pOldData{};

	bool bPasv{true};
	bool bTriedPasv{};
	bool bTriedActive{};

	wxString host;
	int port{};
};

#endif
