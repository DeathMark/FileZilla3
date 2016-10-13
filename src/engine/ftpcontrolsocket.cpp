#include <filezilla.h>

#include "directorycache.h"
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "externalipresolver.h"
#include "ftpcontrolsocket.h"
#include "iothread.h"
#include "pathcache.h"
#include "servercapabilities.h"
#include "tlssocket.h"
#include "transfersocket.h"
#include "proxy.h"

#include <libfilezilla/file.hpp>
#include <libfilezilla/iputils.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/util.hpp>

#include <algorithm>

#define LOGON_WELCOME	0
#define LOGON_AUTH_TLS	1
#define LOGON_AUTH_SSL	2
#define LOGON_AUTH_WAIT	3
#define LOGON_LOGON		4
#define LOGON_SYST		5
#define LOGON_FEAT		6
#define LOGON_CLNT		7
#define LOGON_OPTSUTF8	8
#define LOGON_PBSZ		9
#define LOGON_PROT		10
#define LOGON_OPTSMLST	11
#define LOGON_CUSTOMCOMMANDS 12
#define LOGON_DONE		13

CFtpFileTransferOpData::CFtpFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, CServerPath const& remote_path)
	: CFileTransferOpData(is_download, local_file, remote_file, remote_path)
{
}

CFtpFileTransferOpData::~CFtpFileTransferOpData()
{
	if (pIOThread) {
		CIOThread *pThread = pIOThread;
		pIOThread = 0;
		pThread->Destroy();
		delete pThread;
	}
}

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitcwd,
	filetransfer_waitlist,
	filetransfer_size,
	filetransfer_mdtm,
	filetransfer_resumetest,
	filetransfer_transfer,
	filetransfer_waittransfer,
	filetransfer_waitresumetest,
	filetransfer_mfmt
};

enum rawtransferStates
{
	rawtransfer_init = 0,
	rawtransfer_type,
	rawtransfer_port_pasv,
	rawtransfer_rest,
	rawtransfer_transfer,
	rawtransfer_waitfinish,
	rawtransfer_waittransferpre,
	rawtransfer_waittransfer,
	rawtransfer_waitsocket
};

enum class loginCommandType
{
	user,
	pass,
	account,
	other
};

struct t_loginCommand
{
	bool optional;
	bool hide_arguments;
	loginCommandType type;

	wxString command;
};

class CFtpLogonOpData : public CConnectOpData
{
public:
	CFtpLogonOpData()
	{
		waitChallenge = false;
		gotPassword = false;
		waitForAsyncRequest = false;
		gotFirstWelcomeLine = false;
		ftp_proxy_type = 0;

		customCommandIndex = 0;

		for (int i = 0; i < LOGON_DONE; ++i)
			neededCommands[i] = 1;
	}

	virtual ~CFtpLogonOpData()
	{
	}

	std::wstring challenge; // Used for interactive logons
	bool waitChallenge;
	bool waitForAsyncRequest;
	bool gotPassword;
	bool gotFirstWelcomeLine;

	unsigned int customCommandIndex;

	int neededCommands[LOGON_DONE];

	std::deque<t_loginCommand> loginSequence;

	int ftp_proxy_type;
};

class CFtpDeleteOpData final : public COpData
{
public:
	CFtpDeleteOpData()
		: COpData(Command::del)
	{
	}

	virtual ~CFtpDeleteOpData() {}

	CServerPath path;
	std::deque<std::wstring> files;
	bool omitPath{};

	// Set to fz::datetime::Now initially and after
	// sending an updated listing to the UI.
	fz::datetime m_time;

	bool m_needSendListing{};

	// Set to true if deletion of at least one file failed
	bool m_deleteFailed{};
};

class CRawTransferOpData final : public COpData
{
public:
	CRawTransferOpData()
		: COpData(Command::rawtransfer)
	{
	}

	std::wstring cmd;

	CFtpTransferOpData* pOldData{};

	bool bPasv{true};
	bool bTriedPasv{};
	bool bTriedActive{};

	std::wstring host;
	int port{};
};

CFtpControlSocket::CFtpControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	m_pIPResolver = 0;
	m_pTransferSocket = 0;
	m_sentRestartOffset = false;
	m_bufferLen = 0;
	m_repliesToSkip = 0;
	m_pendingReplies = 1;
	m_pTlsSocket = 0;
	m_protectDataChannel = false;
	m_lastTypeBinary = -1;

	// Enable TCP_NODELAY, speeds things up a bit.
	// Enable SO_KEEPALIVE, lots of clueless users have broken routers and
	// firewalls which terminate the control connection on long transfers.
	m_pSocket->SetFlags(CSocket::flag_nodelay | CSocket::flag_keepalive);

	int v = engine_.GetOptions().GetOptionVal(OPTION_TCP_KEEPALIVE_INTERVAL);
	if (v >= 1 && v < 10000) {
		m_pSocket->SetKeepaliveInterval(fz::duration::from_minutes(v));
	}
}

CFtpControlSocket::~CFtpControlSocket()
{
	remove_handler();

	DoClose();
}

void CFtpControlSocket::OnReceive()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::OnReceive()"));

	for (;;) {
		int error;
		int read = m_pBackend->Read(m_receiveBuffer + m_bufferLen, RECVBUFFERSIZE - m_bufferLen, error);

		if (read < 0) {
			if (error != EAGAIN) {
				LogMessage(MessageType::Error, _("Could not read from socket: %s"), CSocket::GetErrorDescription(error));
				if (GetCurrentCommandId() != Command::connect)
					LogMessage(MessageType::Error, _("Disconnected from server"));
				DoClose();
			}
			return;
		}

		if (!read) {
			auto messageType = (GetCurrentCommandId() == Command::none) ? MessageType::Status : MessageType::Error;
			LogMessage(messageType, _("Connection closed by server"));
			DoClose();
			return;
		}

		SetActive(CFileZillaEngine::recv);

		char* start = m_receiveBuffer;
		m_bufferLen += read;

		for (int i = start - m_receiveBuffer; i < m_bufferLen; ++i) {
			char& p = m_receiveBuffer[i];
			if (p == '\r' ||
				p == '\n' ||
				p == 0)
			{
				int len = i - (start - m_receiveBuffer);
				if (!len) {
					++start;
					continue;
				}

				p = 0;
				std::wstring line = ConvToLocal(start, i + 1 - (start - m_receiveBuffer));
				start = m_receiveBuffer + i + 1;

				ParseLine(line);

				// Abort if connection got closed
				if (!m_pCurrentServer) {
					return;
				}
			}
		}
		memmove(m_receiveBuffer, start, m_bufferLen - (start - m_receiveBuffer));
		m_bufferLen -= (start -m_receiveBuffer);
		if (m_bufferLen > MAXLINELEN) {
			m_bufferLen = MAXLINELEN;
		}
	}
}

namespace {
bool HasFeature(wxString const& line, wxString const& feature)
{
	return line == feature || line.StartsWith(feature + _T(" "));
}
}

void CFtpControlSocket::ParseFeat(wxString line)
{
	line.Trim(false);
	wxString up = line.Upper();

	if (HasFeature(up, _T("UTF8")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, utf8_command, yes);
	else if (HasFeature(up, _T("CLNT")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, clnt_command, yes);
	else if (HasFeature(up, _T("MLSD"))) {
		wxString facts;
		// FEAT output for MLST overrides MLSD
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, mlsd_command, &facts) != yes || facts.empty()) {
			facts = line.Mid(5);
		}
		CServerCapabilities::SetCapability(*m_pCurrentServer, mlsd_command, yes, facts);

		// MLST/MLSD specs require use of UTC
		CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, no);
	}
	else if (HasFeature(up, _T("MLST"))) {
		wxString facts = line.Mid(5);
		// FEAT output for MLST overrides MLSD
		if (facts.empty()) {
			if (CServerCapabilities::GetCapability(*m_pCurrentServer, mlsd_command, &facts) != yes) {
				facts.clear();
			}
		}
		CServerCapabilities::SetCapability(*m_pCurrentServer, mlsd_command, yes, facts);

		// MLST/MLSD specs require use of UTC
		CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, no);
	}
	else if (HasFeature(up, _T("MODE Z")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, mode_z_support, yes);
	else if (HasFeature(up, _T("MFMT")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, mfmt_command, yes);
	else if (HasFeature(up, _T("MDTM")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, mdtm_command, yes);
	else if (HasFeature(up, _T("SIZE")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, size_command, yes);
	else if (HasFeature(up, _T("TVFS")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, tvfs_support, yes);
	else if (HasFeature(up, _T("REST STREAM")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, rest_stream, yes);
	else if (HasFeature(up, _T("EPSV")))
		CServerCapabilities::SetCapability(*m_pCurrentServer, epsv_command, yes);
}

void CFtpControlSocket::ParseLine(std::wstring line)
{
	m_rtt.Stop();
	LogMessageRaw(MessageType::Response, line);
	SetAlive();

	if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
		CFtpLogonOpData* pData = static_cast<CFtpLogonOpData *>(m_pCurOpData);
		if (pData->waitChallenge) {
			std::wstring& challenge = pData->challenge;
			if (!challenge.empty())
#ifdef __WXMSW__
				challenge += _T("\r\n");
#else
				challenge += _T("\n");
#endif
			challenge += line;
		}
		else if (pData->opState == LOGON_FEAT) {
			ParseFeat(line);
		}
		else if (pData->opState == LOGON_WELCOME) {
			if (!pData->gotFirstWelcomeLine) {
				if (fz::str_tolower_ascii(line).substr(0, 3) == _T("ssh")) {
					LogMessage(MessageType::Error, _("Cannot establish FTP connection to an SFTP server. Please select proper protocol."));
					DoClose(FZ_REPLY_CRITICALERROR);
					return;
				}
				pData->gotFirstWelcomeLine = true;
			}
		}
	}
	//Check for multi-line responses
	if (line.size() > 3) {
		if (!m_MultilineResponseCode.empty()) {
			if (line.substr(0, 4) == m_MultilineResponseCode) {
				// end of multi-line found
				m_MultilineResponseCode.clear();
				m_Response = line;
				ParseResponse();
				m_Response = _T("");
				m_MultilineResponseLines.clear();
			}
			else {
				m_MultilineResponseLines.push_back(line);
			}
		}
		// start of new multi-line
		else if (line[3] == '-') {
			// DDD<SP> is the end of a multi-line response
			m_MultilineResponseCode = line.substr(0, 3) + _T(" ");
			m_MultilineResponseLines.push_back(line);
		}
		else {
			m_Response = line;
			ParseResponse();
			m_Response = _T("");
		}
	}
}

void CFtpControlSocket::OnConnect()
{
	m_lastTypeBinary = -1;

	SetAlive();

	if (m_pCurrentServer->GetProtocol() == FTPS) {
		if (!m_pTlsSocket) {
			LogMessage(MessageType::Status, _("Connection established, initializing TLS..."));

			wxASSERT(!m_pTlsSocket);
			delete m_pBackend;
			m_pTlsSocket = new CTlsSocket(this, *m_pSocket, this);
			m_pBackend = m_pTlsSocket;

			if (!m_pTlsSocket->Init()) {
				LogMessage(MessageType::Error, _("Failed to initialize TLS."));
				DoClose();
				return;
			}

			int res = m_pTlsSocket->Handshake();
			if (res == FZ_REPLY_ERROR)
				DoClose();

			return;
		}
		else
			LogMessage(MessageType::Status, _("TLS connection established, waiting for welcome message..."));
	}
	else if ((m_pCurrentServer->GetProtocol() == FTPES || m_pCurrentServer->GetProtocol() == FTP) && m_pTlsSocket) {
		LogMessage(MessageType::Status, _("TLS connection established."));
		SendNextCommand();
		return;
	}
	else
		LogMessage(MessageType::Status, _("Connection established, waiting for welcome message..."));
	m_pendingReplies = 1;
	m_repliesToSkip = 0;
	Logon();
}

void CFtpControlSocket::ParseResponse()
{
	if( m_Response.empty() ) {
		LogMessage(MessageType::Debug_Warning, _T("No reply in ParseResponse"));
		return;
	}

	if (m_Response[0] != '1') {
		if (m_pendingReplies > 0)
			m_pendingReplies--;
		else {
			LogMessage(MessageType::Debug_Warning, _T("Unexpected reply, no reply was pending."));
			return;
		}
	}

	if (m_repliesToSkip)
	{
		LogMessage(MessageType::Debug_Info, _T("Skipping reply after cancelled operation or keepalive command."));
		if (m_Response[0] != '1')
			m_repliesToSkip--;

		if (!m_repliesToSkip)
		{
			SetWait(false);
			if (!m_pCurOpData)
				StartKeepaliveTimer();
			else if (!m_pendingReplies)
				SendNextCommand();
		}

		return;
	}

	Command commandId = GetCurrentCommandId();
	switch (commandId)
	{
	case Command::connect:
		LogonParseResponse();
		break;
	case Command::list:
		ListParseResponse();
		break;
	case Command::cwd:
		ChangeDirParseResponse();
		break;
	case Command::transfer:
		FileTransferParseResponse();
		break;
	case Command::raw:
		RawCommandParseResponse();
		break;
	case Command::del:
		DeleteParseResponse();
		break;
	case Command::removedir:
		RemoveDirParseResponse();
		break;
	case Command::mkdir:
		MkdirParseResponse();
		break;
	case Command::rename:
		RenameParseResponse();
		break;
	case Command::chmod:
		ChmodParseResponse();
		break;
	case Command::rawtransfer:
		TransferParseResponse();
		break;
	case Command::none:
		LogMessage(MessageType::Debug_Verbose, _T("Out-of-order reply, ignoring."));
		break;
	default:
		LogMessage(MessageType::Debug_Warning, _T("No action for parsing replies to command %d"), (int)commandId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}
}

bool CFtpControlSocket::GetLoginSequence(const CServer& server)
{
	CFtpLogonOpData *pData = static_cast<CFtpLogonOpData *>(m_pCurOpData);
	pData->loginSequence.clear();

	if (!pData->ftp_proxy_type) {
		// User
		t_loginCommand cmd = {false, false, loginCommandType::user, _T("")};
		pData->loginSequence.push_back(cmd);

		// Password
		cmd.optional = true;
		cmd.hide_arguments = true;
		cmd.type = loginCommandType::pass;
		pData->loginSequence.push_back(cmd);

		// Optional account
		if (!server.GetAccount().empty()) {
			cmd.hide_arguments = false;
			cmd.type = loginCommandType::account;
			pData->loginSequence.push_back(cmd);
		}
	}
	else if (pData->ftp_proxy_type == 1) {
		const wxString& proxyUser = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_USER);
		if (!proxyUser.empty()) {
			// Proxy logon (if credendials are set)
			t_loginCommand cmd = {false, false, loginCommandType::other, _T("USER ") + proxyUser};
			pData->loginSequence.push_back(cmd);
			cmd.optional = true;
			cmd.hide_arguments = true;
			cmd.command = _T("PASS ") + engine_.GetOptions().GetOption(OPTION_FTP_PROXY_PASS);
			pData->loginSequence.push_back(cmd);
		}
		// User@host
		t_loginCommand cmd = {false, false, loginCommandType::user, wxString::Format(_T("USER %s@%s"), server.GetUser(), server.Format(ServerFormat::with_optional_port))};
		pData->loginSequence.push_back(cmd);

		// Password
		cmd.optional = true;
		cmd.hide_arguments = true;
		cmd.type = loginCommandType::pass;
		cmd.command = _T("");
		pData->loginSequence.push_back(cmd);

		// Optional account
		if (!server.GetAccount().empty()) {
			cmd.hide_arguments = false;
			cmd.type = loginCommandType::account;
			pData->loginSequence.push_back(cmd);
		}
	}
	else if (pData->ftp_proxy_type == 2 || pData->ftp_proxy_type == 3) {
		const wxString& proxyUser = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_USER);
		if (!proxyUser.empty()) {
			// Proxy logon (if credendials are set)
			t_loginCommand cmd = {false, false, loginCommandType::other, _T("USER ") + proxyUser};
			pData->loginSequence.push_back(cmd);
			cmd.optional = true;
			cmd.hide_arguments = true;
			cmd.command = _T("PASS ") + engine_.GetOptions().GetOption(OPTION_FTP_PROXY_PASS);
			pData->loginSequence.push_back(cmd);
		}

		// Site or Open
		t_loginCommand cmd = {false, false, loginCommandType::user, _T("")};
		if (pData->ftp_proxy_type == 2)
			cmd.command = _T("SITE ") + server.Format(ServerFormat::with_optional_port);
		else
			cmd.command = _T("OPEN ") + server.Format(ServerFormat::with_optional_port);
		pData->loginSequence.push_back(cmd);

		// User
		cmd.type = loginCommandType::user;
		cmd.command = _T("");
		pData->loginSequence.push_back(cmd);

		// Password
		cmd.optional = true;
		cmd.hide_arguments = true;
		cmd.type = loginCommandType::pass;
		pData->loginSequence.push_back(cmd);

		// Optional account
		if (!server.GetAccount().empty()) {
			cmd.hide_arguments = false;
			cmd.type = loginCommandType::account;
			pData->loginSequence.push_back(cmd);
		}
	}
	else if (pData->ftp_proxy_type == 4) {
		std::wstring proxyUser = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_USER);
		std::wstring proxyPass = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_PASS);
		std::wstring host = server.Format(ServerFormat::with_optional_port);
		std::wstring user = server.GetUser();
		std::wstring account = server.GetAccount();
		fz::replace_substrings(proxyUser, _T("%"), _T("%%"));
		fz::replace_substrings(proxyPass, _T("%"), _T("%%"));
		fz::replace_substrings(host, _T("%"), _T("%%"));
		fz::replace_substrings(user, _T("%"), _T("%%"));
		fz::replace_substrings(account, _T("%"), _T("%%"));

		std::wstring const loginSequence = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_CUSTOMLOGINSEQUENCE);
		std::vector<std::wstring> const tokens = fz::strtok(loginSequence, L"\r\n");

		for (auto token : tokens) {
			bool isHost = false;
			bool isUser = false;
			bool password = false;
			bool isProxyUser = false;
			bool isProxyPass = false;
			if (token.find(_T("%h")) != token.npos) {
				isHost = true;
			}
			if (token.find(_T("%u")) != token.npos) {
				isUser = true;
			}
			if (token.find(_T("%p")) != token.npos) {
				password = true;
			}
			if (token.find(_T("%s")) != token.npos) {
				isProxyUser = true;
			}
			if (token.find(_T("%w")) != token.npos) {
				isProxyPass = true;
			}

			// Skip account if empty
			bool isAccount = false;
			if (token.find(_T("%a")) != token.npos) {
				if (account.empty()) {
					continue;
				}
				else {
					isAccount = true;
				}
			}

			if (isProxyUser && !isHost && !isUser && proxyUser.empty()) {
				continue;
			}
			if (isProxyPass && !isHost && !isUser && proxyUser.empty()) {
				continue;
			}

			fz::replace_substrings(token, _T("%s"), proxyUser);
			fz::replace_substrings(token, _T("%w"), proxyPass);
			fz::replace_substrings(token, _T("%h"), host);
			fz::replace_substrings(token, _T("%u"), user);
			fz::replace_substrings(token, _T("%a"), account);
			// Pass will be replaced before sending to cope with interactve login

			if (!password) {
				fz::replace_substrings(token, _T("%%"), _T("%"));
			}

			t_loginCommand cmd;
			if (password || isProxyPass) {
				cmd.hide_arguments = true;
			}
			else {
				cmd.hide_arguments = false;
			}

			if (isUser && !password && !isAccount) {
				cmd.optional = false;
				cmd.type = loginCommandType::user;
			}
			else if (password && !isUser && !isAccount) {
				cmd.optional = true;
				cmd.type = loginCommandType::pass;
			}
			else if (isAccount && !isUser && !password) {
				cmd.optional = true;
				cmd.type = loginCommandType::account;
			}
			else {
				cmd.optional = false;
				cmd.type = loginCommandType::other;
			}

			cmd.command = token;

			pData->loginSequence.push_back(cmd);
		}

		if (pData->loginSequence.empty()) {
			LogMessage(MessageType::Error, _("Could not generate custom login sequence."));
			return false;
		}
	}
	else {
		LogMessage(MessageType::Error, _("Unknown FTP proxy type, cannot generate login sequence."));
		return false;
	}

	return true;
}

int CFtpControlSocket::Logon()
{
	const CharsetEncoding encoding = m_pCurrentServer->GetEncodingType();
	if (encoding == ENCODING_AUTO && CServerCapabilities::GetCapability(*m_pCurrentServer, utf8_command) != no)
		m_useUTF8 = true;
	else if (encoding == ENCODING_UTF8)
		m_useUTF8 = true;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::LogonParseResponse()
{
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("LogonParseResponse without m_pCurOpData called"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_INTERNALERROR;
	}

	CFtpLogonOpData *pData = static_cast<CFtpLogonOpData *>(m_pCurOpData);

	int code = GetReplyCode();

	if (pData->opState == LOGON_WELCOME) {
		if (code != 2 && code != 3) {
			DoClose(code == 5 ? FZ_REPLY_CRITICALERROR : 0);
			return FZ_REPLY_DISCONNECTED;
		}
	}
	else if (pData->opState == LOGON_AUTH_TLS ||
			 pData->opState == LOGON_AUTH_SSL)
	{
		if (code != 2 && code != 3) {
			CServerCapabilities::SetCapability(*m_pCurrentServer, (pData->opState == LOGON_AUTH_TLS) ? auth_tls_command : auth_ssl_command, no);
			if (pData->opState == LOGON_AUTH_SSL) {
				if (m_pCurrentServer->GetProtocol() == FTP) {
					// For now. In future make TLS mandatory unless explicitly requested INSECURE_FTP as protocol
					LogMessage(MessageType::Status, _("Insecure server, it does not support FTP over TLS."));
					pData->neededCommands[LOGON_PBSZ] = 0;
					pData->neededCommands[LOGON_PROT] = 0;

					pData->opState = LOGON_LOGON;
					return SendNextCommand();
				}
				else {
					DoClose(code == 5 ? FZ_REPLY_CRITICALERROR : 0);
					return FZ_REPLY_DISCONNECTED;
				}
			}
		}
		else {
			CServerCapabilities::SetCapability(*m_pCurrentServer, (pData->opState == LOGON_AUTH_TLS) ? auth_tls_command : auth_ssl_command, yes);

			LogMessage(MessageType::Status, _("Initializing TLS..."));

			wxASSERT(!m_pTlsSocket);
			delete m_pBackend;

			m_pTlsSocket = new CTlsSocket(this, *m_pSocket, this);
			m_pBackend = m_pTlsSocket;

			if (!m_pTlsSocket->Init()) {
				LogMessage(MessageType::Error, _("Failed to initialize TLS."));
				DoClose(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}

			int res = m_pTlsSocket->Handshake();
			if (res == FZ_REPLY_ERROR) {
				DoClose();
				return FZ_REPLY_ERROR;
			}

			pData->neededCommands[LOGON_AUTH_SSL] = 0;
			pData->opState = LOGON_AUTH_WAIT;

			if (res == FZ_REPLY_WOULDBLOCK)
				return FZ_REPLY_WOULDBLOCK;
		}
	}
	else if (pData->opState == LOGON_LOGON) {
		t_loginCommand cmd = pData->loginSequence.front();

		if (code != 2 && code != 3) {
			if( cmd.type == loginCommandType::user || cmd.type == loginCommandType::pass ) {
				auto const user = m_pCurrentServer->GetUser();
				if (!user.empty() && (user.front() == ' ' || user.back() == ' ')) {
					LogMessage(MessageType::Status, _("Check your login credentials. The entered username starts or ends with a space character."));
				}
				auto const pw = m_pCurrentServer->GetPass();
				if (!pw.empty() && (pw.front() == ' ' || pw.back() == ' ')) {
					LogMessage(MessageType::Status, _("Check your login credentials. The entered password starts or ends with a space character."));
				}
			}

			if (m_pCurrentServer->GetEncodingType() == ENCODING_AUTO && m_useUTF8) {
				// Fall back to local charset for the case that the server might not
				// support UTF8 and the login data contains non-ascii characters.
				bool asciiOnly = true;
				if (!fz::str_is_ascii(m_pCurrentServer->GetUser())) {
					asciiOnly = false;
				}
				if (!fz::str_is_ascii(m_pCurrentServer->GetPass())) {
					asciiOnly = false;
				}
				if (!fz::str_is_ascii(m_pCurrentServer->GetAccount())) {
					asciiOnly = false;
				}
				if (!asciiOnly) {
					if (pData->ftp_proxy_type) {
						LogMessage(MessageType::Status, _("Login data contains non-ASCII characters and server might not be UTF-8 aware. Cannot fall back to local charset since using proxy."));
						int error = FZ_REPLY_DISCONNECTED;
						if (cmd.type == loginCommandType::pass && code == 5)
							error |= FZ_REPLY_PASSWORDFAILED;
						DoClose(error);
						return FZ_REPLY_ERROR;
					}
					LogMessage(MessageType::Status, _("Login data contains non-ASCII characters and server might not be UTF-8 aware. Trying local charset."));
					m_useUTF8 = false;
					if (!GetLoginSequence(*m_pCurrentServer)) {
						int error = FZ_REPLY_DISCONNECTED;
						if (cmd.type == loginCommandType::pass && code == 5) {
							error |= FZ_REPLY_PASSWORDFAILED;
						}
						DoClose(error);
						return FZ_REPLY_ERROR;
					}
					return SendNextCommand();
				}
			}

			int error = FZ_REPLY_DISCONNECTED;
			if (cmd.type == loginCommandType::pass && code == 5)
				error |= FZ_REPLY_CRITICALERROR | FZ_REPLY_PASSWORDFAILED;
			DoClose(error);
			return FZ_REPLY_ERROR;
		}

		pData->loginSequence.pop_front();
		if (code == 2) {
			while (!pData->loginSequence.empty() && pData->loginSequence.front().optional)
				pData->loginSequence.pop_front();
		}
		else if (code == 3 && pData->loginSequence.empty()) {
			LogMessage(MessageType::Error, _("Login sequence fully executed yet not logged in. Aborting."));
			if (cmd.type == loginCommandType::pass && m_pCurrentServer->GetAccount().empty())
				LogMessage(MessageType::Error, _("Server might require an account. Try specifying an account using the Site Manager"));
			DoClose(FZ_REPLY_CRITICALERROR);
			return FZ_REPLY_ERROR;
		}

		if (!pData->loginSequence.empty()) {
			pData->waitChallenge = false;

			return SendNextCommand();
		}
	}
	else if (pData->opState == LOGON_SYST) {
		if (code == 2)
			CServerCapabilities::SetCapability(*GetCurrentServer(), syst_command, yes, m_Response.Mid(4));
		else
			CServerCapabilities::SetCapability(*GetCurrentServer(), syst_command, no);

		if (m_pCurrentServer->GetType() == DEFAULT && code == 2) {
			if (m_Response.Length() > 7 && m_Response.Mid(3, 4) == _T(" MVS"))
				m_pCurrentServer->SetType(MVS);
			else if (m_Response.Len() > 12 && m_Response.Mid(3, 9).Upper() == _T(" NONSTOP "))
				m_pCurrentServer->SetType(HPNONSTOP);

			if (!m_MultilineResponseLines.empty() && fz::str_tolower_ascii(m_MultilineResponseLines.front().substr(4, 4)) == _T("z/vm")) {
				CServerCapabilities::SetCapability(*GetCurrentServer(), syst_command, yes, m_MultilineResponseLines.front().substr(4) + _T(" ") + m_Response.Mid(4));
				m_pCurrentServer->SetType(ZVM);
			}
		}

		if (m_Response.Find(_T("FileZilla")) != -1) {
			pData->neededCommands[LOGON_CLNT] = 0;
			pData->neededCommands[LOGON_OPTSUTF8] = 0;
		}
	}
	else if (pData->opState == LOGON_FEAT) {
		if (code == 2) {
			CServerCapabilities::SetCapability(*GetCurrentServer(), feat_command, yes);
			if (CServerCapabilities::GetCapability(*m_pCurrentServer, utf8_command) != yes)
				CServerCapabilities::SetCapability(*m_pCurrentServer, utf8_command, no);
			if (CServerCapabilities::GetCapability(*m_pCurrentServer, clnt_command) != yes)
				CServerCapabilities::SetCapability(*m_pCurrentServer, clnt_command, no);
		}
		else
			CServerCapabilities::SetCapability(*GetCurrentServer(), feat_command, no);

		if (CServerCapabilities::GetCapability(*m_pCurrentServer, tvfs_support) != yes)
			CServerCapabilities::SetCapability(*m_pCurrentServer, tvfs_support, no);

		const CharsetEncoding encoding = m_pCurrentServer->GetEncodingType();
		if (encoding == ENCODING_AUTO && CServerCapabilities::GetCapability(*m_pCurrentServer, utf8_command) != yes)
		{
			LogMessage(MessageType::Status, _("Server does not support non-ASCII characters."));
			m_useUTF8 = false;
		}
	}
	else if (pData->opState == LOGON_PROT) {
		if (code == 2 || code == 3)
			m_protectDataChannel = true;
	}
	else if (pData->opState == LOGON_CUSTOMCOMMANDS) {
		++pData->customCommandIndex;
		if (pData->customCommandIndex < m_pCurrentServer->GetPostLoginCommands().size())
			return SendNextCommand();
	}

	for (;;) {
		++pData->opState;

		if (pData->opState == LOGON_DONE) {
			LogMessage(MessageType::Status, _("Logged in"));
			ResetOperation(FZ_REPLY_OK);
			LogMessage(MessageType::Debug_Info, _T("Measured latency of %d ms"), m_rtt.GetLatency());
			return true;
		}

		if (!pData->neededCommands[pData->opState])
			continue;
		else if (pData->opState == LOGON_SYST) {
			wxString system;
			capabilities cap = CServerCapabilities::GetCapability(*GetCurrentServer(), syst_command, &system);
			if (cap == unknown)
				break;
			else if (cap == yes) {
				if (m_pCurrentServer->GetType() == DEFAULT) {
					if (system.Left(3) == _T("MVS"))
						m_pCurrentServer->SetType(MVS);
					else if (system.Left(4).Upper() == _T("Z/VM"))
						m_pCurrentServer->SetType(ZVM);
					else if (system.Left(8).Upper() == _T("NONSTOP "))
						m_pCurrentServer->SetType(HPNONSTOP);
				}

				if (system.Find(_T("FileZilla")) != -1) {
					pData->neededCommands[LOGON_CLNT] = 0;
					pData->neededCommands[LOGON_OPTSUTF8] = 0;
				}
			}
		}
		else if (pData->opState == LOGON_FEAT) {
			capabilities cap = CServerCapabilities::GetCapability(*GetCurrentServer(), feat_command);
			if (cap == unknown)
				break;
			const CharsetEncoding encoding = m_pCurrentServer->GetEncodingType();
			if (encoding == ENCODING_AUTO && CServerCapabilities::GetCapability(*m_pCurrentServer, utf8_command) != yes) {
				LogMessage(MessageType::Status, _("Server does not support non-ASCII characters."));
				m_useUTF8 = false;
			}
		}
		else if (pData->opState == LOGON_CLNT) {
			if (!m_useUTF8)
				continue;

			if (CServerCapabilities::GetCapability(*GetCurrentServer(), clnt_command) == yes)
				break;
		}
		else if (pData->opState == LOGON_OPTSUTF8) {
			if (!m_useUTF8)
				continue;

			if (CServerCapabilities::GetCapability(*GetCurrentServer(), utf8_command) == yes)
				break;
		}
		else if (pData->opState == LOGON_OPTSMLST) {
			wxString facts;
			if (CServerCapabilities::GetCapability(*GetCurrentServer(), mlsd_command, &facts) != yes)
				continue;
			capabilities cap = CServerCapabilities::GetCapability(*GetCurrentServer(), opst_mlst_command);
			if (cap == unknown) {
				facts = fz::str_tolower_ascii(facts);

				bool had_unset = false;
				wxString opts_facts;

				// Create a list of all facts understood by both FZ and the server.
				// Check if there's any supported fact not enabled by default, should that
				// be the case we need to send OPTS MLST
				while (!facts.empty()) {
					int delim = facts.Find(';');
					if (delim == -1)
						break;

					if (!delim) {
						facts = facts.Mid(1);
						continue;
					}

					bool enabled;
					wxString fact;

					if (facts[delim - 1] == '*') {
						if (delim == 1) {
							facts = facts.Mid(delim + 1);
							continue;
						}
						enabled = true;
						fact = facts.Left(delim - 1);
					}
					else {
						enabled = false;
						fact = facts.Left(delim);
					}
					facts = facts.Mid(delim + 1);

					if (fact == _T("type") ||
						fact == _T("size") ||
						fact == _T("modify") ||
						fact == _T("perm") ||
						fact == _T("unix.mode") ||
						fact == _T("unix.owner") ||
						fact == _T("unix.user") ||
						fact == _T("unix.group") ||
						fact == _T("unix.uid") ||
						fact == _T("unix.gid") ||
						fact == _T("x.hidden"))
					{
						had_unset |= !enabled;
						opts_facts += fact + _T(";");
					}
				}

				if (had_unset) {
					CServerCapabilities::SetCapability(*GetCurrentServer(), opst_mlst_command, yes, opts_facts);
					break;
				}
				else
					CServerCapabilities::SetCapability(*GetCurrentServer(), opst_mlst_command, no);
			}
			else if (cap == yes)
				break;
		}
		else
			break;
	}

	return SendNextCommand();
}

int CFtpControlSocket::LogonSend()
{
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("LogonParseResponse without m_pCurOpData called"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_INTERNALERROR;
	}

	CFtpLogonOpData *pData = static_cast<CFtpLogonOpData *>(m_pCurOpData);

	bool res;
	switch (pData->opState)
	{
	case LOGON_AUTH_WAIT:
		res = FZ_REPLY_WOULDBLOCK;
		LogMessage(MessageType::Debug_Info, _T("LogonSend() called during LOGON_AUTH_WAIT, ignoring"));
		break;
	case LOGON_AUTH_TLS:
		res = SendCommand(_T("AUTH TLS"), false, false);
		break;
	case LOGON_AUTH_SSL:
		res = SendCommand(_T("AUTH SSL"), false, false);
		break;
	case LOGON_SYST:
		res = SendCommand(_T("SYST"));
		break;
	case LOGON_LOGON:
		{
			t_loginCommand cmd = pData->loginSequence.front();
			switch (cmd.type)
			{
			case loginCommandType::user:
				if (m_pCurrentServer->GetLogonType() == INTERACTIVE) {
					pData->waitChallenge = true;
					pData->challenge = _T("");
				}

				if (cmd.command.empty())
					res = SendCommand(_T("USER ") + m_pCurrentServer->GetUser());
				else
					res = SendCommand(cmd.command);
				break;
			case loginCommandType::pass:
				if (!pData->challenge.empty()) {
					CInteractiveLoginNotification *pNotification = new CInteractiveLoginNotification(CInteractiveLoginNotification::interactive, pData->challenge, false);
					pNotification->server = *m_pCurrentServer;
					pData->challenge = _T("");

					SendAsyncRequest(pNotification);

					return FZ_REPLY_WOULDBLOCK;
				}

				if (cmd.command.empty())
					res = SendCommand(_T("PASS ") + m_pCurrentServer->GetPass(), true);
				else {
					wxString c = cmd.command;
					wxString pass = m_pCurrentServer->GetPass();
					pass.Replace(_T("%"), _T("%%"));
					c.Replace(_T("%p"), pass);
					c.Replace(_T("%%"), _T("%"));
					res = SendCommand(c, true);
				}
				break;
			case loginCommandType::account:
				if (cmd.command.empty())
					res = SendCommand(_T("ACCT ") + m_pCurrentServer->GetAccount());
				else
					res = SendCommand(cmd.command);
				break;
			case loginCommandType::other:
				wxASSERT(!cmd.command.empty());
				res = SendCommand(cmd.command, cmd.hide_arguments);
				break;
			default:
				res = false;
				break;
			}
		}
		break;
	case LOGON_FEAT:
		res = SendCommand(_T("FEAT"));
		break;
	case LOGON_CLNT:
		// Some servers refuse to enable UTF8 if client does not send CLNT command
		// to fix compatibility with Internet Explorer, but in the process breaking
		// compatibility with other clients.
		// Rather than forcing MS to fix Internet Explorer, letting other clients
		// suffer is a questionable decision in my opinion.
		res = SendCommand(_T("CLNT FileZilla"));
		break;
	case LOGON_OPTSUTF8:
		// Handle servers that disobey RFC 2640 by having UTF8 in their FEAT
		// response but do not use UTF8 unless OPTS UTF8 ON gets send.
		// However these servers obey a conflicting ietf draft:
		// http://www.ietf.org/proceedings/02nov/I-D/draft-ietf-ftpext-utf-8-option-00.txt
		// Example servers are, amongst others, G6 FTP Server and RaidenFTPd.
		res = SendCommand(_T("OPTS UTF8 ON"));
		break;
	case LOGON_PBSZ:
		res = SendCommand(_T("PBSZ 0"));
		break;
	case LOGON_PROT:
		res = SendCommand(_T("PROT P"));
		break;
	case LOGON_CUSTOMCOMMANDS:
		if (pData->customCommandIndex >= m_pCurrentServer->GetPostLoginCommands().size())
		{
			LogMessage(MessageType::Debug_Warning, _T("pData->customCommandIndex >= m_pCurrentServer->GetPostLoginCommands().size()"));
			DoClose(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		res = SendCommand(m_pCurrentServer->GetPostLoginCommands()[pData->customCommandIndex]);
		break;
	case LOGON_OPTSMLST:
		{
			wxString args;
			CServerCapabilities::GetCapability(*GetCurrentServer(), opst_mlst_command, &args);
			res = SendCommand(_T("OPTS MLST " + args));
		}
		break;
	default:
		return FZ_REPLY_ERROR;
	}

	if (!res)
		return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::GetReplyCode() const
{
	if (m_Response.empty()) {
		return 0;
	}
	else if (m_Response[0] < '0' || m_Response[0] > '9') {
		return 0;
	}
	else {
		return m_Response[0] - '0';
	}
}

bool CFtpControlSocket::SendCommand(wxString const& str, bool maskArgs, bool measureRTT)
{
	int pos;
	if (maskArgs && (pos = str.Find(_T(" "))) != -1)
	{
		wxString stars('*', str.Length() - pos - 1);
		LogMessageRaw(MessageType::Command, str.Left(pos + 1) + stars);
	}
	else
		LogMessageRaw(MessageType::Command, str);

	wxCharBuffer buffer = ConvToServer(str + _T("\r\n"));
	if (!buffer)
	{
		LogMessage(MessageType::Error, _T("Failed to convert command to 8 bit charset"));
		return false;
	}
	unsigned int len = (unsigned int)strlen(buffer);
	bool res = CRealControlSocket::Send(buffer, len);
	if (res)
		++m_pendingReplies;

	if (measureRTT)
		m_rtt.Start();

	return res;
}

class CFtpListOpData : public COpData, public CFtpTransferOpData
{
public:
	CFtpListOpData()
		: COpData(Command::list)
	{
	}

	virtual ~CFtpListOpData()
	{
		delete m_pDirectoryListingParser;
	}

	CServerPath path;
	std::wstring subDir;
	bool fallback_to_current{};

	CDirectoryListingParser* m_pDirectoryListingParser{};

	CDirectoryListing directoryListing;

	// Set to true to get a directory listing even if a cache
	// lookup can be made after finding out true remote directory
	bool refresh{};

	bool viewHiddenCheck{};
	bool viewHidden{}; // Uses LIST -a command

	// Listing index for list_mdtm
	int mdtm_index{};

	fz::monotonic_clock m_time_before_locking;
};

enum listStates
{
	list_init = 0,
	list_waitcwd,
	list_waitlock,
	list_waittransfer,
	list_mdtm
};

int CFtpControlSocket::List(CServerPath path, std::wstring const& subDir, int flags)
{
	CServerPath newPath = m_CurrentPath;
	if (!path.empty()) {
		newPath = path;
	}
	if (!newPath.ChangePath(subDir)) {
		newPath.clear();
	}

	if (newPath.empty()) {
		LogMessage(MessageType::Status, _("Retrieving directory listing..."));
	}
	else {
		LogMessage(MessageType::Status, _("Retrieving directory listing of \"%s\"..."), newPath.GetPath());
	}

	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("List called from other command"));
	}
	CFtpListOpData *pData = new CFtpListOpData;
	pData->pNextOpData = m_pCurOpData;
	m_pCurOpData = pData;

	pData->opState = list_waitcwd;

	if (path.GetType() == DEFAULT) {
		path.SetType(m_pCurrentServer->GetType());
	}
	pData->path = path;
	pData->subDir = subDir;
	pData->refresh = (flags & LIST_FLAG_REFRESH) != 0;
	pData->fallback_to_current = !path.empty() && (flags & LIST_FLAG_FALLBACK_CURRENT) != 0;

	int res = ChangeDir(path, subDir, (flags & LIST_FLAG_LINK) != 0);
	if (res != FZ_REPLY_OK) {
		return res;
	}

	return ParseSubcommandResult(FZ_REPLY_OK);
}

int CFtpControlSocket::ListSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ListSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpListOpData *pData = static_cast<CFtpListOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == list_waitcwd) {
		if (prevResult != FZ_REPLY_OK) {
			if (prevResult & FZ_REPLY_LINKNOTDIR) {
				ResetOperation(prevResult);
				return FZ_REPLY_ERROR;
			}

			if (pData->fallback_to_current) {
				// List current directory instead
				pData->fallback_to_current = false;
				pData->path.clear();
				pData->subDir = _T("");
				int res = ChangeDir();
				if (res != FZ_REPLY_OK)
					return res;
			}
			else {
				ResetOperation(prevResult);
				return FZ_REPLY_ERROR;
			}
		}
		if (pData->path.empty()) {
			pData->path = m_CurrentPath;
			wxASSERT(pData->subDir.empty());
			wxASSERT(!pData->path.empty());
		}

		if (!pData->refresh) {
			wxASSERT(!pData->pNextOpData);

			// Do a cache lookup now that we know the correct directory
			int hasUnsureEntries;
			bool is_outdated = false;
			bool found = engine_.GetDirectoryCache().DoesExist(*m_pCurrentServer, m_CurrentPath, hasUnsureEntries, is_outdated);
			if (found) {
				// We're done if listing is recent and has no outdated entries
				if (!is_outdated && !hasUnsureEntries) {
					engine_.SendDirectoryListingNotification(m_CurrentPath, true, false, false);

					ResetOperation(FZ_REPLY_OK);

					return FZ_REPLY_OK;
				}
			}
		}

		if (!pData->holdsLock) {
			if (!TryLockCache(lock_list, m_CurrentPath)) {
				pData->opState = list_waitlock;
				pData->m_time_before_locking = fz::monotonic_clock::now();
				return FZ_REPLY_WOULDBLOCK;
			}
		}

		delete m_pTransferSocket;
		m_pTransferSocket = new CTransferSocket(engine_, *this, TransferMode::list);

		// Assume that a server supporting UTF-8 does not send EBCDIC listings.
		listingEncoding::type encoding = listingEncoding::unknown;
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, utf8_command) == yes)
			encoding = listingEncoding::normal;

		pData->m_pDirectoryListingParser = new CDirectoryListingParser(this, *m_pCurrentServer, encoding);

		pData->m_pDirectoryListingParser->SetTimezoneOffset(GetTimezoneOffset());
		m_pTransferSocket->m_pDirectoryListingParser = pData->m_pDirectoryListingParser;

		engine_.transfer_status_.Init(-1, 0, true);

		pData->opState = list_waittransfer;
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, mlsd_command) == yes)
			return Transfer(_T("MLSD"), pData);
		else {
			if (engine_.GetOptions().GetOptionVal(OPTION_VIEW_HIDDEN_FILES)) {
				capabilities cap = CServerCapabilities::GetCapability(*m_pCurrentServer, list_hidden_support);
				if (cap == unknown)
					pData->viewHiddenCheck = true;
				else if (cap == yes)
					pData->viewHidden = true;
				else
					LogMessage(MessageType::Debug_Info, _("View hidden option set, but unsupported by server"));
			}

			if (pData->viewHidden)
				return Transfer(_T("LIST -a"), pData);
			else
				return Transfer(_T("LIST"), pData);
		}
	}
	else if (pData->opState == list_waittransfer) {
		if (prevResult == FZ_REPLY_OK) {
			CDirectoryListing listing = pData->m_pDirectoryListingParser->Parse(m_CurrentPath);

			if (pData->viewHiddenCheck) {
				if (!pData->viewHidden) {
					// Repeat with LIST -a
					pData->viewHidden = true;
					pData->directoryListing = listing;

					// Reset status
					pData->transferEndReason = TransferEndReason::successful;
					pData->tranferCommandSent = false;
					delete m_pTransferSocket;
					m_pTransferSocket = new CTransferSocket(engine_, *this, TransferMode::list);
					pData->m_pDirectoryListingParser->Reset();
					m_pTransferSocket->m_pDirectoryListingParser = pData->m_pDirectoryListingParser;

					return Transfer(_T("LIST -a"), pData);
				}
				else {
					if (CheckInclusion(listing, pData->directoryListing)) {
						LogMessage(MessageType::Debug_Info, _T("Server seems to support LIST -a"));
						CServerCapabilities::SetCapability(*m_pCurrentServer, list_hidden_support, yes);
					}
					else {
						LogMessage(MessageType::Debug_Info, _T("Server does not seem to support LIST -a"));
						CServerCapabilities::SetCapability(*m_pCurrentServer, list_hidden_support, no);
						listing = pData->directoryListing;
					}
				}
			}

			SetAlive();

			int res = ListCheckTimezoneDetection(listing);
			if (res != FZ_REPLY_OK)
				return res;

			engine_.GetDirectoryCache().Store(listing, *m_pCurrentServer);

			engine_.SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, true, false);

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else {
			if (pData->tranferCommandSent && IsMisleadingListResponse()) {
				CDirectoryListing listing;
				listing.path = m_CurrentPath;
				listing.m_firstListTime = fz::monotonic_clock::now();

				if (pData->viewHiddenCheck) {
					if (pData->viewHidden) {
						if (pData->directoryListing.GetCount()) {
							// Less files with LIST -a
							// Not supported
							LogMessage(MessageType::Debug_Info, _T("Server does not seem to support LIST -a"));
							CServerCapabilities::SetCapability(*m_pCurrentServer, list_hidden_support, no);
							listing = pData->directoryListing;
						}
						else {
							LogMessage(MessageType::Debug_Info, _T("Server seems to support LIST -a"));
							CServerCapabilities::SetCapability(*m_pCurrentServer, list_hidden_support, yes);
						}
					}
					else {
						// Reset status
						pData->transferEndReason = TransferEndReason::successful;
						pData->tranferCommandSent = false;
						delete m_pTransferSocket;
						m_pTransferSocket = new CTransferSocket(engine_, *this, TransferMode::list);
						pData->m_pDirectoryListingParser->Reset();
						m_pTransferSocket->m_pDirectoryListingParser = pData->m_pDirectoryListingParser;

						// Repeat with LIST -a
						pData->viewHidden = true;
						pData->directoryListing = listing;
						return Transfer(_T("LIST -a"), pData);
					}
				}

				int res = ListCheckTimezoneDetection(listing);
				if (res != FZ_REPLY_OK)
					return res;

				engine_.GetDirectoryCache().Store(listing, *m_pCurrentServer);

				engine_.SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, true, false);

				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else {
				if (pData->viewHiddenCheck) {
					// If server does not support LIST -a, the server might reject this command
					// straight away. In this case, back to the previously retrieved listing.
					// On other failures like timeouts and such, return an error
					if (pData->viewHidden &&
						pData->transferEndReason == TransferEndReason::transfer_command_failure_immediate)
					{
						CServerCapabilities::SetCapability(*m_pCurrentServer, list_hidden_support, no);

						int res = ListCheckTimezoneDetection(pData->directoryListing);
						if (res != FZ_REPLY_OK)
							return res;

						engine_.GetDirectoryCache().Store(pData->directoryListing, *m_pCurrentServer);

						engine_.SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, true, false);

						ResetOperation(FZ_REPLY_OK);
						return FZ_REPLY_OK;
					}
				}

				if (prevResult & FZ_REPLY_ERROR)
					engine_.SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, true, true);
			}

			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}
	}
	else {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Wrong opState: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}
}

int CFtpControlSocket::ListSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ListSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpListOpData *pData = static_cast<CFtpListOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState == list_waitlock) {
		if (!pData->holdsLock) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Not holding the lock as expected"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}

		// Check if we can use already existing listing
		CDirectoryListing listing;
		bool is_outdated = false;
		wxASSERT(pData->subDir.empty()); // Did do ChangeDir before trying to lock
		bool found = engine_.GetDirectoryCache().Lookup(listing, *m_pCurrentServer, pData->path, true, is_outdated);
		if (found && !is_outdated && !listing.get_unsure_flags() &&
			listing.m_firstListTime >= pData->m_time_before_locking)
		{
			engine_.SendDirectoryListingNotification(listing.path, !pData->pNextOpData, false, false);

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}

		pData->opState = list_waitcwd;

		return ListSubcommandResult(FZ_REPLY_OK);
	}
	if (pData->opState == list_mdtm) {
		LogMessage(MessageType::Status, _("Calculating timezone offset of server..."));
		wxString cmd = _T("MDTM ") + m_CurrentPath.FormatFilename(pData->directoryListing[pData->mdtm_index].name, true);
		if (!SendCommand(cmd))
			return FZ_REPLY_ERROR;
		else
			return FZ_REPLY_WOULDBLOCK;
	}

	LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("invalid opstate"));
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CFtpControlSocket::ListParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ListParseResponse()"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpListOpData *pData = static_cast<CFtpListOpData *>(m_pCurOpData);

	if (pData->opState != list_mdtm) {
		LogMessage(MessageType::Debug_Warning, _T("ListParseResponse should never be called if opState != list_mdtm"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}


	// First condition prevents problems with concurrent MDTM
	if (CServerCapabilities::GetCapability(*m_pCurrentServer, timezone_offset) == unknown &&
		m_Response.Left(4) == _T("213 ") && m_Response.Length() > 16)
	{
		fz::datetime date(m_Response.Mid(4).ToStdWstring(), fz::datetime::utc);
		if (!date.empty()) {
			wxASSERT(pData->directoryListing[pData->mdtm_index].has_date());
			fz::datetime listTime = pData->directoryListing[pData->mdtm_index].time;
			listTime -= fz::duration::from_minutes(m_pCurrentServer->GetTimezoneOffset());

			int serveroffset = static_cast<int>((date - listTime).get_seconds());
			if (!pData->directoryListing[pData->mdtm_index].has_seconds()) {
				// Round offset to full minutes
				if (serveroffset < 0)
					serveroffset -= 59;
				serveroffset -= serveroffset % 60;
			}

			LogMessage(MessageType::Status, _("Timezone offset of server is %d seconds."), -serveroffset);

			fz::duration span = fz::duration::from_seconds(serveroffset);
			const int count = pData->directoryListing.GetCount();
			for (int i = 0; i < count; ++i) {
				CDirentry& entry = pData->directoryListing[i];
				entry.time += span;
			}

			// TODO: Correct cached listings

			CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, yes, serveroffset);
		}
		else {
			CServerCapabilities::SetCapability(*m_pCurrentServer, mdtm_command, no);
			CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, no);
		}
	}
	else
		CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, no);

	engine_.GetDirectoryCache().Store(pData->directoryListing, *m_pCurrentServer);

	engine_.SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, true, false);

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

int CFtpControlSocket::ListCheckTimezoneDetection(CDirectoryListing& listing)
{
	wxASSERT(m_pCurOpData);

	CFtpListOpData *pData = static_cast<CFtpListOpData *>(m_pCurOpData);

	if (CServerCapabilities::GetCapability(*m_pCurrentServer, timezone_offset) == unknown)
	{
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, mdtm_command) != yes)
			CServerCapabilities::SetCapability(*m_pCurrentServer, timezone_offset, no);
		else
		{
			const int count = listing.GetCount();
			for (int i = 0; i < count; ++i) {
				if (!listing[i].is_dir() && listing[i].has_time()) {
					pData->opState = list_mdtm;
					pData->directoryListing = listing;
					pData->mdtm_index = i;
					return SendNextCommand();
				}
			}
		}
	}

	return FZ_REPLY_OK;
}

int CFtpControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ResetOperation(%d)"), nErrorCode);

	CTransferSocket* pTransferSocket = m_pTransferSocket;
	m_pTransferSocket = 0;
	delete pTransferSocket;
	delete m_pIPResolver;
	m_pIPResolver = 0;

	m_repliesToSkip = m_pendingReplies;

	if (m_pCurOpData && m_pCurOpData->opId == Command::transfer) {
		CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);
		if (pData->tranferCommandSent) {
			if (pData->transferEndReason == TransferEndReason::transfer_failure_critical)
				nErrorCode |= FZ_REPLY_CRITICALERROR | FZ_REPLY_WRITEFAILED;
			if (pData->transferEndReason != TransferEndReason::transfer_command_failure_immediate || GetReplyCode() != 5)
				pData->transferInitiated = true;
			else {
				if (nErrorCode == FZ_REPLY_ERROR)
					nErrorCode |= FZ_REPLY_CRITICALERROR;
			}
		}
		if (nErrorCode != FZ_REPLY_OK && pData->download && !pData->fileDidExist) {
			delete pData->pIOThread;
			pData->pIOThread = 0;
			int64_t size;
			bool isLink;
			if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file && size == 0) {
				// Download failed and a new local file was created before, but
				// nothing has been written to it. Remove it again, so we don't
				// leave a bunch of empty files all over the place.
				LogMessage(MessageType::Debug_Verbose, _T("Deleting empty file"));
				wxRemoveFile(pData->localFile);
			}
		}
	}
	if (m_pCurOpData && m_pCurOpData->opId == Command::del && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);
		if (pData->m_needSendListing)
			engine_.SendDirectoryListingNotification(pData->path, false, true, false);
	}

	if (m_pCurOpData && m_pCurOpData->opId == Command::rawtransfer &&
		nErrorCode != FZ_REPLY_OK)
	{
		CRawTransferOpData *pData = static_cast<CRawTransferOpData *>(m_pCurOpData);
		if (pData->pOldData->transferEndReason == TransferEndReason::successful)
		{
			if ((nErrorCode & FZ_REPLY_TIMEOUT) == FZ_REPLY_TIMEOUT)
				pData->pOldData->transferEndReason = TransferEndReason::timeout;
			else if (!pData->pOldData->tranferCommandSent)
				pData->pOldData->transferEndReason = TransferEndReason::pre_transfer_command_failure;
			else
				pData->pOldData->transferEndReason = TransferEndReason::failure;
		}
	}

	m_lastCommandCompletionTime = fz::monotonic_clock::now();
	if (m_pCurOpData && !(nErrorCode & FZ_REPLY_DISCONNECTED))
		StartKeepaliveTimer();
	else {
		stop_timer(m_idleTimer);
		m_idleTimer = 0;
	}

	return CControlSocket::ResetOperation(nErrorCode);
}

int CFtpControlSocket::SendNextCommand()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::SendNextCommand()"));
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("SendNextCommand called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->waitForAsyncRequest)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Waiting for async request, ignoring SendNextCommand..."));
		return FZ_REPLY_WOULDBLOCK;
	}

	if (m_repliesToSkip)
	{
		LogMessage(MessageType::Status, _T("Waiting for replies to skip before sending next command..."));
		SetWait(true);
		return FZ_REPLY_WOULDBLOCK;
	}

	switch (m_pCurOpData->opId)
	{
	case Command::list:
		return ListSend();
	case Command::connect:
		return LogonSend();
	case Command::cwd:
		return ChangeDirSend();
	case Command::transfer:
		return FileTransferSend();
	case Command::mkdir:
		return MkdirSend();
	case Command::rename:
		return RenameSend();
	case Command::chmod:
		return ChmodSend();
	case Command::rawtransfer:
		return TransferSend();
	case Command::raw:
		return RawCommandSend();
	case Command::del:
		return DeleteSend();
	case Command::removedir:
		return RemoveDirSend();
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown opID (%d) in SendNextCommand"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}

class CFtpChangeDirOpData : public CChangeDirOpData
{
public:
	CFtpChangeDirOpData()
	{
		tried_cdup = false;
	}

	virtual ~CFtpChangeDirOpData()
	{
	}

	bool tried_cdup;
};

enum cwdStates
{
	cwd_init = 0,
	cwd_pwd,
	cwd_cwd,
	cwd_pwd_cwd,
	cwd_cwd_subdir,
	cwd_pwd_subdir
};

int CFtpControlSocket::ChangeDir(CServerPath path, std::wstring subDir, bool link_discovery)
{
	cwdStates state = cwd_init;

	if (path.GetType() == DEFAULT) {
		path.SetType(m_pCurrentServer->GetType());
	}

	CServerPath target;
	if (path.empty()) {
		if (m_CurrentPath.empty()) {
			state = cwd_pwd;
		}
		else {
			return FZ_REPLY_OK;
		}
	}
	else {
		if (!subDir.empty()) {
			// Check if the target is in cache already
			target = engine_.GetPathCache().Lookup(*m_pCurrentServer, path, subDir);
			if (!target.empty()) {
				if (m_CurrentPath == target) {
					return FZ_REPLY_OK;
				}

				path = target;
				subDir.clear();
				state = cwd_cwd;
			}
			else {
				// Target unknown, check for the parent's target
				target = engine_.GetPathCache().Lookup(*m_pCurrentServer, path, _T(""));
				if (m_CurrentPath == path || (!target.empty() && target == m_CurrentPath)) {
					target.clear();
					state = cwd_cwd_subdir;
				}
				else {
					state = cwd_cwd;
				}
			}
		}
		else {
			target = engine_.GetPathCache().Lookup(*m_pCurrentServer, path, _T(""));
			if (m_CurrentPath == path || (!target.empty() && target == m_CurrentPath)) {
				return FZ_REPLY_OK;
			}
			state = cwd_cwd;
		}
	}

	CFtpChangeDirOpData *pData = new CFtpChangeDirOpData;
	pData->pNextOpData = m_pCurOpData;
	pData->opState = state;
	pData->path = path;
	pData->subDir = subDir;
	pData->target = target;
	pData->link_discovery = link_discovery;

	if (pData->pNextOpData && pData->pNextOpData->opId == Command::transfer &&
		!static_cast<CFtpFileTransferOpData *>(pData->pNextOpData)->download)
	{
		pData->tryMkdOnFail = true;
		assert(subDir.empty());
	}


	m_pCurOpData = pData;

	return SendNextCommand();
}

int CFtpControlSocket::ChangeDirParseResponse()
{
	if (!m_pCurOpData) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CFtpChangeDirOpData *pData = static_cast<CFtpChangeDirOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case cwd_pwd:
		if (code != 2 && code != 3) {
			error = true;
		}
		else if (ParsePwdReply(m_Response.ToStdWstring())) {
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else {
			error = true;
		}
		break;
	case cwd_cwd:
		if (code != 2 && code != 3) {
			// Create remote directory if part of a file upload
			if (pData->tryMkdOnFail) {
				pData->tryMkdOnFail = false;
				int res = Mkdir(pData->path);
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
			else {
				error = true;
			}
		}
		else {
			if (pData->target.empty()) {
				pData->opState = cwd_pwd_cwd;
			}
			else {
				m_CurrentPath = pData->target;
				if (pData->subDir.empty()) {
					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_OK;
				}

				pData->target.clear();
				pData->opState = cwd_cwd_subdir;
			}
		}
		break;
	case cwd_pwd_cwd:
		if (code != 2 && code != 3) 	{
			LogMessage(MessageType::Debug_Warning, _T("PWD failed, assuming path is '%s'."), pData->path.GetPath());
			m_CurrentPath = pData->path;

			if (pData->target.empty()) {
				engine_.GetPathCache().Store(*m_pCurrentServer, m_CurrentPath, pData->path);
			}

			if (pData->subDir.empty()) {
				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else {
				pData->opState = cwd_cwd_subdir;
			}
		}
		else if (ParsePwdReply(m_Response.ToStdWstring(), false, pData->path)) {
			if (pData->target.empty()) {
				engine_.GetPathCache().Store(*m_pCurrentServer, m_CurrentPath, pData->path);
			}
			if (pData->subDir.empty()) {
				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else {
				pData->opState = cwd_cwd_subdir;
			}
		}
		else {
			error = true;
		}
		break;
	case cwd_cwd_subdir:
		if (code != 2 && code != 3) {
			if (pData->subDir == _T("..") && !pData->tried_cdup && m_Response.Left(2) == _T("50")) {
				// CDUP command not implemented, try again using CWD ..
				pData->tried_cdup = true;
			}
			else if (pData->link_discovery) {
				LogMessage(MessageType::Debug_Info, _T("Symlink does not link to a directory, probably a file"));
				ResetOperation(FZ_REPLY_LINKNOTDIR);
				return FZ_REPLY_ERROR;
			}
			else {
				error = true;
			}
		}
		else {
			pData->opState = cwd_pwd_subdir;
		}
		break;
	case cwd_pwd_subdir:
		{
			CServerPath assumedPath(pData->path);
			if (pData->subDir == _T("..")) {
				if (!assumedPath.HasParent()) {
					assumedPath.clear();
				}
				else {
					assumedPath = assumedPath.GetParent();
				}
			}
			else {
				assumedPath.AddSegment(pData->subDir);
			}

			if (code != 2 && code != 3) {
				if (!assumedPath.empty()) {
					LogMessage(MessageType::Debug_Warning, _T("PWD failed, assuming path is '%s'."), assumedPath.GetPath());
					m_CurrentPath = assumedPath;

					if (pData->target.empty()) {
						engine_.GetPathCache().Store(*m_pCurrentServer, m_CurrentPath, pData->path, pData->subDir);
					}

					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_OK;
				}
				else {
					LogMessage(MessageType::Debug_Warning, _T("PWD failed, unable to guess current path."));
					error = true;
				}
			}
			else if (ParsePwdReply(m_Response.ToStdWstring(), false, assumedPath)) {
				if (pData->target.empty()) {
					engine_.GetPathCache().Store(*m_pCurrentServer, m_CurrentPath, pData->path, pData->subDir);
				}

				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else {
				error = true;
			}
		}
		break;
	}

	if (error) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CFtpControlSocket::ChangeDirSubcommandResult(int)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ChangeDirSubcommandResult()"));

	return SendNextCommand();
}

int CFtpControlSocket::ChangeDirSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ChangeDirSend()"));

	if (!m_pCurOpData) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CFtpChangeDirOpData *pData = static_cast<CFtpChangeDirOpData *>(m_pCurOpData);

	wxString cmd;
	switch (pData->opState)
	{
	case cwd_pwd:
	case cwd_pwd_cwd:
	case cwd_pwd_subdir:
		cmd = _T("PWD");
		break;
	case cwd_cwd:
		if (pData->tryMkdOnFail && !pData->holdsLock)
		{
			if (IsLocked(lock_mkdir, pData->path))
			{
				// Some other engine is already creating this directory or
				// performing an action that will lead to its creation
				pData->tryMkdOnFail = false;
			}
			if (!TryLockCache(lock_mkdir, pData->path))
				return FZ_REPLY_WOULDBLOCK;
		}
		cmd = _T("CWD ") + pData->path.GetPath();
		m_CurrentPath.clear();
		break;
	case cwd_cwd_subdir:
		if (pData->subDir.empty())
		{
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		else if (pData->subDir == _T("..") && !pData->tried_cdup)
			cmd = _T("CDUP");
		else
			cmd = _T("CWD ") + pData->path.FormatSubdir(pData->subDir);
		m_CurrentPath.clear();
		break;
	}

	if (!cmd.empty())
		if (!SendCommand(cmd))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
									std::wstring const& remoteFile, bool download,
									CFileTransferCommand::t_transferSettings const& transferSettings)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::FileTransfer()"));

	if (localFile.empty()) {
		if (!download)
			ResetOperation(FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED);
		else
			ResetOperation(FZ_REPLY_SYNTAXERROR);
		return FZ_REPLY_ERROR;
	}

	if (download) {
		wxString filename = remotePath.FormatFilename(remoteFile);
		LogMessage(MessageType::Status, _("Starting download of %s"), filename);
	}
	else {
		LogMessage(MessageType::Status, _("Starting upload of %s"), localFile);
	}
	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("deleting nonzero pData"));
		delete m_pCurOpData;
	}

	CFtpFileTransferOpData *pData = new CFtpFileTransferOpData(download, localFile, remoteFile, remotePath);
	m_pCurOpData = pData;

	pData->transferSettings = transferSettings;
	pData->binary = transferSettings.binary;

	int64_t size;
	bool isLink;
	if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file)
		pData->localFileSize = size;

	pData->opState = filetransfer_waitcwd;

	if (pData->remotePath.GetType() == DEFAULT)
		pData->remotePath.SetType(m_pCurrentServer->GetType());

	int res = ChangeDir(pData->remotePath);
	if (res != FZ_REPLY_OK)
		return res;

	return ParseSubcommandResult(FZ_REPLY_OK);
}

int CFtpControlSocket::FileTransferParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferParseResponse()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);
	if (pData->opState == filetransfer_init) {
		return FZ_REPLY_ERROR;
	}

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState)
	{
	case filetransfer_size:
		if (code != 2 && code != 3) {
			if (CServerCapabilities::GetCapability(*m_pCurrentServer, size_command) == yes ||
				!m_Response.Mid(4).CmpNoCase(_T("file not found")) ||
				(wxString(pData->remotePath.FormatFilename(pData->remoteFile)).Lower().Find(_T("file not found")) == -1 &&
				 m_Response.Lower().Find(_T("file not found")) != -1))
			{
				// Server supports SIZE command but command failed. Most likely MDTM will fail as well, so
				// skip it.
				pData->opState = filetransfer_resumetest;

				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
			else {
				pData->opState = filetransfer_mdtm;
			}
		}
		else {
			pData->opState = filetransfer_mdtm;
			if (m_Response.Left(4) == _T("213 ") && m_Response.Length() > 4) {
				if (CServerCapabilities::GetCapability(*m_pCurrentServer, size_command) == unknown) {
					CServerCapabilities::SetCapability(*m_pCurrentServer, size_command, yes);
				}
				wxString str = m_Response.Mid(4);
				int64_t size = 0;
				const wxChar *buf = str.c_str();
				while (*buf) {
					if (*buf < '0' || *buf > '9') {
						break;
					}

					size *= 10;
					size += *buf - '0';
					++buf;
				}
				pData->remoteFileSize = size;
			}
			else {
				LogMessage(MessageType::Debug_Info, _T("Invalid SIZE reply"));
			}
		}
		break;
	case filetransfer_mdtm:
		pData->opState = filetransfer_resumetest;
		if (m_Response.Left(4) == _T("213 ") && m_Response.Length() > 16) {
			pData->fileTime = fz::datetime(m_Response.Mid(4).ToStdWstring(), fz::datetime::utc);
			if (!pData->fileTime.empty()) {
				pData->fileTime += fz::duration::from_minutes(m_pCurrentServer->GetTimezoneOffset());
			}
		}

		{
			int res = CheckOverwriteFile();
			if (res != FZ_REPLY_OK) {
				return res;
			}
		}

		break;
	case filetransfer_mfmt:
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state"));
		error = true;
		break;
	}

	if (error) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CFtpControlSocket::FileTransferSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_waitcwd) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist)
					pData->opState = filetransfer_waitlist;
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
					CServerCapabilities::GetCapability(*m_pCurrentServer, mdtm_command) == yes)
				{
					pData->opState = filetransfer_mdtm;
				}
				else
					pData->opState = filetransfer_resumetest;
			}
			else {
				if (entry.is_unsure())
					pData->opState = filetransfer_waitlist;
				else {
					if (matchedCase) {
						pData->remoteFileSize = entry.size;
						if (entry.has_date())
							pData->fileTime = entry.time;

						if (pData->download &&
							!entry.has_time() &&
							engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
							CServerCapabilities::GetCapability(*m_pCurrentServer, mdtm_command) == yes)
						{
							pData->opState = filetransfer_mdtm;
						}
						else
							pData->opState = filetransfer_resumetest;
					}
					else
						pData->opState = filetransfer_size;
				}
			}
			if (pData->opState == filetransfer_waitlist) {
				int res = List(CServerPath(), _T(""), LIST_FLAG_REFRESH);
				if (res != FZ_REPLY_OK)
					return res;
				ResetOperation(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}
			else if (pData->opState == filetransfer_resumetest) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK)
					return res;
			}
		}
		else {
			pData->tryAbsolutePath = true;
			pData->opState = filetransfer_size;
		}
	}
	else if (pData->opState == filetransfer_waitlist) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist)
					pData->opState = filetransfer_size;
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
					CServerCapabilities::GetCapability(*m_pCurrentServer, mdtm_command) == yes)
				{
					pData->opState = filetransfer_mdtm;
				}
				else
					pData->opState = filetransfer_resumetest;
			}
			else {
				if (matchedCase && !entry.is_unsure()) {
					pData->remoteFileSize = entry.size;
					if (entry.has_date())
						pData->fileTime = entry.time;

					if (pData->download &&
						!entry.has_time() &&
						engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
						CServerCapabilities::GetCapability(*m_pCurrentServer, mdtm_command) == yes)
					{
						pData->opState = filetransfer_mdtm;
					}
					else
						pData->opState = filetransfer_resumetest;
				}
				else
					pData->opState = filetransfer_size;
			}

			if (pData->opState == filetransfer_resumetest) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK)
					return res;
			}
		}
		else
			pData->opState = filetransfer_size;
	}
	else if (pData->opState == filetransfer_waittransfer) {
		if (prevResult == FZ_REPLY_OK && engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS)) {
			if (!pData->download &&
				CServerCapabilities::GetCapability(*m_pCurrentServer, mfmt_command) == yes)
			{
				fz::datetime mtime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));
				if (!mtime.empty()) {
					pData->fileTime = mtime;
					pData->opState = filetransfer_mfmt;
					return SendNextCommand();
				}
			}
			else if (pData->download && !pData->fileTime.empty()) {
				delete pData->pIOThread;
				pData->pIOThread = 0;
				if (!fz::local_filesys::set_modification_time(fz::to_native(pData->localFile), pData->fileTime))
					LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Could not set modification time"));
			}
		}
		ResetOperation(prevResult);
		return prevResult;
	}
	else if (pData->opState == filetransfer_waitresumetest) {
		if (prevResult != FZ_REPLY_OK) {
			if (pData->transferEndReason == TransferEndReason::failed_resumetest) {
				if (pData->localFileSize > (1ll << 32)) {
					CServerCapabilities::SetCapability(*m_pCurrentServer, resume4GBbug, yes);
					LogMessage(MessageType::Error, _("Server does not support resume of files > 4GB."));
				}
				else {
					CServerCapabilities::SetCapability(*m_pCurrentServer, resume2GBbug, yes);
					LogMessage(MessageType::Error, _("Server does not support resume of files > 2GB."));
				}

				ResetOperation(prevResult | FZ_REPLY_CRITICALERROR);
				return FZ_REPLY_ERROR;
			}
			else
				ResetOperation(prevResult);
			return prevResult;
		}
		if (pData->localFileSize > (1ll << 32))
			CServerCapabilities::SetCapability(*m_pCurrentServer, resume4GBbug, no);
		else
			CServerCapabilities::SetCapability(*m_pCurrentServer, resume2GBbug, no);

		pData->opState = filetransfer_transfer;
	}

	return SendNextCommand();
}

int CFtpControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	wxString cmd;
	switch (pData->opState)
	{
	case filetransfer_size:
		cmd = _T("SIZE ");
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);
		break;
	case filetransfer_mdtm:
		cmd = _T("MDTM ");
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);
		break;
	case filetransfer_resumetest:
	case filetransfer_transfer:
		if (m_pTransferSocket)
		{
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Verbose, _T("m_pTransferSocket != 0"));
			delete m_pTransferSocket;
			m_pTransferSocket = 0;
		}

		{
			auto pFile = std::make_unique<fz::file>();
			if (pData->download) {
				int64_t startOffset = 0;

				// Potentially racy
				bool didExist = fz::local_filesys::get_file_type(fz::to_native(pData->localFile)) != fz::local_filesys::unknown;

				if (pData->resume) {
					if (!pFile->open(fz::to_native(pData->localFile), fz::file::writing, fz::file::existing)) {
						LogMessage(MessageType::Error, _("Failed to open \"%s\" for appending/writing"), pData->localFile);
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					pData->fileDidExist = didExist;

					startOffset = pFile->seek(0, fz::file::end);

					if (startOffset == wxInvalidOffset) {
						LogMessage(MessageType::Error, _("Could not seek to the end of the file"));
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}
					pData->localFileSize = startOffset;

					// Check resume capabilities
					if (pData->opState == filetransfer_resumetest) {
						int res = FileTransferTestResumeCapability();
						if ((res & FZ_REPLY_CANCELED) == FZ_REPLY_CANCELED) {
							// Server does not support resume but remote and local filesizes are equal
							return FZ_REPLY_OK;
						}
						if (res != FZ_REPLY_OK) {
							return res;
						}
					}
				}
				else {
					CreateLocalDir(pData->localFile);

					if (!pFile->open(fz::to_native(pData->localFile), fz::file::writing, fz::file::empty)) {
						LogMessage(MessageType::Error, _("Failed to open \"%s\" for writing"), pData->localFile);
						ResetOperation(FZ_REPLY_ERROR);
						return FZ_REPLY_ERROR;
					}

					pData->fileDidExist = didExist;
					pData->localFileSize = 0;
				}

				if (pData->resume)
					pData->resumeOffset = pData->localFileSize;
				else
					pData->resumeOffset = 0;

				engine_.transfer_status_.Init(pData->remoteFileSize, startOffset, false);

				if (engine_.GetOptions().GetOptionVal(OPTION_PREALLOCATE_SPACE)) {
					// Try to preallocate the file in order to reduce fragmentation
					int64_t sizeToPreallocate = pData->remoteFileSize - startOffset;
					if (sizeToPreallocate > 0) {
						LogMessage(MessageType::Debug_Info, _T("Preallocating %d bytes for the file \"%s\""), sizeToPreallocate, pData->localFile);
						auto oldPos = pFile->seek(0, fz::file::current);
						if (oldPos >= 0) {
							if (pFile->seek(sizeToPreallocate, fz::file::end) == pData->remoteFileSize) {
								if (!pFile->truncate())
									LogMessage(MessageType::Debug_Warning, _T("Could not preallocate the file"));
							}
							pFile->seek(oldPos, fz::file::begin);
						}
					}
				}
			}
			else {
				if (!pFile->open(fz::to_native(pData->localFile), fz::file::reading)) {
					LogMessage(MessageType::Error, _("Failed to open \"%s\" for reading"), pData->localFile);
					ResetOperation(FZ_REPLY_ERROR);
					return FZ_REPLY_ERROR;
				}

				int64_t startOffset;
				if (pData->resume) {
					if (pData->remoteFileSize > 0) {
						startOffset = pData->remoteFileSize;

						if (pData->localFileSize < 0) {
							auto s = pFile->size();
							if (s >= 0) {
								pData->localFileSize = s;
							}
						}

						if (startOffset == pData->localFileSize && pData->binary) {
							LogMessage(MessageType::Debug_Info, _T("No need to resume, remote file size matches local file size."));

							if (engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS) &&
								CServerCapabilities::GetCapability(*m_pCurrentServer, mfmt_command) == yes)
							{
								fz::datetime mtime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));
								if (!mtime.empty()) {
									pData->fileTime = mtime;
									pData->opState = filetransfer_mfmt;
									return SendNextCommand();
								}
							}
							ResetOperation(FZ_REPLY_OK);
							return FZ_REPLY_OK;
						}

						// Assume native 64 bit type exists
						if (pFile->seek(startOffset, fz::file::begin) == wxInvalidOffset) {
							wxString const s = std::to_wstring(startOffset);
							LogMessage(MessageType::Error, _("Could not seek to offset %s within file"), s);
							ResetOperation(FZ_REPLY_ERROR);
							return FZ_REPLY_ERROR;
						}
					}
					else
						startOffset = 0;
				}
				else
					startOffset = 0;

				if (CServerCapabilities::GetCapability(*m_pCurrentServer, rest_stream) == yes) {
					// Use REST + STOR if resuming
					pData->resumeOffset = startOffset;
				}
				else {
					// Play it safe, use APPE if resuming
					pData->resumeOffset = 0;
				}

				auto len = pFile->size();
				engine_.transfer_status_.Init(len, startOffset, false);
			}
			pData->pIOThread = new CIOThread;
			if (!pData->pIOThread->Create(engine_.GetThreadPool(), std::move(pFile), !pData->download, pData->binary)) {
				// CIOThread will delete pFile
				delete pData->pIOThread;
				pData->pIOThread = 0;
				LogMessage(MessageType::Error, _("Could not spawn IO thread"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		m_pTransferSocket = new CTransferSocket(engine_, *this, pData->download ? TransferMode::download : TransferMode::upload);
		m_pTransferSocket->m_binaryMode = pData->transferSettings.binary;
		m_pTransferSocket->SetIOThread(pData->pIOThread);

		if (pData->download)
			cmd = _T("RETR ");
		else if (pData->resume) {
			if (CServerCapabilities::GetCapability(*m_pCurrentServer, rest_stream) == yes)
				cmd = _T("STOR "); // In this case REST gets sent since resume offset was set earlier
			else {
				wxASSERT(pData->resumeOffset == 0);
				cmd = _T("APPE ");
			}
		}
		else
			cmd = _T("STOR ");
		cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);

		pData->opState = filetransfer_waittransfer;
		return Transfer(cmd.ToStdWstring(), pData);
	case filetransfer_mfmt:
		{
			cmd = _T("MFMT ");
			fz::datetime t = pData->fileTime;
			t -= fz::duration::from_minutes(m_pCurrentServer->GetTimezoneOffset());
			cmd += t.format(_T("%Y%m%d%H%M%S "), fz::datetime::utc);
			cmd += pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath);

			break;
		}
	default:
		LogMessage(MessageType::Debug_Warning, _T("Unhandled opState: %d"), pData->opState);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (!cmd.empty())
		if (!SendCommand(cmd))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

void CFtpControlSocket::TransferEnd()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::TransferEnd()"));

	// If m_pTransferSocket is zero, the message was sent by the previous command.
	// We can safely ignore it.
	// It does not cause problems, since before creating the next transfer socket, other
	// messages which were added to the queue later than this one will be processed first.
	if (!m_pCurOpData || !m_pTransferSocket || GetCurrentCommandId() != Command::rawtransfer)
	{
		LogMessage(MessageType::Debug_Verbose, _T("Call to TransferEnd at unusual time, ignoring"));
		return;
	}

	TransferEndReason reason = m_pTransferSocket->GetTransferEndreason();
	if (reason == TransferEndReason::none)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Call to TransferEnd at unusual time"));
		return;
	}

	if (reason == TransferEndReason::successful)
		SetAlive();

	CRawTransferOpData *pData = static_cast<CRawTransferOpData *>(m_pCurOpData);
	if (pData->pOldData->transferEndReason == TransferEndReason::successful)
		pData->pOldData->transferEndReason = reason;

	switch (m_pCurOpData->opState)
	{
	case rawtransfer_transfer:
		pData->opState = rawtransfer_waittransferpre;
		break;
	case rawtransfer_waitfinish:
		pData->opState = rawtransfer_waittransfer;
		break;
	case rawtransfer_waitsocket:
		ResetOperation((reason == TransferEndReason::successful) ? FZ_REPLY_OK : FZ_REPLY_ERROR);
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("TransferEnd at unusual op state, ignoring"));
		break;
	}
}

bool CFtpControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (m_pCurOpData)
	{
		if (!m_pCurOpData->waitForAsyncRequest)
		{
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Not waiting for request reply, ignoring request reply %d"), pNotification->GetRequestID());
			return false;
		}
		m_pCurOpData->waitForAsyncRequest = false;
	}

	const RequestId requestId = pNotification->GetRequestID();
	switch (requestId)
	{
	case reqId_fileexists:
		{
                        if (!m_pCurOpData || m_pCurOpData->opId != Command::transfer) {
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %f"), pNotification->GetRequestID());
				return false;
			}

			CFileExistsNotification *pFileExistsNotification = static_cast<CFileExistsNotification *>(pNotification);
			return SetFileExistsAction(pFileExistsNotification);
		}
		break;
	case reqId_interactiveLogin:
		{
			if (!m_pCurOpData || m_pCurOpData->opId != Command::connect)
			{
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %d"), pNotification->GetRequestID());
				return false;
			}

			CFtpLogonOpData* pData = static_cast<CFtpLogonOpData*>(m_pCurOpData);

			CInteractiveLoginNotification *pInteractiveLoginNotification = static_cast<CInteractiveLoginNotification *>(pNotification);
			if (!pInteractiveLoginNotification->passwordSet)
			{
				ResetOperation(FZ_REPLY_CANCELED);
				return false;
			}
			m_pCurrentServer->SetUser(m_pCurrentServer->GetUser(), pInteractiveLoginNotification->server.GetPass());
			pData->gotPassword = true;
			SendNextCommand();
		}
		break;
	case reqId_certificate:
		{
			if (!m_pTlsSocket || m_pTlsSocket->GetState() != CTlsSocket::TlsState::verifycert) {
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("No or invalid operation in progress, ignoring request reply %d"), pNotification->GetRequestID());
				return false;
			}

			CCertificateNotification* pCertificateNotification = static_cast<CCertificateNotification *>(pNotification);
			m_pTlsSocket->TrustCurrentCert(pCertificateNotification->m_trusted);

			if (!pCertificateNotification->m_trusted) {
				DoClose(FZ_REPLY_CRITICALERROR);
				return false;
			}

			if (m_pCurOpData && m_pCurOpData->opId == Command::connect &&
				m_pCurOpData->opState == LOGON_AUTH_WAIT)
			{
				m_pCurOpData->opState = LOGON_LOGON;
			}
		}
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown request %d"), pNotification->GetRequestID());
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return true;
}

class CRawCommandOpData : public COpData
{
public:
	CRawCommandOpData(std::wstring const& command)
		: COpData(Command::raw)
	{
		m_command = command;
	}

	std::wstring m_command;
};

int CFtpControlSocket::RawCommand(std::wstring const& command)
{
	wxASSERT(!command.empty());

	m_pCurOpData = new CRawCommandOpData(command);

	return SendNextCommand();
}

int CFtpControlSocket::RawCommandSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RawCommandSend"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().InvalidateServer(*m_pCurrentServer);
	engine_.GetPathCache().InvalidateServer(*m_pCurrentServer);
	m_CurrentPath.clear();

	m_lastTypeBinary = -1;

	CRawCommandOpData *pData = static_cast<CRawCommandOpData *>(m_pCurOpData);

	if (!SendCommand(pData->m_command, false, false)) {
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::RawCommandParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RawCommandParseResponse"));

	int code = GetReplyCode();
	if (code == 2 || code == 3) {
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}
	else {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
}

int CFtpControlSocket::Delete(const CServerPath& path, std::deque<std::wstring>&& files)
{
	wxASSERT(!m_pCurOpData);
	CFtpDeleteOpData *pData = new CFtpDeleteOpData();
	m_pCurOpData = pData;
	pData->path = path;
	pData->files = files;
	pData->omitPath = true;

	int res = ChangeDir(pData->path);
	if (res != FZ_REPLY_OK) {
		return res;
	}

	// CFileZillaEnginePrivate should have checked this already
	wxASSERT(!files.empty());

	return SendNextCommand();
}

int CFtpControlSocket::DeleteSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::DeleteSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	if (prevResult != FZ_REPLY_OK)
		pData->omitPath = false;

	return SendNextCommand();
}

int CFtpControlSocket::DeleteSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::DeleteSend"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}
	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	std::wstring const& file = pData->files.front();
	if (file.empty()) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty filename"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	std::wstring filename = pData->path.FormatFilename(file, pData->omitPath);
	if (filename.empty()) {
		LogMessage(MessageType::Error, _("Filename cannot be constructed for directory %s and filename %s"), pData->path.GetPath(), file);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->m_time.empty()) {
		pData->m_time = fz::datetime::now();
	}

	engine_.GetDirectoryCache().InvalidateFile(*m_pCurrentServer, pData->path, file);

	if (!SendCommand(L"DELE " + filename))
		return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::DeleteParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::DeleteParseResponse()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpDeleteOpData *pData = static_cast<CFtpDeleteOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	if (code != 2 && code != 3)
		pData->m_deleteFailed = true;
	else {
		const wxString& file = pData->files.front();

		engine_.GetDirectoryCache().RemoveFile(*m_pCurrentServer, pData->path, file);

		fz::datetime now = fz::datetime::now();
		if (!pData->m_time.empty() && (now - pData->m_time).get_seconds() >= 1) {
			engine_.SendDirectoryListingNotification(pData->path, false, true, false);
			pData->m_time = now;
			pData->m_needSendListing = false;
		}
		else
			pData->m_needSendListing = true;
	}

	pData->files.pop_front();

	if (!pData->files.empty())
		return SendNextCommand();

	return ResetOperation(pData->m_deleteFailed ? FZ_REPLY_ERROR : FZ_REPLY_OK);
}

class CFtpRemoveDirOpData : public COpData
{
public:
	CFtpRemoveDirOpData()
		: COpData(Command::removedir)
		, omitPath()
	{
	}

	virtual ~CFtpRemoveDirOpData() {}

	CServerPath path;
	CServerPath fullPath;
	std::wstring subDir;
	bool omitPath;
};

int CFtpControlSocket::RemoveDir(const CServerPath& path, std::wstring const& subDir)
{
	wxASSERT(!m_pCurOpData);
	CFtpRemoveDirOpData *pData = new CFtpRemoveDirOpData();
	m_pCurOpData = pData;
	pData->path = path;
	pData->subDir = subDir;
	pData->omitPath = true;
	pData->fullPath = path;

	if (!pData->fullPath.AddSegment(subDir)) {
		LogMessage(MessageType::Error, _("Path cannot be constructed for directory %s and subdir %s"), path.GetPath(), subDir);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	int res = ChangeDir(pData->path);
	if (res != FZ_REPLY_OK) {
		return res;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RemoveDirSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RemoveDirSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	if (prevResult != FZ_REPLY_OK) {
		pData->omitPath = false;
	}
	else {
		pData->path = m_CurrentPath;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RemoveDirSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RemoveDirSend()"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	engine_.GetDirectoryCache().InvalidateFile(*m_pCurrentServer, pData->path, pData->subDir);

	CServerPath path(engine_.GetPathCache().Lookup(*m_pCurrentServer, pData->path, pData->subDir));
	if (path.empty())
	{
		path = pData->path;
		path.AddSegment(pData->subDir);
	}
	engine_.InvalidateCurrentWorkingDirs(path);

	engine_.GetPathCache().InvalidatePath(*m_pCurrentServer, pData->path, pData->subDir);

	if (pData->omitPath)
	{
		if (!SendCommand(_T("RMD ") + pData->subDir))
			return FZ_REPLY_ERROR;
	}
	else
		if (!SendCommand(_T("RMD ") + pData->fullPath.GetPath()))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::RemoveDirParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RemoveDirParseResponse()"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpRemoveDirOpData *pData = static_cast<CFtpRemoveDirOpData *>(m_pCurOpData);

	int code = GetReplyCode();
	if (code != 2 && code != 3)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().RemoveDir(*m_pCurrentServer, pData->path, pData->subDir, engine_.GetPathCache().Lookup(*m_pCurrentServer, pData->path, pData->subDir));
	engine_.SendDirectoryListingNotification(pData->path, false, true, false);

	return ResetOperation(FZ_REPLY_OK);
}

enum mkdStates
{
	mkd_init = 0,
	mkd_findparent,
	mkd_mkdsub,
	mkd_cwdsub,
	mkd_tryfull
};

int CFtpControlSocket::Mkdir(const CServerPath& path)
{
	/* Directory creation works like this: First find a parent directory into
	 * which we can CWD, then create the subdirs one by one. If either part
	 * fails, try MKD with the full path directly.
	 */

	if (!m_pCurOpData)
		LogMessage(MessageType::Status, _("Creating directory '%s'..."), path.GetPath());

	CMkdirOpData *pData = new CMkdirOpData;
	pData->path = path;

	if (!m_CurrentPath.empty()) {
		// Unless the server is broken, a directory already exists if current directory is a subdir of it.
		if (m_CurrentPath == path || m_CurrentPath.IsSubdirOf(path, false)) {
			delete pData;
			return FZ_REPLY_OK;
		}

		if (m_CurrentPath.IsParentOf(path, false))
			pData->commonParent = m_CurrentPath;
		else
			pData->commonParent = path.GetCommonParent(m_CurrentPath);
	}

	if (!path.HasParent())
		pData->opState = mkd_tryfull;
	else {
		pData->currentPath = path.GetParent();
		pData->segments.push_back(path.GetLastSegment());

		if (pData->currentPath == m_CurrentPath)
			pData->opState = mkd_mkdsub;
		else
			pData->opState = mkd_findparent;
	}

	pData->pNextOpData = m_pCurOpData;
	m_pCurOpData = pData;

	return SendNextCommand();
}

int CFtpControlSocket::MkdirParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::MkdirParseResonse"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CMkdirOpData *pData = static_cast<CMkdirOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	int code = GetReplyCode();
	bool error = false;
	switch (pData->opState) {
	case mkd_findparent:
		if (code == 2 || code == 3) {
			m_CurrentPath = pData->currentPath;
			pData->opState = mkd_mkdsub;
		}
		else if (pData->currentPath == pData->commonParent)
			pData->opState = mkd_tryfull;
		else if (pData->currentPath.HasParent()) {
			const CServerPath& parent = pData->currentPath.GetParent();
			pData->segments.push_back(pData->currentPath.GetLastSegment());
			pData->currentPath = parent;
		}
		else
			pData->opState = mkd_tryfull;
		break;
	case mkd_mkdsub:
		if (code != 2 && code != 3) {
			// Don't fall back to using the full path if the error message
			// is "already exists".
			// Case 1: Full response a known "already exists" message.
			// Case 2: Substrng of response contains "already exists". pData->path may not
			//         contain this substring as the path might be returned in the reply.
			// Case 3: Substrng of response contains "file exists". pData->path may not
			//         contain this substring as the path might be returned in the reply.
			wxString const response = m_Response.Mid(4).Lower();
			wxString const path = wxString(pData->path.GetPath()).Lower();
			if (response != _T("directory already exists") &&
				(path.Find(_T("already exists")) != -1 ||
				 response.Find(_T("already exists")) == -1) &&
				(path.Find(_T("file exists")) != -1 ||
				 response.Find(_T("file exists")) == -1)
				)
			{
				pData->opState = mkd_tryfull;
				break;
			}
		}

		{
			if (pData->segments.empty()) {
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("  pData->segments is empty"));
				ResetOperation(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}

			// If entry did exist and is a file instead of a directory, report failure.
			int result = FZ_REPLY_OK;
			if (code != 2 && code != 3) {
				CDirentry entry;
				bool tmp;
				if (engine_.GetDirectoryCache().LookupFile(entry, *m_pCurrentServer, pData->currentPath, pData->segments.back(), tmp, tmp) && !entry.is_dir()) {
					result = FZ_REPLY_ERROR;
				}
			}

			engine_.GetDirectoryCache().UpdateFile(*m_pCurrentServer, pData->currentPath, pData->segments.back(), true, CDirectoryCache::dir);
			engine_.SendDirectoryListingNotification(pData->currentPath, false, true, false);

			pData->currentPath.AddSegment(pData->segments.back());
			pData->segments.pop_back();

			if (pData->segments.empty() || result != FZ_REPLY_OK) {
				ResetOperation(result);
				return result;
			}
			else
				pData->opState = mkd_cwdsub;
		}
		break;
	case mkd_cwdsub:
		if (code == 2 || code == 3) {
			m_CurrentPath = pData->currentPath;
			pData->opState = mkd_mkdsub;
		}
		else
			pData->opState = mkd_tryfull;
		break;
	case mkd_tryfull:
		if (code != 2 && code != 3)
			error = true;
		else {
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (error) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CFtpControlSocket::MkdirSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::MkdirSend"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CMkdirOpData *pData = static_cast<CMkdirOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (!pData->holdsLock) {
		if (!TryLockCache(lock_mkdir, pData->path))
			return FZ_REPLY_WOULDBLOCK;
	}

	bool res;
	switch (pData->opState)
	{
	case mkd_findparent:
	case mkd_cwdsub:
		m_CurrentPath.clear();
		res = SendCommand(_T("CWD ") + pData->currentPath.GetPath());
		break;
	case mkd_mkdsub:
		res = SendCommand(_T("MKD ") + pData->segments.back());
		break;
	case mkd_tryfull:
		res = SendCommand(_T("MKD ") + pData->path.GetPath());
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res)
		return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

class CFtpRenameOpData : public COpData
{
public:
	CFtpRenameOpData(CRenameCommand const& command)
		: COpData(Command::rename), m_cmd(command)
	{
		m_useAbsolute = false;
	}

	virtual ~CFtpRenameOpData() {}

	CRenameCommand m_cmd;
	bool m_useAbsolute;
};

enum renameStates
{
	rename_init = 0,
	rename_rnfrom,
	rename_rnto
};

int CFtpControlSocket::Rename(CRenameCommand const& command)
{
	if (m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData not empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	LogMessage(MessageType::Status, _("Renaming '%s' to '%s'"), command.GetFromPath().FormatFilename(command.GetFromFile()), command.GetToPath().FormatFilename(command.GetToFile()));

	CFtpRenameOpData *pData = new CFtpRenameOpData(command);
	pData->opState = rename_rnfrom;
	m_pCurOpData = pData;

	int res = ChangeDir(command.GetFromPath());
	if (res != FZ_REPLY_OK)
		return res;

	return SendNextCommand();
}

int CFtpControlSocket::RenameParseResponse()
{
	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	int code = GetReplyCode();
	if (code != 2 && code != 3)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->opState == rename_rnfrom)
		pData->opState = rename_rnto;
	else
	{
		const CServerPath& fromPath = pData->m_cmd.GetFromPath();
		const CServerPath& toPath = pData->m_cmd.GetToPath();
		engine_.GetDirectoryCache().Rename(*m_pCurrentServer, fromPath, pData->m_cmd.GetFromFile(), toPath, pData->m_cmd.GetToFile());

		engine_.SendDirectoryListingNotification(fromPath, false, true, false);
		if (fromPath != toPath)
			engine_.SendDirectoryListingNotification(toPath, false, true, false);

		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}

	return SendNextCommand();
}

int CFtpControlSocket::RenameSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RenameSubcommandResult()"));

	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK)
		pData->m_useAbsolute = true;

	return SendNextCommand();
}

int CFtpControlSocket::RenameSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::RenameSend()"));

	CFtpRenameOpData *pData = static_cast<CFtpRenameOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case rename_rnfrom:
		res = SendCommand(_T("RNFR ") + pData->m_cmd.GetFromPath().FormatFilename(pData->m_cmd.GetFromFile(), !pData->m_useAbsolute));
		break;
	case rename_rnto:
		{
			engine_.GetDirectoryCache().InvalidateFile(*m_pCurrentServer, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
			engine_.GetDirectoryCache().InvalidateFile(*m_pCurrentServer, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			CServerPath path(engine_.GetPathCache().Lookup(*m_pCurrentServer, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile()));
			if (path.empty()) {
				path = pData->m_cmd.GetFromPath();
				path.AddSegment(pData->m_cmd.GetFromFile());
			}
			engine_.InvalidateCurrentWorkingDirs(path);

			engine_.GetPathCache().InvalidatePath(*m_pCurrentServer, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
			engine_.GetPathCache().InvalidatePath(*m_pCurrentServer, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			res = SendCommand(_T("RNTO ") + pData->m_cmd.GetToPath().FormatFilename(pData->m_cmd.GetToFile(), !pData->m_useAbsolute && pData->m_cmd.GetFromPath() == pData->m_cmd.GetToPath()));
			break;
		}
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

class CFtpChmodOpData : public COpData
{
public:
	CFtpChmodOpData(const CChmodCommand& command)
		: COpData(Command::chmod), m_cmd(command)
	{
		m_useAbsolute = false;
	}

	virtual ~CFtpChmodOpData() {}

	CChmodCommand m_cmd;
	bool m_useAbsolute;
};

enum chmodStates
{
	chmod_init = 0,
	chmod_chmod
};

int CFtpControlSocket::Chmod(CChmodCommand const& command)
{
	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData not empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	LogMessage(MessageType::Status, _("Set permissions of '%s' to '%s'"), command.GetPath().FormatFilename(command.GetFile()), command.GetPermission());

	CFtpChmodOpData *pData = new CFtpChmodOpData(command);
	pData->opState = chmod_chmod;
	m_pCurOpData = pData;

	int res = ChangeDir(command.GetPath());
	if (res != FZ_REPLY_OK)
		return res;

	return SendNextCommand();
}

int CFtpControlSocket::ChmodParseResponse()
{
	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	int code = GetReplyCode();
	if (code != 2 && code != 3) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().UpdateFile(*m_pCurrentServer, pData->m_cmd.GetPath(), pData->m_cmd.GetFile(), false, CDirectoryCache::unknown);

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

int CFtpControlSocket::ChmodSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ChmodSubcommandResult()"));

	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK)
		pData->m_useAbsolute = true;

	return SendNextCommand();
}

int CFtpControlSocket::ChmodSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ChmodSend()"));

	CFtpChmodOpData *pData = static_cast<CFtpChmodOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case chmod_chmod:
		res = SendCommand(_T("SITE CHMOD ") + pData->m_cmd.GetPermission() + _T(" ") + pData->m_cmd.GetPath().FormatFilename(pData->m_cmd.GetFile(), !pData->m_useAbsolute));
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

bool CFtpControlSocket::IsMisleadingListResponse() const
{
	// Some servers are broken. Instead of an empty listing, some MVS servers
	// for example they return "550 no members found"
	// Other servers return "550 No files found."

	if (!m_Response.CmpNoCase(_T("550 No members found."))) {
		return true;
	}

	if (!m_Response.CmpNoCase(_T("550 No data sets found."))) {
		return true;
	}

	if (!m_Response.CmpNoCase(_T("550 No files found."))) {
		return true;
	}

	return false;
}

bool CFtpControlSocket::ParseEpsvResponse(CRawTransferOpData* pData)
{
	int pos = m_Response.Find(_T("(|||"));
	if (pos == -1) {
		return false;
	}

	int pos2 = m_Response.Mid(pos + 4).Find(_T("|)"));
	if (pos2 <= 0) {
		return false;
	}

	wxString number = m_Response.Mid(pos + 4, pos2);
	unsigned long port;
	if (!number.ToULong(&port)) {
		return false;
	}

	if (port == 0 || port > 65535) {
		return false;
	}

	pData->port = port;

	if (m_pProxyBackend) {
		pData->host = m_pCurrentServer->GetHost();
	}
	else {
		pData->host = fz::to_wstring(m_pSocket->GetPeerIP());
	}
	return true;
}

bool CFtpControlSocket::ParsePasvResponse(CRawTransferOpData* pData)
{
	// Validate ip address
	if (!m_pasvReplyRegex) {
		std::wstring digit = _T("0*[0-9]{1,3}");
		wchar_t const* const  dot = _T(",");
		std::wstring exp = _T("( |\\()(") + digit + dot + digit + dot + digit + dot + digit + dot + digit + dot + digit + _T(")( |\\)|$)");
		m_pasvReplyRegex = std::make_unique<std::wregex>(exp);
	}

	std::wsmatch m;
	if (!std::regex_search(m_Response.ToStdWstring(), m, *m_pasvReplyRegex)) {
		return false;
	}

	pData->host = m[2].str();

	size_t i = pData->host.rfind(',');
	if (i == std::wstring::npos) {
		return false;
	}
	auto number = fz::to_integral<unsigned int>(pData->host.substr(i + 1));
	if (number > 255) {
		return false;
	}

	pData->port = number; //get ls byte of server socket
	pData->host = pData->host.substr(0, i);
	i = pData->host.rfind(',');
	if (i == std::string::npos) {
		return false;
	}
	number = fz::to_integral<unsigned int>(pData->host.substr(i + 1));
	if (number > 255) {
		return false;
	}

	pData->port += 256 * number; //add ms byte of server socket
	pData->host = pData-> host.substr(0, i);
	fz::replace_substrings(pData->host, L",", L".");

	if (m_pProxyBackend) {
		// We do not have any information about the proxy's inner workings
		return true;
	}

	std::wstring const peerIP = fz::to_wstring(m_pSocket->GetPeerIP());
	if (!fz::is_routable_address(pData->host) && fz::is_routable_address(peerIP)) {
		if (engine_.GetOptions().GetOptionVal(OPTION_PASVREPLYFALLBACKMODE) != 1 || pData->bTriedActive) {
			LogMessage(MessageType::Status, _("Server sent passive reply with unroutable address. Using server address instead."));
			LogMessage(MessageType::Debug_Info, _T("  Reply: %s, peer: %s"), pData->host, peerIP);
			pData->host = peerIP;
		}
		else {
			LogMessage(MessageType::Status, _("Server sent passive reply with unroutable address. Passive mode failed."));
			LogMessage(MessageType::Debug_Info, _T("  Reply: %s, peer: %s"), pData->host, peerIP);
			return false;
		}
	}
	else if (engine_.GetOptions().GetOptionVal(OPTION_PASVREPLYFALLBACKMODE) == 2) {
		// Always use server address
		pData->host = peerIP;
	}

	return true;
}

int CFtpControlSocket::GetExternalIPAddress(std::string& address)
{
	// Local IP should work. Only a complete moron would use IPv6
	// and NAT at the same time.
	if (m_pSocket->GetAddressFamily() != CSocket::ipv6) {
		int mode = engine_.GetOptions().GetOptionVal(OPTION_EXTERNALIPMODE);

		if (mode) {
			if (engine_.GetOptions().GetOptionVal(OPTION_NOEXTERNALONLOCAL) &&
				!fz::is_routable_address(m_pSocket->GetPeerIP()))
				// Skip next block, use local address
				goto getLocalIP;
		}

		if (mode == 1) {
			wxString ip = engine_.GetOptions().GetOption(OPTION_EXTERNALIP);
			if (!ip.empty()) {
				address = ip;
				return FZ_REPLY_OK;
			}

			LogMessage(MessageType::Debug_Warning, _("No external IP address set, trying default."));
		}
		else if (mode == 2) {
			if (!m_pIPResolver) {
				std::string localAddress = m_pSocket->GetLocalIP(true);

				if (!localAddress.empty() && localAddress == engine_.GetOptions().GetOption(OPTION_LASTRESOLVEDIP)) {
					LogMessage(MessageType::Debug_Verbose, _T("Using cached external IP address"));

					address = localAddress;
					return FZ_REPLY_OK;
				}

				std::wstring resolverAddress = engine_.GetOptions().GetOption(OPTION_EXTERNALIPRESOLVER);

				LogMessage(MessageType::Debug_Info, _("Retrieving external IP address from %s"), resolverAddress);

				m_pIPResolver = new CExternalIPResolver(engine_.GetThreadPool(), *this);
				m_pIPResolver->GetExternalIP(resolverAddress, CSocket::ipv4);
				if (!m_pIPResolver->Done()) {
					LogMessage(MessageType::Debug_Verbose, _T("Waiting for resolver thread"));
					return FZ_REPLY_WOULDBLOCK;
				}
			}
			if (!m_pIPResolver->Successful()) {
				delete m_pIPResolver;
				m_pIPResolver = 0;

				LogMessage(MessageType::Debug_Warning, _("Failed to retrieve external ip address, using local address"));
			}
			else {
				LogMessage(MessageType::Debug_Info, _T("Got external IP address"));
				address = m_pIPResolver->GetIP();

				engine_.GetOptions().SetOption(OPTION_LASTRESOLVEDIP, fz::to_wstring(address));

				delete m_pIPResolver;
				m_pIPResolver = 0;

				return FZ_REPLY_OK;
			}
		}
	}

getLocalIP:
	address = m_pSocket->GetLocalIP(true);
	if (address.empty()) {
		LogMessage(MessageType::Error, _("Failed to retrieve local ip address."), 1);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_OK;
}

void CFtpControlSocket::OnExternalIPAddress()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::OnExternalIPAddress()"));
	if (!m_pIPResolver) {
		LogMessage(MessageType::Debug_Info, _T("Ignoring event"));
		return;
	}

	SendNextCommand();
}

int CFtpControlSocket::Transfer(std::wstring const& cmd, CFtpTransferOpData* oldData)
{
	assert(oldData);
	oldData->tranferCommandSent = false;

	CRawTransferOpData *pData = new CRawTransferOpData;
	pData->pNextOpData = m_pCurOpData;
	m_pCurOpData = pData;

	pData->cmd = cmd;
	pData->pOldData = oldData;
	pData->pOldData->transferEndReason = TransferEndReason::successful;

	if (m_pProxyBackend) {
		// Only passive suported
		// Theoretically could use reverse proxy ability in SOCKS5, but
		// it is too fragile to set up with all those broken routers and
		// firewalls sabotaging connections. Regular active mode is hard
		// enough already
		pData->bPasv = true;
		pData->bTriedActive = true;
	}
	else {
		switch (m_pCurrentServer->GetPasvMode())
		{
		case MODE_PASSIVE:
			pData->bPasv = true;
			break;
		case MODE_ACTIVE:
			pData->bPasv = false;
			break;
		default:
			pData->bPasv = engine_.GetOptions().GetOptionVal(OPTION_USEPASV) != 0;
			break;
		}
	}

	if ((pData->pOldData->binary && m_lastTypeBinary == 1) ||
		(!pData->pOldData->binary && m_lastTypeBinary == 0))
	{
		pData->opState = rawtransfer_port_pasv;
	}
	else {
		pData->opState = rawtransfer_type;
	}

	return SendNextCommand();
}

int CFtpControlSocket::TransferParseResponse()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::TransferParseResponse()"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CRawTransferOpData *pData = static_cast<CRawTransferOpData *>(m_pCurOpData);
	if (pData->opState == rawtransfer_init)
		return FZ_REPLY_ERROR;

	int code = GetReplyCode();

	LogMessage(MessageType::Debug_Debug, _T("  code = %d"), code);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	bool error = false;
	switch (pData->opState)
	{
	case rawtransfer_type:
		if (code != 2 && code != 3)
			error = true;
		else
		{
			pData->opState = rawtransfer_port_pasv;
			m_lastTypeBinary = pData->pOldData->binary ? 1 : 0;
		}
		break;
	case rawtransfer_port_pasv:
		if (code != 2 && code != 3)
		{
			if (!engine_.GetOptions().GetOptionVal(OPTION_ALLOW_TRANSFERMODEFALLBACK))
			{
				error = true;
				break;
			}

			if (pData->bTriedPasv)
				if (pData->bTriedActive)
					error = true;
				else
					pData->bPasv = false;
			else
				pData->bPasv = true;
			break;
		}
		if (pData->bPasv) {
			bool parsed;
			if (GetPassiveCommand(*pData) == _T("EPSV"))
				parsed = ParseEpsvResponse(pData);
			else
				parsed = ParsePasvResponse(pData);
			if (!parsed) {
				if (!engine_.GetOptions().GetOptionVal(OPTION_ALLOW_TRANSFERMODEFALLBACK)) {
					error = true;
					break;
				}

				if (!pData->bTriedActive)
					pData->bPasv = false;
				else
					error = true;
				break;
			}
		}
		if (pData->pOldData->resumeOffset > 0 || m_sentRestartOffset)
			pData->opState = rawtransfer_rest;
		else
			pData->opState = rawtransfer_transfer;
		break;
	case rawtransfer_rest:
		if (pData->pOldData->resumeOffset <= 0)
			m_sentRestartOffset = false;
		if (pData->pOldData->resumeOffset > 0 && code != 2 && code != 3)
			error = true;
		else
			pData->opState = rawtransfer_transfer;
		break;
	case rawtransfer_transfer:
		if (code == 1) {
			pData->opState = rawtransfer_waitfinish;
		}
		else if (code == 2 || code == 3) {
			// A few broken servers omit the 1yz reply.
			pData->opState = rawtransfer_waitsocket;
		}
		else {
			if (pData->pOldData->transferEndReason == TransferEndReason::successful)
				pData->pOldData->transferEndReason = TransferEndReason::transfer_command_failure_immediate;
			error = true;
		}
		break;
	case rawtransfer_waittransferpre:
		if (code == 1) {
			pData->opState = rawtransfer_waittransfer;
		}
		else if (code == 2 || code == 3) {
			// A few broken servers omit the 1yz reply.
			if (pData->pOldData->transferEndReason != TransferEndReason::successful) {
				error = true;
				break;
			}

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else {
			if (pData->pOldData->transferEndReason == TransferEndReason::successful)
				pData->pOldData->transferEndReason = TransferEndReason::transfer_command_failure_immediate;
			error = true;
		}
		break;
	case rawtransfer_waitfinish:
		if (code != 2 && code != 3) {
			if (pData->pOldData->transferEndReason == TransferEndReason::successful)
				pData->pOldData->transferEndReason = TransferEndReason::transfer_command_failure;
			error = true;
		}
		else
			pData->opState = rawtransfer_waitsocket;
		break;
	case rawtransfer_waittransfer:
		if (code != 2 && code != 3) {
			if (pData->pOldData->transferEndReason == TransferEndReason::successful)
				pData->pOldData->transferEndReason = TransferEndReason::transfer_command_failure;
			error = true;
		}
		else {
			if (pData->pOldData->transferEndReason != TransferEndReason::successful) {
				error = true;
				break;
			}

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		break;
	case rawtransfer_waitsocket:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Extra reply received during rawtransfer_waitsocket."));
		error = true;
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state"));
		error = true;
	}
	if (error)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CFtpControlSocket::TransferSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::TransferSend()"));

	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!m_pTransferSocket)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pTransferSocket"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CRawTransferOpData *pData = static_cast<CRawTransferOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	wxString cmd;
	bool measureRTT = false;
	switch (pData->opState)
	{
	case rawtransfer_type:
		m_lastTypeBinary = -1;
		if (pData->pOldData->binary)
			cmd = _T("TYPE I");
		else
			cmd = _T("TYPE A");
		measureRTT = true;
		break;
	case rawtransfer_port_pasv:
		if (pData->bPasv) {
			cmd = GetPassiveCommand(*pData);
		}
		else {
			std::string address;
			int res = GetExternalIPAddress(address);
			if (res == FZ_REPLY_WOULDBLOCK)
				return res;
			else if (res == FZ_REPLY_OK) {
				std::wstring portArgument = m_pTransferSocket->SetupActiveTransfer(address);
				if (!portArgument.empty()) {
					pData->bTriedActive = true;
					if (m_pSocket->GetAddressFamily() == CSocket::ipv6)
						cmd = _T("EPRT " + portArgument);
					else
						cmd = _T("PORT " + portArgument);
					break;
				}
			}

			if (!engine_.GetOptions().GetOptionVal(OPTION_ALLOW_TRANSFERMODEFALLBACK) || pData->bTriedPasv) {
				LogMessage(MessageType::Error, _("Failed to create listening socket for active mode transfer"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
			LogMessage(MessageType::Debug_Warning, _("Failed to create listening socket for active mode transfer"));
			pData->bTriedActive = true;
			pData->bPasv = true;
			cmd = GetPassiveCommand(*pData);
		}
		break;
	case rawtransfer_rest:
		cmd = _T("REST ") + std::to_wstring(pData->pOldData->resumeOffset);
		if (pData->pOldData->resumeOffset > 0)
			m_sentRestartOffset = true;
		measureRTT = true;
		break;
	case rawtransfer_transfer:
		if (pData->bPasv)
		{
			if (!m_pTransferSocket->SetupPassiveTransfer(pData->host, pData->port))
			{
				LogMessage(MessageType::Error, _("Could not establish connection to server"));
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}

		cmd = pData->cmd;
		pData->pOldData->tranferCommandSent = true;

		engine_.transfer_status_.SetStartTime();
		m_pTransferSocket->SetActive();
		break;
	case rawtransfer_waitfinish:
	case rawtransfer_waittransferpre:
	case rawtransfer_waittransfer:
	case rawtransfer_waitsocket:
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("invalid opstate"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}
	if (!cmd.empty())
		if (!SendCommand(cmd, false, measureRTT))
			return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CFtpControlSocket::FileTransferTestResumeCapability()
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferTestResumeCapability()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CFtpFileTransferOpData *pData = static_cast<CFtpFileTransferOpData *>(m_pCurOpData);

	if (!pData->download) {
		return FZ_REPLY_OK;
	}

	for (int i = 0; i < 2; ++i) {
		if (pData->localFileSize >= (1ll << (i ? 31 : 32))) {
			switch (CServerCapabilities::GetCapability(*GetCurrentServer(), i ? resume2GBbug : resume4GBbug))
			{
			case yes:
				if (pData->remoteFileSize == pData->localFileSize) {
					LogMessage(MessageType::Debug_Info, _("Server does not support resume of files > %d GB. End transfer since file sizes match."), i ? 2 : 4);
					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_CANCELED;
				}
				LogMessage(MessageType::Error, _("Server does not support resume of files > %d GB."), i ? 2 : 4);
				ResetOperation(FZ_REPLY_CRITICALERROR);
				return FZ_REPLY_ERROR;
			case unknown:
				if (pData->remoteFileSize < pData->localFileSize) {
					// Don't perform test
					break;
				}
				if (pData->remoteFileSize == pData->localFileSize) {
					LogMessage(MessageType::Debug_Info, _("Server may not support resume of files > %d GB. End transfer since file sizes match."), i ? 2 : 4);
					ResetOperation(FZ_REPLY_OK);
					return FZ_REPLY_CANCELED;
				}
				else if (pData->remoteFileSize > pData->localFileSize) {
					LogMessage(MessageType::Status, _("Testing resume capabilities of server"));

					pData->opState = filetransfer_waitresumetest;
					pData->resumeOffset = pData->remoteFileSize - 1;

					m_pTransferSocket = new CTransferSocket(engine_, *this, TransferMode::resumetest);

					return Transfer(_T("RETR ") + pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath), pData);
				}
				break;
			case no:
				break;
			}
		}
	}

	return FZ_REPLY_OK;
}

int CFtpControlSocket::Connect(const CServer &server)
{
	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("deleting nonzero pData"));
		delete m_pCurOpData;
	}

	CFtpLogonOpData* pData = new CFtpLogonOpData;
	m_pCurOpData = pData;

	// Do not use FTP proxy if generic proxy is set
	int generic_proxy_type = engine_.GetOptions().GetOptionVal(OPTION_PROXY_TYPE);
	if ((generic_proxy_type <= CProxySocket::unknown || generic_proxy_type >= CProxySocket::proxytype_count) &&
		(pData->ftp_proxy_type = engine_.GetOptions().GetOptionVal(OPTION_FTP_PROXY_TYPE)) && !server.GetBypassProxy())
	{
		pData->host = engine_.GetOptions().GetOption(OPTION_FTP_PROXY_HOST);

		size_t pos = -1;
		if (!pData->host.empty() && pData->host[0] == '[') {
			// Probably IPv6 address
			pos = pData->host.find(']');
			if (pos == std::wstring::npos) {
				LogMessage(MessageType::Error, _("Proxy host starts with '[' but no closing bracket found."));
				DoClose(FZ_REPLY_CRITICALERROR);
				return FZ_REPLY_ERROR;
			}
			if (pData->host.size() > (pos + 1) && pData->host[pos + 1]) {
				if (pData->host[pos + 1] != ':') {
					LogMessage(MessageType::Error, _("Invalid proxy host, after closing bracket only colon and port may follow."));
					DoClose(FZ_REPLY_CRITICALERROR);
					return FZ_REPLY_ERROR;
				}
				++pos;
			}
			else {
				pos = std::wstring::npos;
			}
		}
		else {
			pos = pData->host.find(':');
		}

		if (pos != std::wstring::npos) {
			pData->port = fz::to_integral<unsigned int>(pData->host.substr(pos + 1));
			pData->host = pData->host.substr(0, pos);
		}
		else {
			pData->port = 21;
		}

		if (pData->host.empty() || pData->port < 1 || pData->port > 65535) {
			LogMessage(MessageType::Error, _("Proxy set but proxy host or port invalid"));
			DoClose(FZ_REPLY_CRITICALERROR);
			return FZ_REPLY_ERROR;
		}

		LogMessage(MessageType::Status, _("Connecting to %s through %s proxy"), server.Format(ServerFormat::with_optional_port), _T("FTP")); // @translator: Connecting to ftp.example.com through SOCKS5 proxy
	}
	else {
		pData->ftp_proxy_type = 0;
		pData->host = server.GetHost();
		pData->port = server.GetPort();
	}

	if (server.GetProtocol() != FTPES && server.GetProtocol() != FTP) {
		pData->neededCommands[LOGON_AUTH_TLS] = 0;
		pData->neededCommands[LOGON_AUTH_SSL] = 0;
		pData->neededCommands[LOGON_AUTH_WAIT] = 0;
		if (server.GetProtocol() != FTPS) {
			pData->neededCommands[LOGON_PBSZ] = 0;
			pData->neededCommands[LOGON_PROT] = 0;
		}
	}
	if (server.GetPostLoginCommands().empty())
		pData->neededCommands[LOGON_CUSTOMCOMMANDS] = 0;

	if (!GetLoginSequence(server))
		return DoClose(FZ_REPLY_INTERNALERROR);

	return CRealControlSocket::Connect(server);
}

bool CFtpControlSocket::CheckInclusion(const CDirectoryListing& listing1, const CDirectoryListing& listing2)
{
	// Check if listing2 is contained within listing1

	if (listing2.GetCount() > listing1.GetCount())
		return false;

	std::vector<std::wstring> names1, names2;
	listing1.GetFilenames(names1);
	listing2.GetFilenames(names2);
	std::sort(names1.begin(), names1.end());
	std::sort(names2.begin(), names2.end());

	std::vector<std::wstring>::const_iterator iter1, iter2;
	iter1 = names1.cbegin();
	iter2 = names2.cbegin();
	while (iter2 != names2.cbegin()) {
		if (iter1 == names1.cend())
			return false;

		if (*iter1 != *iter2) {
			++iter1;
			continue;
		}

		++iter1;
		++iter2;
	}

	return true;
}

void CFtpControlSocket::OnTimer(fz::timer_id id)
{
	if (id != m_idleTimer) {
		CControlSocket::OnTimer(id);
		return;
	}

	if (m_pCurOpData)
		return;

	if (m_pendingReplies || m_repliesToSkip)
		return;

	LogMessage(MessageType::Status, _("Sending keep-alive command"));

	wxString cmd;
	int i = fz::random_number(0, 2);
	if (!i)
		cmd = _T("NOOP");
	else if (i == 1)
	{
		if (m_lastTypeBinary)
			cmd = _T("TYPE I");
		else
			cmd = _T("TYPE A");
	}
	else
		cmd = _T("PWD");

	if (!SendCommand(cmd))
		return;
	++m_repliesToSkip;
}

void CFtpControlSocket::StartKeepaliveTimer()
{
	if (!engine_.GetOptions().GetOptionVal(OPTION_FTP_SENDKEEPALIVE))
		return;

	if (m_repliesToSkip || m_pendingReplies)
		return;

	if (!m_lastCommandCompletionTime)
		return;

	fz::duration const span = fz::monotonic_clock::now() - m_lastCommandCompletionTime;
	if (span.get_minutes() >= 30) {
		return;
	}

	stop_timer(m_idleTimer);
	m_idleTimer = add_timer(fz::duration::from_seconds(30), true);
}

int CFtpControlSocket::ParseSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CFtpControlSocket::ParseSubcommandResult(%d)"), prevResult);
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("ParseSubcommandResult called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	switch (m_pCurOpData->opId)
	{
	case Command::cwd:
		return ChangeDirSubcommandResult(prevResult);
	case Command::list:
		return ListSubcommandResult(prevResult);
	case Command::transfer:
		return FileTransferSubcommandResult(prevResult);
	case Command::del:
		return DeleteSubcommandResult(prevResult);
	case Command::removedir:
		return RemoveDirSubcommandResult(prevResult);
	case Command::rename:
		return RenameSubcommandResult(prevResult);
	case Command::chmod:
		return ChmodSubcommandResult(prevResult);
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown opID (%d) in ParseSubcommandResult"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}

void CFtpControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::timer_event>(ev, this, &CFtpControlSocket::OnTimer)) {
		return;
	}

	if (fz::dispatch<CExternalIPResolveEvent>(ev, this, &CFtpControlSocket::OnExternalIPAddress)) {
		return;
	}

	CRealControlSocket::operator()(ev);
}

std::wstring CFtpControlSocket::GetPassiveCommand(CRawTransferOpData& data)
{
	std::wstring ret = L"PASV";

	assert(data.bPasv);
	data.bTriedPasv = true;

	if (m_pProxyBackend) {
		// We don't actually know the address family the other end of the proxy uses to reach the server. Hence prefer EPSV
		// if the server supports it.
		if (CServerCapabilities::GetCapability(*m_pCurrentServer, epsv_command) == yes) {
			ret = L"EPSV";
		}
	}
	else if (m_pSocket->GetAddressFamily() == CSocket::ipv6) {
		// EPSV is mandatory for IPv6, don't check capabilities
		ret = L"EPSV";
	}
	return ret;
}
