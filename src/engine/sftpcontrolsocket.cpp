#include <filezilla.h>

#include "directorycache.h"
#include "directorylistingparser.h"
#include "engineprivate.h"
#include "pathcache.h"
#include "proxy.h"
#include "servercapabilities.h"
#include "sftpcontrolsocket.h"

#include <libfilezilla/event_loop.hpp>
#include <libfilezilla/local_filesys.hpp>
#include <libfilezilla/process.hpp>
#include <libfilezilla/thread_pool.hpp>

#include <wx/string.h>

#include <algorithm>
#include <cwchar>

#define FZSFTP_PROTOCOL_VERSION 8

struct sftp_message
{
	sftpEvent type;
	mutable std::wstring text[3];
};

struct sftp_event_type;
typedef fz::simple_event<sftp_event_type, sftp_message> CSftpEvent;

struct terminate_event_type;
typedef fz::simple_event<terminate_event_type, std::wstring> CTerminateEvent;

class CSftpFileTransferOpData final : public CFileTransferOpData
{
public:
	CSftpFileTransferOpData(bool is_download, std::wstring const& local_file, std::wstring const& remote_file, CServerPath const& remote_path)
		: CFileTransferOpData(is_download, local_file, remote_file, remote_path)
	{
	}
};

enum filetransferStates
{
	filetransfer_init = 0,
	filetransfer_waitcwd,
	filetransfer_waitlist,
	filetransfer_mtime,
	filetransfer_transfer,
	filetransfer_chmtime
};

class CSftpInputThread final
{
public:
	CSftpInputThread(CSftpControlSocket* pOwner, fz::process& proc)
		: process_(proc)
		, m_pOwner(pOwner)
	{
	}

	~CSftpInputThread()
	{
		thread_.join();
	}

	bool spawn(fz::thread_pool & pool)
	{
		if (!thread_) {
			thread_ = pool.spawn([this]() { entry(); });
		}
		return thread_.operator bool();
	}

protected:

	std::wstring ReadLine(std::wstring &error)
	{
		int len = 0;
		const int buffersize = 4096;
		char buffer[buffersize];

		while (true) {
			char c;
			int read = process_.read(&c, 1);
			if (read != 1) {
				if (!read) {
					error = L"Unexpected EOF.";
				}
				else {
					error = L"Unknown error reading from process";
				}
				return std::wstring();
			}

			if (c == '\n') {
				break;
			}

			if (len == buffersize - 1) {
				// Cap string length
				continue;
			}

			buffer[len++] = c;
		}

		while (len && buffer[len - 1] == '\r') {
			--len;
		}

		buffer[len] = 0;

		std::wstring const line = m_pOwner->ConvToLocal(buffer, len + 1);
		if (len && line.empty()) {
			error = L"Failed to convert reply to local character set.";
		}

		return line;
	}

	void entry()
	{
		std::wstring error;
		while (error.empty()) {
			char readType = 0;
			int read = process_.read(&readType, 1);
			if (read != 1) {
				break;
			}

			readType -= '0';

			if (readType < 0 || readType >= static_cast<char>(sftpEvent::count) ) {
				error = fz::sprintf(L"Unknown eventType %d", readType);
				break;
			}

			sftpEvent eventType = static_cast<sftpEvent>(readType);

			int lines{};
			switch (eventType)
			{
			case sftpEvent::count:
			case sftpEvent::Unknown:
				error = fz::sprintf(L"Unknown eventType %d", readType);
				break;
			case sftpEvent::Recv:
			case sftpEvent::Send:
			case sftpEvent::UsedQuotaRecv:
			case sftpEvent::UsedQuotaSend:
				break;
			case sftpEvent::Reply:
			case sftpEvent::Done:
			case sftpEvent::Error:
			case sftpEvent::Verbose:
			case sftpEvent::Info:
			case sftpEvent::Status:
			case sftpEvent::Transfer:
			case sftpEvent::AskPassword:
			case sftpEvent::RequestPreamble:
			case sftpEvent::RequestInstruction:
			case sftpEvent::KexAlgorithm:
			case sftpEvent::KexHash:
			case sftpEvent::KexCurve:
			case sftpEvent::CipherClientToServer:
			case sftpEvent::CipherServerToClient:
			case sftpEvent::MacClientToServer:
			case sftpEvent::MacServerToClient:
			case sftpEvent::Hostkey:
				lines = 1;
				break;
			case sftpEvent::AskHostkey:
			case sftpEvent::AskHostkeyChanged:
			case sftpEvent::AskHostkeyBetteralg:
				lines = 2;
				break;
			case sftpEvent::Listentry:
				lines = 3;
				break;
			};

			auto msg = new CSftpEvent;
			auto & message = std::get<0>(msg->v_);
			message.type = eventType;
			for (int i = 0; i < lines && error.empty(); ++i) {
				message.text[i] = ReadLine(error);
			}

			if (!error.empty()) {
				delete msg;
				break;
			}

			m_pOwner->send_event(msg);
		}

		m_pOwner->send_event<CTerminateEvent>(error);
	}

	fz::process& process_;
	CSftpControlSocket* m_pOwner;

	fz::async_task thread_;
};

class CSftpDeleteOpData final : public COpData
{
public:
	CSftpDeleteOpData()
		: COpData(Command::del)
	{
	}

	CServerPath path;
	std::deque<std::wstring> files;

	// Set to fz::datetime::Now initially and after
	// sending an updated listing to the UI.
	fz::datetime m_time;

	bool m_needSendListing{};

	// Set to true if deletion of at least one file failed
	bool m_deleteFailed{};
};

CSftpControlSocket::CSftpControlSocket(CFileZillaEnginePrivate & engine)
	: CControlSocket(engine)
{
	m_useUTF8 = true;
}

CSftpControlSocket::~CSftpControlSocket()
{
	remove_handler();
	DoClose();
}

enum connectStates
{
	connect_init,
	connect_proxy,
	connect_keys,
	connect_open
};

class CSftpConnectOpData final : public COpData
{
public:
	CSftpConnectOpData()
		: COpData(Command::connect)
		, keyfile_(keyfiles_.cend())
	{
	}

	std::wstring lastChallenge;
	CInteractiveLoginNotification::type lastChallengeType{CInteractiveLoginNotification::interactive};
	bool criticalFailure{};

	std::vector<std::wstring> keyfiles_;
	std::vector<std::wstring>::const_iterator keyfile_;
};

void CSftpControlSocket::Connect(CServer const& server)
{
	LogMessage(MessageType::Status, _("Connecting to %s..."), server.Format(ServerFormat::with_optional_port));
	SetWait(true);

	m_sftpEncryptionDetails = CSftpEncryptionNotification();

	delete m_pCSConv;
	if (server.GetEncodingType() == ENCODING_CUSTOM) {
		LogMessage(MessageType::Debug_Info, _T("Using custom encoding: %s"), server.GetCustomEncoding());
		m_pCSConv = new wxCSConv(server.GetCustomEncoding());
		m_useUTF8 = false;
	}
	else {
		m_pCSConv = 0;
		m_useUTF8 = true;
	}

	currentServer_ = server;

	CSftpConnectOpData* pData = new CSftpConnectOpData;
	Push(pData);

	pData->opState = connect_init;

	if (currentServer_.GetLogonType() == KEY) {
		pData->keyfiles_ = fz::strtok(currentServer_.GetKeyFile(), L"\r\n");
	}
	else {
		pData->keyfiles_ = fz::strtok(engine_.GetOptions().GetOption(OPTION_SFTP_KEYFILES), L"\r\n");
	}

	pData->keyfiles_.erase(
		std::remove_if(pData->keyfiles_.begin(), pData->keyfiles_.end(),
			[this](std::wstring const& keyfile) {
				if (fz::local_filesys::get_file_type(fz::to_native(keyfile), true) != fz::local_filesys::file) {
					LogMessage(MessageType::Status, _("Skipping non-existing key file \"%s\""), keyfile);
					return true;
				}
				return false;
		}), pData->keyfiles_.end());

	pData->keyfile_ = pData->keyfiles_.cbegin();

	m_pProcess = std::make_unique<fz::process>();

	engine_.GetRateLimiter().AddObject(this);

	auto executable = fz::to_native(engine_.GetOptions().GetOption(OPTION_FZSFTP_EXECUTABLE));
	if (executable.empty())
		executable = fzT("fzsftp");
	LogMessage(MessageType::Debug_Verbose, _T("Going to execute %s"), executable);

	std::vector<fz::native_string> args = {fzT("-v")};
	if (engine_.GetOptions().GetOptionVal(OPTION_SFTP_COMPRESSION)) {
		args.push_back(fzT("-C"));
	}
	if (!m_pProcess->spawn(executable, args)) {
		LogMessage(MessageType::Debug_Warning, _T("Could not create process"));
		DoClose();
		// FIXME
		//return FZ_REPLY_ERROR;
	}

	m_pInputThread = std::make_unique<CSftpInputThread>(this, *m_pProcess);
	if (!m_pInputThread->spawn(engine_.GetThreadPool())) {
		LogMessage(MessageType::Debug_Warning, _T("Thread creation failed"));
		m_pInputThread.reset();
		DoClose();
		// FIXME
		//return FZ_REPLY_ERROR;
	}
}

int CSftpControlSocket::ConnectParseResponse(bool successful, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ConnectParseResponse(%s)"), reply);

	if (!successful) {
		DoClose(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpConnectOpData *pData = static_cast<CSftpConnectOpData *>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	switch (pData->opState)
	{
	case connect_init:
		if (reply != fz::sprintf(L"fzSftp started, protocol_version=%d", FZSFTP_PROTOCOL_VERSION)) {
			LogMessage(MessageType::Error, _("fzsftp belongs to a different version of FileZilla"));
			DoClose(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		if (engine_.GetOptions().GetOptionVal(OPTION_PROXY_TYPE) && !currentServer_.GetBypassProxy()) {
			pData->opState = connect_proxy;
		}
		else if (pData->keyfile_ != pData->keyfiles_.cend()) {
			pData->opState = connect_keys;
		}
		else {
			pData->opState = connect_open;
		}
		break;
	case connect_proxy:
		if (pData->keyfile_ != pData->keyfiles_.cend()) {
			pData->opState = connect_keys;
		}
		else {
			pData->opState = connect_open;
		}
		break;
	case connect_keys:
		if (pData->keyfile_ == pData->keyfiles_.cend()) {
			pData->opState = connect_open;
		}
		break;
	case connect_open:
		engine_.AddNotification(new CSftpEncryptionNotification(m_sftpEncryptionDetails));
		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state: %d"), pData->opState);
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CSftpControlSocket::ConnectSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ConnectSend()"));
	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpConnectOpData *pData = static_cast<CSftpConnectOpData *>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case connect_proxy:
		{
			int type;
			switch (engine_.GetOptions().GetOptionVal(OPTION_PROXY_TYPE))
			{
			case CProxySocket::HTTP:
				type = 1;
				break;
			case CProxySocket::SOCKS5:
				type = 2;
				break;
			case CProxySocket::SOCKS4:
				type = 3;
				break;
			default:
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unsupported proxy type"));
				DoClose(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}

			std::wstring cmd = fz::sprintf(L"proxy %d \"%s\" %d", type,
											engine_.GetOptions().GetOption(OPTION_PROXY_HOST),
											engine_.GetOptions().GetOptionVal(OPTION_PROXY_PORT));
			std::wstring user = engine_.GetOptions().GetOption(OPTION_PROXY_USER);
			if (!user.empty()) {
				cmd += L" \"" + user + L"\"";
			}

			std::wstring show = cmd;
			std::wstring pass = engine_.GetOptions().GetOption(OPTION_PROXY_PASS);
			if (!pass.empty()) {
				cmd += L" \"" + pass + L"\"";
				show += L" \"" + std::wstring(pass.size(), '*') + L"\"";
			}
			res = SendCommand(cmd, show);
		}
		break;
	case connect_keys:
		res = SendCommand(L"keyfile \"" + *(pData->keyfile_++) + L"\"");
		break;
	case connect_open:
		res = SendCommand(fz::sprintf(L"open \"%s@%s\" %d", currentServer_.GetUser(), ConvertDomainName(currentServer_.GetHost()), currentServer_.GetPort()));
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Unknown op state: %d"), pData->opState);
		DoClose(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (res) {
		return FZ_REPLY_WOULDBLOCK;
	}
	else {
		return FZ_REPLY_ERROR;
	}
}

void CSftpControlSocket::OnSftpEvent(sftp_message const& message)
{
	if (!currentServer_) {
		return;
	}

	if (!m_pInputThread) {
		return;
	}

	switch (message.type)
	{
	case sftpEvent::Reply:
		LogMessageRaw(MessageType::Response, message.text[0]);
		ProcessReply(FZ_REPLY_OK, message.text[0]);
		break;
	case sftpEvent::Done:
		{
			int result;
			if (message.text[0] == L"1") {
				result = FZ_REPLY_OK;
			}
			else if (message.text[0] == L"2") {
				result = FZ_REPLY_CRITICALERROR;
			}
			else {
				result = FZ_REPLY_ERROR;
			}
			ProcessReply(result, std::wstring());
		}
		break;
	case sftpEvent::Error:
		LogMessageRaw(MessageType::Error, message.text[0]);
		break;
	case sftpEvent::Verbose:
		LogMessageRaw(MessageType::Debug_Info, message.text[0]);
		break;
	case sftpEvent::Info:
		LogMessageRaw(MessageType::Command, message.text[0]); // Not exactly the right message type, but it's a silent one.
		break;
	case sftpEvent::Status:
		LogMessageRaw(MessageType::Status, message.text[0]);
		break;
	case sftpEvent::Recv:
		SetActive(CFileZillaEngine::recv);
		break;
	case sftpEvent::Send:
		SetActive(CFileZillaEngine::send);
		break;
	case sftpEvent::Listentry:
		ListParseEntry(std::move(message.text[0]), message.text[1], std::move(message.text[2]));
		break;
	case sftpEvent::Transfer:
		{
			auto value = fz::to_integral<int64_t>(message.text[0]);

			bool tmp;
			CTransferStatus status = engine_.transfer_status_.Get(tmp);
			if (!status.empty() && !status.madeProgress) {
				if (m_pCurOpData && m_pCurOpData->opId == Command::transfer) {
					CSftpFileTransferOpData *pData = static_cast<CSftpFileTransferOpData *>(m_pCurOpData);
					if (pData->download) {
						if (value > 0) {
							engine_.transfer_status_.SetMadeProgress();
						}
					}
					else {
						if (status.currentOffset > status.startOffset + 65565) {
							engine_.transfer_status_.SetMadeProgress();
						}
					}
				}
			}

			engine_.transfer_status_.Update(value);
		}
		break;
	case sftpEvent::AskHostkey:
	case sftpEvent::AskHostkeyChanged:
		{
			auto port = fz::to_integral<int>(message.text[1]);
			if (port <= 0 || port > 65535) {
				DoClose(FZ_REPLY_INTERNALERROR);
				break;
			}
			SendAsyncRequest(new CHostKeyNotification(message.text[0], port, m_sftpEncryptionDetails, message.type == sftpEvent::AskHostkeyChanged));
		}
		break;
	case sftpEvent::AskHostkeyBetteralg:
		LogMessage(MessageType::Error, _T("Got sftpReqHostkeyBetteralg when we shouldn't have. Aborting connection."));
		DoClose(FZ_REPLY_INTERNALERROR);
		break;
	case sftpEvent::AskPassword:
		if (!m_pCurOpData || m_pCurOpData->opId != Command::connect) {
			LogMessage(MessageType::Debug_Warning, _T("sftpReqPassword outside connect operation, ignoring."));
			break;
		}
		else {
			CSftpConnectOpData *pData = static_cast<CSftpConnectOpData*>(m_pCurOpData);

			std::wstring const challengeIdentifier = m_requestPreamble + _T("\n") + m_requestInstruction + _T("\n") + message.text[0];

			CInteractiveLoginNotification::type t = CInteractiveLoginNotification::interactive;
			if (currentServer_.GetLogonType() == INTERACTIVE || m_requestPreamble == _T("SSH key passphrase")) {
				if (m_requestPreamble == _T("SSH key passphrase")) {
					t = CInteractiveLoginNotification::keyfile;
				}

				std::wstring challenge;
				if (!m_requestPreamble.empty() && t != CInteractiveLoginNotification::keyfile) {
					challenge += m_requestPreamble + _T("\n");
				}
				if (!m_requestInstruction.empty()) {
					challenge += m_requestInstruction + _T("\n");
				}
				if (message.text[0] != L"Password:") {
					challenge += message.text[0];
				}
				CInteractiveLoginNotification *pNotification = new CInteractiveLoginNotification(t, challenge, pData->lastChallenge == challengeIdentifier);
				pNotification->server = currentServer_;

				SendAsyncRequest(pNotification);
			}
			else {
				if (!pData->lastChallenge.empty() && pData->lastChallengeType != CInteractiveLoginNotification::keyfile) {
					// Check for same challenge. Will most likely fail as well, so abort early.
					if (pData->lastChallenge == challengeIdentifier) {
						LogMessage(MessageType::Error, _("Authentication failed."));
					}
					else {
						LogMessage(MessageType::Error, _("Server sent an additional login prompt. You need to use the interactive login type."));
					}
					DoClose(FZ_REPLY_CRITICALERROR | FZ_REPLY_PASSWORDFAILED);
					return;
				}

				std::wstring const pass = currentServer_.GetPass();
				std::wstring show = _T("Pass: ");
				show.append(pass.size(), '*');
				SendCommand(pass, show);
			}
			pData->lastChallenge = challengeIdentifier;
			pData->lastChallengeType = t;
		}
		break;
	case sftpEvent::RequestPreamble:
		m_requestPreamble = message.text[0];
		break;
	case sftpEvent::RequestInstruction:
		m_requestInstruction = message.text[0];
		break;
	case sftpEvent::UsedQuotaRecv:
		OnQuotaRequest(CRateLimiter::inbound);
		break;
	case sftpEvent::UsedQuotaSend:
		OnQuotaRequest(CRateLimiter::outbound);
		break;
	case sftpEvent::KexAlgorithm:
		m_sftpEncryptionDetails.kexAlgorithm = message.text[0];
		break;
	case sftpEvent::KexHash:
		m_sftpEncryptionDetails.kexHash = message.text[0];
		break;
	case sftpEvent::KexCurve:
		m_sftpEncryptionDetails.kexCurve = message.text[0];
		break;
	case sftpEvent::CipherClientToServer:
		m_sftpEncryptionDetails.cipherClientToServer = message.text[0];
		break;
	case sftpEvent::CipherServerToClient:
		m_sftpEncryptionDetails.cipherServerToClient = message.text[0];
		break;
	case sftpEvent::MacClientToServer:
		m_sftpEncryptionDetails.macClientToServer = message.text[0];
		break;
	case sftpEvent::MacServerToClient:
		m_sftpEncryptionDetails.macServerToClient = message.text[0];
		break;
	case sftpEvent::Hostkey:
		{
			auto tokens = fz::strtok(message.text[0], ' ');
			if (!tokens.empty()) {
				m_sftpEncryptionDetails.hostKeyFingerprintSHA256 = tokens.back();
				tokens.pop_back();
			}
			if (!tokens.empty()) {
				m_sftpEncryptionDetails.hostKeyFingerprintMD5 = tokens.back();
				tokens.pop_back();
			}
			for (auto const& token : tokens) {
				if (!m_sftpEncryptionDetails.hostKeyAlgorithm.empty()) {
					m_sftpEncryptionDetails.hostKeyAlgorithm += ' ';
				}
				m_sftpEncryptionDetails.hostKeyAlgorithm += token;
			}
		}
		break;
	default:
		wxFAIL_MSG(_T("given notification codes not handled"));
		break;
	}
}

void CSftpControlSocket::OnTerminate(std::wstring const& error)
{
	if (!error.empty()) {
		LogMessageRaw(MessageType::Error, error);
	}
	else {
		LogMessageRaw(MessageType::Debug_Info, _T("CSftpControlSocket::OnTerminate without error"));
	}
	if (m_pProcess) {
		DoClose();
	}
}

bool CSftpControlSocket::SendCommand(std::wstring const& cmd, std::wstring const& show)
{
	SetWait(true);

	LogMessageRaw(MessageType::Command, show.empty() ? cmd : show);

	// Check for newlines in command
	// a command like "ls\nrm foo/bar" is dangerous
	if (cmd.find('\n') != std::wstring::npos ||
		cmd.find('\r') != std::wstring::npos)
	{
		LogMessage(MessageType::Debug_Warning, _T("Command containing newline characters, aborting."));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	return AddToStream(cmd + _T("\n"));
}

bool CSftpControlSocket::AddToStream(std::wstring const& cmd, bool force_utf8)
{
	if (!m_pProcess) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return false;
	}

	std::string const str = ConvToServer(cmd, force_utf8);
	if (str.empty()) {
		LogMessage(MessageType::Error, _("Could not convert command to server encoding"));
		ResetOperation(FZ_REPLY_OK);
		return false;
	}

	return m_pProcess->write(str);
}

bool CSftpControlSocket::SetAsyncRequestReply(CAsyncRequestNotification *pNotification)
{
	if (m_pCurOpData) {
		if (!m_pCurOpData->waitForAsyncRequest) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Not waiting for request reply, ignoring request reply %d"), pNotification->GetRequestID());
			return false;
		}
		m_pCurOpData->waitForAsyncRequest = false;
	}

	RequestId const requestId = pNotification->GetRequestID();
	switch(requestId)
	{
	case reqId_fileexists:
		{
			CFileExistsNotification *pFileExistsNotification = static_cast<CFileExistsNotification *>(pNotification);
			return SetFileExistsAction(pFileExistsNotification);
		}
	case reqId_hostkey:
	case reqId_hostkeyChanged:
		{
			if (GetCurrentCommandId() != Command::connect ||
				!currentServer_)
			{
				LogMessage(MessageType::Debug_Info, _T("SetAsyncRequestReply called to wrong time"));
				return false;
			}

			CHostKeyNotification *pHostKeyNotification = static_cast<CHostKeyNotification *>(pNotification);
			std::wstring show;
			if (requestId == reqId_hostkey) {
				show = _("Trust new Hostkey:");
			}
			else {
				show = _("Trust changed Hostkey:");
			}
			show += ' ';
			if (!pHostKeyNotification->m_trust) {
				SendCommand(std::wstring(), show + _("No"));
				if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
					CSftpConnectOpData *pData = static_cast<CSftpConnectOpData *>(m_pCurOpData);
					pData->criticalFailure = true;
				}
			}
			else if (pHostKeyNotification->m_alwaysTrust) {
				SendCommand(L"y", show + _("Yes"));
			}
			else {
				SendCommand(L"n", show + _("Once"));
			}
		}
		break;
	case reqId_interactiveLogin:
		{
			CInteractiveLoginNotification *pInteractiveLoginNotification = static_cast<CInteractiveLoginNotification *>(pNotification);

			if (!pInteractiveLoginNotification->passwordSet) {
				DoClose(FZ_REPLY_CANCELED);
				return false;
			}
			std::wstring const pass = pInteractiveLoginNotification->server.GetPass();
			currentServer_.SetUser(currentServer_.GetUser(), pass);
			std::wstring show = L"Pass: ";
			show.append(pass.size(), '*');
			SendCommand(pass, show);
		}
		break;
	default:
		LogMessage(MessageType::Debug_Warning, _T("Unknown async request reply id: %d"), requestId);
		return false;
	}

	return true;
}

class CSftpListOpData final : public COpData
{
public:
	CSftpListOpData()
		: COpData(Command::list)
	{
	}

	std::unique_ptr<CDirectoryListingParser> pParser;

	CServerPath path;
	std::wstring subDir;

	// Set to true to get a directory listing even if a cache
	// lookup can be made after finding out true remote directory
	bool refresh{};
	bool fallback_to_current{};

	CDirectoryListing directoryListing;

	fz::monotonic_clock m_time_before_locking;
};

enum listStates
{
	list_init = 0,
	list_waitcwd,
	list_waitlock,
	list_list
};

int CSftpControlSocket::List(CServerPath path, std::wstring const& subDir, int flags)
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

	if (!currentServer_) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurrenServer == 0"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpListOpData *pData = new CSftpListOpData;
	Push(pData);
	
	pData->opState = list_waitcwd;

	if (path.GetType() == DEFAULT) {
		path.SetType(currentServer_.GetType());
	}
	pData->path = path;
	pData->subDir = subDir;
	pData->refresh = (flags & LIST_FLAG_REFRESH) != 0;
	pData->fallback_to_current = !path.empty() && (flags & LIST_FLAG_FALLBACK_CURRENT) != 0;

	int res = ChangeDir(path, subDir, (flags & LIST_FLAG_LINK) != 0);
	if (res != FZ_REPLY_OK) {
		return res;
	}

	return ListSubcommandResult(FZ_REPLY_OK);
}

int CSftpControlSocket::ListParseResponse(bool successful, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ListParseResponse(%s)"), reply);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpListOpData *pData = static_cast<CSftpListOpData *>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->opState == list_list) {
		if (!successful) {
			ResetOperation(FZ_REPLY_ERROR);
			return FZ_REPLY_ERROR;
		}

		if (!pData->pParser) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("pData->pParser is 0"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}

		pData->directoryListing = pData->pParser->Parse(m_CurrentPath);
		engine_.GetDirectoryCache().Store(pData->directoryListing, currentServer_);
		SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, false);

		ResetOperation(FZ_REPLY_OK);
		return FZ_REPLY_OK;
	}

	LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("ListParseResponse called at inproper time: %d"), pData->opState);
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

int CSftpControlSocket::ListParseEntry(std::wstring && entry, std::wstring const& stime, std::wstring && name)
{
	if (!m_pCurOpData) {
		LogMessageRaw(MessageType::RawList, entry);
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->opId != Command::list) {
		LogMessageRaw(MessageType::RawList, entry);
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Listentry received, but current operation is not Command::list"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpListOpData *pData = static_cast<CSftpListOpData *>(m_pCurOpData);
	if (!pData) {
		LogMessageRaw(MessageType::RawList, entry);
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData of wrong type"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->opState != list_list) {
		LogMessageRaw(MessageType::RawList, entry);
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("ListParseResponse called at inproper time: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!pData->pParser) {
		LogMessageRaw(MessageType::RawList, entry);
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("pData->pParser is 0"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_INTERNALERROR;
	}

	fz::datetime time;
	if (!stime.empty()) {
		int64_t t = std::wcstoll(stime.c_str(), 0, 10);
		if (t > 0) {
			time = fz::datetime(static_cast<time_t>(t), fz::datetime::seconds);
		}
	}
	pData->pParser->AddLine(std::move(entry), std::move(name), time);

	return FZ_REPLY_WOULDBLOCK;
}

int CSftpControlSocket::ListSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ListSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpListOpData *pData = static_cast<CSftpListOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (pData->opState != list_waitcwd) {
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		if (pData->fallback_to_current) {
			// List current directory instead
			pData->fallback_to_current = false;
			pData->path.clear();
			pData->subDir = _T("");
			int res = ChangeDir();
			if (res != FZ_REPLY_OK) {
				return res;
			}
		}
		else {
			ResetOperation(prevResult);
			return FZ_REPLY_ERROR;
		}
	}

	if (pData->path.empty()) {
		pData->path = m_CurrentPath;
	}

	if (!pData->refresh) {
		assert(!pData->pNextOpData);

		// Do a cache lookup now that we know the correct directory

		int hasUnsureEntries;
		bool is_outdated = false;
		bool found = engine_.GetDirectoryCache().DoesExist(currentServer_, m_CurrentPath, hasUnsureEntries, is_outdated);
		if (found) {
			// We're done if listins is recent and has no outdated entries
			if (!is_outdated && !hasUnsureEntries) {
				SendDirectoryListingNotification(m_CurrentPath, !pData->pNextOpData, false);

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

	pData->opState = list_list;

	return SendNextCommand();
}

int CSftpControlSocket::ListSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ListSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpListOpData *pData = static_cast<CSftpListOpData *>(m_pCurOpData);
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
		assert(pData->subDir.empty()); // Did do ChangeDir before trying to lock
		bool found = engine_.GetDirectoryCache().Lookup(listing, currentServer_, pData->path, true, is_outdated);
		if (found && !is_outdated && !listing.get_unsure_flags() &&
			listing.m_firstListTime >= pData->m_time_before_locking)
		{
			SendDirectoryListingNotification(listing.path, !pData->pNextOpData, false);

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}

		pData->opState = list_waitcwd;

		return ListSubcommandResult(FZ_REPLY_OK);
	}
	else if (pData->opState == list_list) {
		pData->pParser = std::make_unique<CDirectoryListingParser>(this, currentServer_, listingEncoding::unknown);
		if (!SendCommand(_T("ls"))) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	LogMessage(MessageType::Debug_Warning, _T("Unknown opStatein CSftpControlSocket::ListSend"));
	ResetOperation(FZ_REPLY_INTERNALERROR);
	return FZ_REPLY_ERROR;
}

class CSftpChangeDirOpData final : public CChangeDirOpData
{
};

enum cwdStates
{
	cwd_init = 0,
	cwd_pwd,
	cwd_cwd,
	cwd_cwd_subdir
};

int CSftpControlSocket::ChangeDir(CServerPath path, std::wstring subDir, bool link_discovery)
{
	cwdStates state = cwd_init;

	if (path.GetType() == DEFAULT) {
		path.SetType(currentServer_.GetType());
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
			target = engine_.GetPathCache().Lookup(currentServer_, path, subDir);
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
				target = engine_.GetPathCache().Lookup(currentServer_, path, _T(""));
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
			target = engine_.GetPathCache().Lookup(currentServer_, path, _T(""));
			if (m_CurrentPath == path || (!target.empty() && target == m_CurrentPath)) {
				return FZ_REPLY_OK;
			}
			state = cwd_cwd;
		}
	}

	CSftpChangeDirOpData *pData = new CSftpChangeDirOpData;
	pData->pNextOpData = m_pCurOpData;
	pData->opState = state;
	pData->path = path;
	pData->subDir = subDir;
	pData->target = target;
	pData->link_discovery = link_discovery;

	if (pData->pNextOpData && pData->pNextOpData->opId == Command::transfer &&
		!static_cast<CSftpFileTransferOpData *>(pData->pNextOpData)->download)
	{
		pData->tryMkdOnFail = true;
		assert(subDir.empty());
	}

	Push(pData);

	return SendNextCommand();
}

int CSftpControlSocket::ChangeDirParseResponse(bool successful, std::wstring const& reply)
{
	if (!m_pCurOpData) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CSftpChangeDirOpData *pData = static_cast<CSftpChangeDirOpData *>(m_pCurOpData);

	bool error = false;
	switch (pData->opState)
	{
	case cwd_pwd:
		if (!successful || reply.empty()) {
			error = true;
		}
		if (ParsePwdReply(reply)) {
			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else {
			error = true;
		}
		break;
	case cwd_cwd:
		if (!successful) {
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
		else if (reply.empty()) {
			error = true;
		}
		else if (ParsePwdReply(reply)) {
			engine_.GetPathCache().Store(currentServer_, m_CurrentPath, pData->path);

			if (pData->subDir.empty()) {
				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}

			pData->target.clear();
			pData->opState = cwd_cwd_subdir;
		}
		else {
			error = true;
		}
		break;
	case cwd_cwd_subdir:
		if (!successful || reply.empty()) {
			if (pData->link_discovery) {
				LogMessage(MessageType::Debug_Info, _T("Symlink does not link to a directory, probably a file"));
				ResetOperation(FZ_REPLY_LINKNOTDIR);
				return FZ_REPLY_ERROR;
			}
			else {
				error = true;
			}
		}
		else if (ParsePwdReply(reply)) {
			engine_.GetPathCache().Store(currentServer_, m_CurrentPath, pData->path, pData->subDir);

			ResetOperation(FZ_REPLY_OK);
			return FZ_REPLY_OK;
		}
		else {
			error = true;
		}
		break;
	default:
		error = true;
		break;
	}

	if (error) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CSftpControlSocket::ChangeDirSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ChangeDirSubcommandResult(%d)"), prevResult);

	return SendNextCommand();
}

int CSftpControlSocket::ChangeDirSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ChangeDirSend()"));

	if (!m_pCurOpData) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}
	CSftpChangeDirOpData *pData = static_cast<CSftpChangeDirOpData *>(m_pCurOpData);

	std::wstring cmd;
	switch (pData->opState)
	{
	case cwd_pwd:
		cmd = L"pwd";
		break;
	case cwd_cwd:
		if (pData->tryMkdOnFail && !pData->holdsLock) {
			if (IsLocked(lock_mkdir, pData->path)) {
				// Some other engine is already creating this directory or
				// performing an action that will lead to its creation
				pData->tryMkdOnFail = false;
			}
			if (!TryLockCache(lock_mkdir, pData->path)) {
				return FZ_REPLY_WOULDBLOCK;
			}
		}
		cmd = L"cd " + QuoteFilename(pData->path.GetPath());
		m_CurrentPath.clear();
		break;
	case cwd_cwd_subdir:
		if (pData->subDir.empty()) {
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
		else {
			cmd = L"cd " + QuoteFilename(pData->subDir);
		}
		m_CurrentPath.clear();
		break;
	}

	if (!cmd.empty()) {
		if (!SendCommand(cmd)) {
			return FZ_REPLY_ERROR;
		}
	}

	return FZ_REPLY_WOULDBLOCK;
}

void CSftpControlSocket::ProcessReply(int result, std::wstring const& reply)
{
	Command commandId = GetCurrentCommandId();
	switch (commandId)
	{
	case Command::connect:
		ConnectParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::list:
		ListParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::transfer:
		FileTransferParseResponse(result, reply);
		break;
	case Command::cwd:
		ChangeDirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::mkdir:
		MkdirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::del:
		DeleteParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::removedir:
		RemoveDirParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::chmod:
		ChmodParseResponse(result == FZ_REPLY_OK, reply);
		break;
	case Command::rename:
		RenameParseResponse(result == FZ_REPLY_OK, reply);
		break;
	default:
		LogMessage(MessageType::Debug_Warning, _T("No action for parsing replies to command %d"), commandId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}
}

int CSftpControlSocket::ResetOperation(int nErrorCode)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ResetOperation(%d)"), nErrorCode);

	if (m_pCurOpData && m_pCurOpData->opId == Command::connect) {
		CSftpConnectOpData *pData = static_cast<CSftpConnectOpData *>(m_pCurOpData);
		if (pData->opState == connect_init && (nErrorCode & FZ_REPLY_CANCELED) != FZ_REPLY_CANCELED) {
			LogMessage(MessageType::Error, _("fzsftp could not be started"));
		}
		if (pData->criticalFailure) {
			nErrorCode |= FZ_REPLY_CRITICALERROR;
		}
	}
	if (m_pCurOpData && m_pCurOpData->opId == Command::del && !(nErrorCode & FZ_REPLY_DISCONNECTED)) {
		CSftpDeleteOpData *pData = static_cast<CSftpDeleteOpData *>(m_pCurOpData);
		if (pData->m_needSendListing) {
			SendDirectoryListingNotification(pData->path, false, false);
		}
	}

	return CControlSocket::ResetOperation(nErrorCode);
}

int CSftpControlSocket::SendNextCommand()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::SendNextCommand()"));
	if (!m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("SendNextCommand called without active operation"));
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (m_pCurOpData->waitForAsyncRequest)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Waiting for async request, ignoring SendNextCommand"));
		return FZ_REPLY_WOULDBLOCK;
	}

	switch (m_pCurOpData->opId)
	{
	case Command::connect:
		return ConnectSend();
	case Command::list:
		return ListSend();
	case Command::transfer:
		return FileTransferSend();
	case Command::cwd:
		return ChangeDirSend();
	case Command::mkdir:
		return MkdirSend();
	case Command::rename:
		return RenameSend();
	case Command::chmod:
		return ChmodSend();
	case Command::del:
		return DeleteSend();
	default:
		LogMessage(MessageType::Debug_Warning, __TFILE__, __LINE__, _T("Unknown opID (%d) in SendNextCommand"), m_pCurOpData->opId);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		break;
	}

	return FZ_REPLY_ERROR;
}

int CSftpControlSocket::FileTransfer(std::wstring const& localFile, CServerPath const& remotePath,
									std::wstring const& remoteFile, bool download,
									CFileTransferCommand::t_transferSettings const& transferSettings)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::FileTransfer(...)"));

	if (localFile.empty()) {
		if (!download) {
			ResetOperation(FZ_REPLY_CRITICALERROR | FZ_REPLY_NOTSUPPORTED);
		}
		else {
			ResetOperation(FZ_REPLY_SYNTAXERROR);
		}
		return FZ_REPLY_ERROR;
	}

	if (download) {
		std::wstring filename = remotePath.FormatFilename(remoteFile);
		LogMessage(MessageType::Status, _("Starting download of %s"), filename);
	}
	else {
		LogMessage(MessageType::Status, _("Starting upload of %s"), localFile);
	}
	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("deleting nonzero pData"));
		delete m_pCurOpData;
	}

	CSftpFileTransferOpData *pData = new CSftpFileTransferOpData(download, localFile, remoteFile, remotePath);
	Push(pData);

	pData->transferSettings = transferSettings;

	int64_t size;
	bool isLink;
	if (fz::local_filesys::get_file_info(fz::to_native(pData->localFile), isLink, &size, 0, 0) == fz::local_filesys::file) {
		pData->localFileSize = size;
	}

	pData->opState = filetransfer_waitcwd;

	if (pData->remotePath.GetType() == DEFAULT)
		pData->remotePath.SetType(currentServer_.GetType());

	int res = ChangeDir(pData->remotePath);
	if (res != FZ_REPLY_OK)
		return res;

	return FileTransferSubcommandResult(FZ_REPLY_OK);
}

int CSftpControlSocket::FileTransferSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::FileTransferSubcommandResult()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpFileTransferOpData *pData = static_cast<CSftpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_waitcwd) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist)
					pData->opState = filetransfer_waitlist;
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS))
				{
					pData->opState = filetransfer_mtime;
				}
				else
					pData->opState = filetransfer_transfer;
			}
			else {
				if (entry.is_unsure())
					pData->opState = filetransfer_waitlist;
				else {
					if (matchedCase) {
						pData->remoteFileSize = entry.size;
						if (entry.has_date())
							pData->fileTime = entry.time;

						if (pData->download && !entry.has_time() &&
							engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS))
						{
							pData->opState = filetransfer_mtime;
						}
						else
							pData->opState = filetransfer_transfer;
					}
					else
						pData->opState = filetransfer_mtime;
				}
			}
			if (pData->opState == filetransfer_waitlist) {
				int res = List(CServerPath(), _T(""), LIST_FLAG_REFRESH);
				if (res != FZ_REPLY_OK)
					return res;
				ResetOperation(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}
			else if (pData->opState == filetransfer_transfer) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK)
					return res;
			}
		}
		else {
			pData->tryAbsolutePath = true;
			pData->opState = filetransfer_mtime;
		}
	}
	else if (pData->opState == filetransfer_waitlist) {
		if (prevResult == FZ_REPLY_OK) {
			CDirentry entry;
			bool dirDidExist;
			bool matchedCase;
			bool found = engine_.GetDirectoryCache().LookupFile(entry, currentServer_, pData->tryAbsolutePath ? pData->remotePath : m_CurrentPath, pData->remoteFile, dirDidExist, matchedCase);
			if (!found) {
				if (!dirDidExist)
					pData->opState = filetransfer_mtime;
				else if (pData->download &&
					engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS))
				{
					pData->opState = filetransfer_mtime;
				}
				else
					pData->opState = filetransfer_transfer;
			}
			else {
				if (matchedCase && !entry.is_unsure()) {
					pData->remoteFileSize = entry.size;
					if (!entry.has_date())
						pData->fileTime = entry.time;

					if (pData->download && !entry.has_time() &&
						engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS))
					{
						pData->opState = filetransfer_mtime;
					}
					else
						pData->opState = filetransfer_transfer;
				}
				else
					pData->opState = filetransfer_mtime;
			}
			if (pData->opState == filetransfer_transfer) {
				int res = CheckOverwriteFile();
				if (res != FZ_REPLY_OK)
					return res;
			}
		}
		else
			pData->opState = filetransfer_mtime;
	}
	else {
		LogMessage(MessageType::Debug_Warning, _T("  Unknown opState (%d)"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	return SendNextCommand();
}

int CSftpControlSocket::FileTransferSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferSend()"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpFileTransferOpData *pData = static_cast<CSftpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_transfer) {
		std::wstring cmd;
		if (pData->resume) {
			cmd = _T("re");
		}
		if (pData->download) {
			if (!pData->resume) {
				CreateLocalDir(pData->localFile);
			}

			engine_.transfer_status_.Init(pData->remoteFileSize, pData->resume ? pData->localFileSize : 0, false);
			cmd += L"get ";
			cmd += QuoteFilename(pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath)) + L" ";

			std::wstring localFile = QuoteFilename(pData->localFile);
			std::wstring logstr = cmd;
			logstr += localFile;
			LogMessageRaw(MessageType::Command, logstr);

			if (!AddToStream(cmd) || !AddToStream(localFile + L"\n", true)) {
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}
		else {
			engine_.transfer_status_.Init(pData->localFileSize, pData->resume ? pData->remoteFileSize : 0, false);
			cmd += L"put ";

			std::wstring logstr = cmd;
			std::wstring localFile = QuoteFilename(pData->localFile) + L" ";
			std::wstring remoteFile = QuoteFilename(pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath));

			logstr += localFile;
			logstr += remoteFile;
			LogMessageRaw(MessageType::Command, logstr);

			if (!AddToStream(cmd) || !AddToStream(localFile, true) ||
				!AddToStream(remoteFile + L"\n"))
			{
				ResetOperation(FZ_REPLY_ERROR);
				return FZ_REPLY_ERROR;
			}
		}
		engine_.transfer_status_.SetStartTime();

		pData->transferInitiated = true;
	}
	else if (pData->opState == filetransfer_mtime) {
		std::wstring quotedFilename = QuoteFilename(pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath));
		if (!SendCommand(_T("mtime ") + WildcardEscape(quotedFilename),
			L"mtime " + quotedFilename))
			return FZ_REPLY_ERROR;
	}
	else if (pData->opState == filetransfer_chmtime) {
		assert(!pData->fileTime.empty());
		if (pData->download) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("  filetransfer_chmtime during download"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}

		std::wstring quotedFilename = QuoteFilename(pData->remotePath.FormatFilename(pData->remoteFile, !pData->tryAbsolutePath));

		fz::datetime t = pData->fileTime;
		t -= fz::duration::from_minutes(currentServer_.GetTimezoneOffset());

		// Y2K38
		time_t ticks = t.get_time_t();
		std::wstring seconds = fz::sprintf(L"%d", ticks);
		if (!SendCommand(L"chmtime " + seconds + _T(" ") + WildcardEscape(quotedFilename),
			L"chmtime " + seconds + L" " + quotedFilename))
			return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

int CSftpControlSocket::FileTransferParseResponse(int result, std::wstring const& reply)
{
	LogMessage(MessageType::Debug_Verbose, _T("FileTransferParseResponse(%d)"), result);

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpFileTransferOpData *pData = static_cast<CSftpFileTransferOpData *>(m_pCurOpData);

	if (pData->opState == filetransfer_transfer) {
		if (result != FZ_REPLY_OK) {
			ResetOperation(result);
			return FZ_REPLY_ERROR;
		}

		if (engine_.GetOptions().GetOptionVal(OPTION_PRESERVE_TIMESTAMPS)) {
			if (pData->download) {
				if (!pData->fileTime.empty()) {
					if (!fz::local_filesys::set_modification_time(fz::to_native(pData->localFile), pData->fileTime))
						LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("Could not set modification time"));
				}
			}
			else {
				pData->fileTime = fz::local_filesys::get_modification_time(fz::to_native(pData->localFile));
				if (!pData->fileTime.empty()) {
					pData->opState = filetransfer_chmtime;
					return SendNextCommand();
				}
			}
		}
	}
	else if (pData->opState == filetransfer_mtime) {
		if (result == FZ_REPLY_OK && !reply.empty()) {
			time_t seconds = 0;
			bool parsed = true;
			for (auto c : reply) {
				if (c < '0' || c > '9') {
					parsed = false;
					break;
				}
				seconds *= 10;
				seconds += c - '0';
			}
			if (parsed) {
				fz::datetime fileTime = fz::datetime(seconds, fz::datetime::seconds);
				if (!fileTime.empty()) {
					pData->fileTime = fileTime;
					pData->fileTime += fz::duration::from_minutes(currentServer_.GetTimezoneOffset());
				}
			}
		}
		pData->opState = filetransfer_transfer;
		int res = CheckOverwriteFile();
		if (res != FZ_REPLY_OK) {
			return res;
		}

		return SendNextCommand();
	}
	else if (pData->opState == filetransfer_chmtime) {
		if (pData->download) {
			LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("  filetransfer_chmtime during download"));
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return FZ_REPLY_ERROR;
		}
	}
	else {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("  Called at improper time: opState == %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

int CSftpControlSocket::DoClose(int nErrorCode)
{
	engine_.GetRateLimiter().RemoveObject(this);

	if (m_pProcess) {
		m_pProcess->kill();
	}

	if (m_pInputThread) {
		m_pInputThread.reset();

		auto threadEventsFilter = [&](fz::event_loop::Events::value_type const& ev) -> bool {
			if (ev.first != this) {
				return false;
			}
			else if (ev.second->derived_type() == CSftpEvent::type() || ev.second->derived_type() == CTerminateEvent::type()) {
				return true;
			}
			return false;
		};

		event_loop_.filter_events(threadEventsFilter);
	}
	m_pProcess.reset();
	return CControlSocket::DoClose(nErrorCode);
}

void CSftpControlSocket::Cancel()
{
	if (GetCurrentCommandId() != Command::none) {
		DoClose(FZ_REPLY_CANCELED);
	}
}

enum mkdStates
{
	mkd_init = 0,
	mkd_findparent,
	mkd_mkdsub,
	mkd_cwdsub,
	mkd_tryfull
};

int CSftpControlSocket::Mkdir(const CServerPath& path)
{
	/* Directory creation works like this: First find a parent directory into
	 * which we can CWD, then create the subdirs one by one. If either part
	 * fails, try MKD with the full path directly.
	 */

	if (!m_pCurOpData) {
		LogMessage(MessageType::Status, _("Creating directory '%s'..."), path.GetPath());
	}

	CMkdirOpData *pData = new CMkdirOpData;
	pData->path = path;

	if (!m_CurrentPath.empty()) {
		// Unless the server is broken, a directory already exists if current directory is a subdir of it.
		if (m_CurrentPath == path || m_CurrentPath.IsSubdirOf(path, false)) {
			delete pData;
			return FZ_REPLY_OK;
		}

		if (m_CurrentPath.IsParentOf(path, false)) {
			pData->commonParent = m_CurrentPath;
		}
		else {
			pData->commonParent = path.GetCommonParent(m_CurrentPath);
		}
	}

	if (!path.HasParent()) {
		pData->opState = mkd_tryfull;
	}
	else {
		pData->currentPath = path.GetParent();
		pData->segments.push_back(path.GetLastSegment());

		if (pData->currentPath == m_CurrentPath) {
			pData->opState = mkd_mkdsub;
		}
		else {
			pData->opState = mkd_findparent;
		}
	}

	Push(pData);

	return SendNextCommand();
}

int CSftpControlSocket::MkdirParseResponse(bool successful, std::wstring const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::MkdirParseResonse"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CMkdirOpData *pData = static_cast<CMkdirOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	bool error = false;
	switch (pData->opState)
	{
	case mkd_findparent:
		if (successful) {
			m_CurrentPath = pData->currentPath;
			pData->opState = mkd_mkdsub;
		}
		else if (pData->currentPath == pData->commonParent) {
			pData->opState = mkd_tryfull;
		}
		else if (pData->currentPath.HasParent()) {
			pData->segments.push_back(pData->currentPath.GetLastSegment());
			pData->currentPath = pData->currentPath.GetParent();
		}
		else {
			pData->opState = mkd_tryfull;
		}
		break;
	case mkd_mkdsub:
		if (successful) {
			if (pData->segments.empty()) {
				LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("  pData->segments is empty"));
				ResetOperation(FZ_REPLY_INTERNALERROR);
				return FZ_REPLY_ERROR;
			}
			engine_.GetDirectoryCache().UpdateFile(currentServer_, pData->currentPath, pData->segments.back(), true, CDirectoryCache::dir);
			SendDirectoryListingNotification(pData->currentPath, false, false);

			pData->currentPath.AddSegment(pData->segments.back());
			pData->segments.pop_back();

			if (pData->segments.empty()) {
				ResetOperation(FZ_REPLY_OK);
				return FZ_REPLY_OK;
			}
			else {
				pData->opState = mkd_cwdsub;
			}
		}
		else {
			pData->opState = mkd_tryfull;
		}
		break;
	case mkd_cwdsub:
		if (successful) {
			m_CurrentPath = pData->currentPath;
			pData->opState = mkd_mkdsub;
		}
		else {
			pData->opState = mkd_tryfull;
		}
		break;
	case mkd_tryfull:
		if (!successful) {
			error = true;
		}
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

	return MkdirSend();
}

int CSftpControlSocket::MkdirSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::MkdirSend"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CMkdirOpData *pData = static_cast<CMkdirOpData *>(m_pCurOpData);
	LogMessage(MessageType::Debug_Debug, _T("  state = %d"), pData->opState);

	if (!pData->holdsLock) {
		if (!TryLockCache(lock_mkdir, pData->path)) {
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	bool res;
	switch (pData->opState)
	{
	case mkd_findparent:
	case mkd_cwdsub:
		m_CurrentPath.clear();
		res = SendCommand(L"cd " + QuoteFilename(pData->currentPath.GetPath()));
		break;
	case mkd_mkdsub:
		res = SendCommand(L"mkdir " + QuoteFilename(pData->segments.back()));
		break;
	case mkd_tryfull:
		res = SendCommand(L"mkdir " + QuoteFilename(pData->path.GetPath()));
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res) {
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

std::wstring CSftpControlSocket::QuoteFilename(std::wstring const& filename)
{
	return L"\"" + fz::replaced_substrings(filename, L"\"", L"\"\"") + L"\"";
}

int CSftpControlSocket::Delete(const CServerPath& path, std::deque<std::wstring>&& files)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::Delete"));
	assert(!m_pCurOpData);
	CSftpDeleteOpData *pData = new CSftpDeleteOpData();
	Push(pData);
	pData->path = path;
	pData->files = files;

	// CFileZillaEnginePrivate should have checked this already
	assert(!files.empty());

	return SendNextCommand();
}

int CSftpControlSocket::DeleteParseResponse(bool successful, std::wstring const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::DeleteParseResponse"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpDeleteOpData *pData = static_cast<CSftpDeleteOpData *>(m_pCurOpData);

	if (!successful) {
		pData->m_deleteFailed = true;
	}
	else {
		std::wstring const& file = pData->files.front();

		engine_.GetDirectoryCache().RemoveFile(currentServer_, pData->path, file);

		auto const now = fz::datetime::now();
		if (!pData->m_time.empty() && (now - pData->m_time).get_seconds() >= 1) {
			SendDirectoryListingNotification(pData->path, false, false);
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

int CSftpControlSocket::DeleteSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::DeleteSend"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}
	CSftpDeleteOpData *pData = static_cast<CSftpDeleteOpData *>(m_pCurOpData);

	std::wstring const& file = pData->files.front();
	if (file.empty()) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty filename"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	std::wstring filename = pData->path.FormatFilename(file);
	if (filename.empty()) {
		LogMessage(MessageType::Error, _("Filename cannot be constructed for directory %s and filename %s"), pData->path.GetPath(), file);
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	if (pData->m_time.empty()) {
		pData->m_time = fz::datetime::now();
	}

	engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->path, file);

	if (!SendCommand(L"rm " + WildcardEscape(QuoteFilename(filename)),
			  L"rm " + QuoteFilename(filename)))
	{
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

class CSftpRemoveDirOpData final : public COpData
{
public:
	CSftpRemoveDirOpData()
		: COpData(Command::removedir)
	{
	}

	CServerPath path;
	std::wstring subDir;
};

int CSftpControlSocket::RemoveDir(CServerPath const& path, std::wstring const& subDir)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::RemoveDir"));

	assert(!m_pCurOpData);
	CSftpRemoveDirOpData *pData = new CSftpRemoveDirOpData();
	Push(pData);
	pData->path = path;
	pData->subDir = subDir;

	CServerPath fullPath = engine_.GetPathCache().Lookup(currentServer_, pData->path, pData->subDir);
	if (fullPath.empty()) {
		fullPath = pData->path;

		if (!fullPath.AddSegment(subDir)) {
			LogMessage(MessageType::Error, _("Path cannot be constructed for directory %s and subdir %s"), path.GetPath(), subDir);
			return FZ_REPLY_ERROR;
		}
	}

	engine_.GetDirectoryCache().InvalidateFile(currentServer_, path, subDir);

	engine_.GetPathCache().InvalidatePath(currentServer_, pData->path, pData->subDir);

	engine_.InvalidateCurrentWorkingDirs(fullPath);
	std::wstring quotedFilename = QuoteFilename(fullPath.GetPath());
	if (!SendCommand(L"rmdir " + WildcardEscape(quotedFilename),
			  L"rmdir " + quotedFilename))
		return FZ_REPLY_ERROR;

	return FZ_REPLY_WOULDBLOCK;
}

int CSftpControlSocket::RemoveDirParseResponse(bool successful, std::wstring const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::RemoveDirParseResponse"));

	if (!m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty m_pCurOpData"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!successful) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	CSftpRemoveDirOpData *pData = static_cast<CSftpRemoveDirOpData *>(m_pCurOpData);
	if (pData->path.empty())
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Info, _T("Empty pData->path"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	engine_.GetDirectoryCache().RemoveDir(currentServer_, pData->path, pData->subDir, engine_.GetPathCache().Lookup(currentServer_, pData->path, pData->subDir));
	SendDirectoryListingNotification(pData->path, false, false);

	return ResetOperation(FZ_REPLY_OK);
}

class CSftpChmodOpData final : public COpData
{
public:
	CSftpChmodOpData(const CChmodCommand& command)
		: COpData(Command::chmod), m_cmd(command)
	{
	}

	CChmodCommand m_cmd;
	bool m_useAbsolute{};
};

enum chmodStates
{
	chmod_init = 0,
	chmod_chmod
};

int CSftpControlSocket::Chmod(const CChmodCommand& command)
{
	if (m_pCurOpData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData not empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	LogMessage(MessageType::Status, _("Set permissions of '%s' to '%s'"), command.GetPath().FormatFilename(command.GetFile()), command.GetPermission());

	CSftpChmodOpData *pData = new CSftpChmodOpData(command);
	pData->opState = chmod_chmod;
	Push(pData);

	int res = ChangeDir(command.GetPath());
	if (res != FZ_REPLY_OK)
		return res;

	return SendNextCommand();
}

int CSftpControlSocket::ChmodParseResponse(bool successful, std::wstring const&)
{
	CSftpChmodOpData *pData = static_cast<CSftpChmodOpData*>(m_pCurOpData);
	if (!pData)
	{
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!successful)
	{
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

int CSftpControlSocket::ChmodSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ChmodSend()"));

	CSftpChmodOpData *pData = static_cast<CSftpChmodOpData*>(m_pCurOpData);
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

int CSftpControlSocket::ChmodSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ChmodSend()"));

	CSftpChmodOpData *pData = static_cast<CSftpChmodOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case chmod_chmod:
		{
			engine_.GetDirectoryCache().UpdateFile(currentServer_, pData->m_cmd.GetPath(), pData->m_cmd.GetFile(), false, CDirectoryCache::unknown);

			std::wstring quotedFilename = QuoteFilename(pData->m_cmd.GetPath().FormatFilename(pData->m_cmd.GetFile(), !pData->m_useAbsolute));

			res = SendCommand(L"chmod " + pData->m_cmd.GetPermission() + L" " + WildcardEscape(quotedFilename),
					   L"chmod " + pData->m_cmd.GetPermission() + L" " + quotedFilename);
		}
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

class CSftpRenameOpData final : public COpData
{
public:
	CSftpRenameOpData(const CRenameCommand& command)
		: COpData(Command::rename), m_cmd(command)
	{
	}

	CRenameCommand m_cmd;
	bool m_useAbsolute{};
};

enum renameStates
{
	rename_init = 0,
	rename_rename
};

int CSftpControlSocket::Rename(const CRenameCommand& command)
{
	if (m_pCurOpData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData not empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	LogMessage(MessageType::Status, _("Renaming '%s' to '%s'"), command.GetFromPath().FormatFilename(command.GetFromFile()), command.GetToPath().FormatFilename(command.GetToFile()));

	CSftpRenameOpData *pData = new CSftpRenameOpData(command);
	pData->opState = rename_rename;
	Push(pData);

	int res = ChangeDir(command.GetFromPath());
	if (res != FZ_REPLY_OK) {
		return res;
	}

	return SendNextCommand();
}

int CSftpControlSocket::RenameParseResponse(bool successful, std::wstring const&)
{
	CSftpRenameOpData *pData = static_cast<CSftpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!successful) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	const CServerPath& fromPath = pData->m_cmd.GetFromPath();
	const CServerPath& toPath = pData->m_cmd.GetToPath();

	engine_.GetDirectoryCache().Rename(currentServer_, fromPath, pData->m_cmd.GetFromFile(), toPath, pData->m_cmd.GetToFile());

	SendDirectoryListingNotification(fromPath, false, false);
	if (fromPath != toPath) {
		SendDirectoryListingNotification(toPath, false, false);
	}

	ResetOperation(FZ_REPLY_OK);
	return FZ_REPLY_OK;
}

int CSftpControlSocket::RenameSubcommandResult(int prevResult)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::RenameSubcommandResult()"));

	CSftpRenameOpData *pData = static_cast<CSftpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (prevResult != FZ_REPLY_OK) {
		pData->m_useAbsolute = true;
	}

	return SendNextCommand();
}

int CSftpControlSocket::RenameSend()
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::RenameSend()"));

	CSftpRenameOpData *pData = static_cast<CSftpRenameOpData*>(m_pCurOpData);
	if (!pData) {
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("m_pCurOpData empty"));
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	bool res;
	switch (pData->opState)
	{
	case rename_rename:
		{
			bool wasDir = false;
			engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile(), &wasDir);
			engine_.GetDirectoryCache().InvalidateFile(currentServer_, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			std::wstring fromQuoted = QuoteFilename(pData->m_cmd.GetFromPath().FormatFilename(pData->m_cmd.GetFromFile(), !pData->m_useAbsolute));
			std::wstring toQuoted = QuoteFilename(pData->m_cmd.GetToPath().FormatFilename(pData->m_cmd.GetToFile(), !pData->m_useAbsolute && pData->m_cmd.GetFromPath() == pData->m_cmd.GetToPath()));

			engine_.GetPathCache().InvalidatePath(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
			engine_.GetPathCache().InvalidatePath(currentServer_, pData->m_cmd.GetToPath(), pData->m_cmd.GetToFile());

			if (wasDir) {
				// Need to invalidate current working directories
				CServerPath path = engine_.GetPathCache().Lookup(currentServer_, pData->m_cmd.GetFromPath(), pData->m_cmd.GetFromFile());
				if (path.empty()) {
					path = pData->m_cmd.GetFromPath();
					path.AddSegment(pData->m_cmd.GetFromFile());
				}
				engine_.InvalidateCurrentWorkingDirs(path);
			}

			res = SendCommand(L"mv " + WildcardEscape(fromQuoted) + L" " + toQuoted,
					   L"mv " + fromQuoted + L" " + toQuoted);
		}
		break;
	default:
		LogMessage(__TFILE__, __LINE__, this, MessageType::Debug_Warning, _T("unknown op state: %d"), pData->opState);
		ResetOperation(FZ_REPLY_INTERNALERROR);
		return FZ_REPLY_ERROR;
	}

	if (!res) {
		ResetOperation(FZ_REPLY_ERROR);
		return FZ_REPLY_ERROR;
	}

	return FZ_REPLY_WOULDBLOCK;
}

std::wstring CSftpControlSocket::WildcardEscape(std::wstring const& file)
{
	std::wstring ret;
	// see src/putty/wildcard.c

	ret.reserve(file.size());
	for (size_t i = 0; i < file.size(); ++i) {
		auto const& c = file[i];
		switch (c)
		{
		case '[':
		case ']':
		case '*':
		case '?':
		case '\\':
			ret.push_back('\\');
			break;
		default:
			break;
		}
		ret.push_back(c);
	}
	return ret;
}

void CSftpControlSocket::OnRateAvailable(CRateLimiter::rate_direction direction)
{
	OnQuotaRequest(direction);
}

void CSftpControlSocket::OnQuotaRequest(CRateLimiter::rate_direction direction)
{
	int64_t bytes = GetAvailableBytes(direction);
	if (bytes > 0) {
		int b;
		if (bytes > INT_MAX) {
			b = INT_MAX;
		}
		else {
			b = bytes;
		}
		AddToStream(fz::sprintf(L"-%d%d,%d\n", direction, b, engine_.GetOptions().GetOptionVal(OPTION_SPEEDLIMIT_INBOUND + static_cast<int>(direction))));
		UpdateUsage(direction, b);
	}
	else if (bytes == 0) {
		Wait(direction);
	}
	else if (bytes < 0) {
		AddToStream(fz::sprintf(L"-%d-\n", direction));
	}
}


int CSftpControlSocket::ParseSubcommandResult(int prevResult, COpData const&)
{
	LogMessage(MessageType::Debug_Verbose, _T("CSftpControlSocket::ParseSubcommandResult(%d)"), prevResult);
	if (!m_pCurOpData) {
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

void CSftpControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<CSftpEvent, CTerminateEvent>(ev, this,
		&CSftpControlSocket::OnSftpEvent,
		&CSftpControlSocket::OnTerminate)) {
		return;
	}

	CControlSocket::operator()(ev);
}
