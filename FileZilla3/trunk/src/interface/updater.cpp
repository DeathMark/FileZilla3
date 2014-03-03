#include <filezilla.h>

#if FZ_MANUALUPDATECHECK

#include "buildinfo.h"
#include "updater.h"
#include "Options.h"
#include <local_filesys.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#ifdef __WXMSW__
#include <wx/dynlib.h> // Used by GetDownloadDir
#endif //__WXMSW__

// This is ugly but does the job
#define SHA512_STANDALONE
typedef unsigned int uint32;
namespace {
#include "../putty/int64.h"
#include "../putty/sshsh512.c"
}

BEGIN_EVENT_TABLE(CUpdater, wxEvtHandler)
EVT_FZ_NOTIFICATION(wxID_ANY, CUpdater::OnEngineEvent)
EVT_TIMER(wxID_ANY, CUpdater::OnTimer)
END_EVENT_TABLE()

static wxChar s_update_cert[] = _T("-----BEGIN CERTIFICATE-----\n\
MIIFsTCCA5ugAwIBAgIESnXLbzALBgkqhkiG9w0BAQ0wSTELMAkGA1UEBhMCREUx\n\
GjAYBgNVBAoTEUZpbGVaaWxsYSBQcm9qZWN0MR4wHAYDVQQDExVmaWxlemlsbGEt\n\
cHJvamVjdC5vcmcwHhcNMDkwODAyMTcyMjU2WhcNMzEwNjI4MTcyMjU4WjBJMQsw\n\
CQYDVQQGEwJERTEaMBgGA1UEChMRRmlsZVppbGxhIFByb2plY3QxHjAcBgNVBAMT\n\
FWZpbGV6aWxsYS1wcm9qZWN0Lm9yZzCCAh8wCwYJKoZIhvcNAQEBA4ICDgAwggIJ\n\
AoICAJqWXy7YzVP5pOk8VB9bd/ROC9SVbAxJiFHh0I0/JmyW+jSfzFCYWr1DKGVv\n\
Oui+qiUsaSgjWTh/UusnVu4Q4Lb00k7INRF6MFcGFkGNmOZPk4Qt0uuWMtsxiFek\n\
9QMPWSYs+bxk+M0u0rNOdAblsIzeV16yhfUQDtrJxPWbRpuLgp9/4/oNbixet7YM\n\
pvwlns2o1KXcsNcBcXraux5QmnD4oJVYbTY2qxdMVyreA7dxd40c55F6FvA+L36L\n\
Nv54VwRFSqY12KBG4I9Up+c9OQ9HMN0zm0FhYtYeKWzdMIRk06EKAxO7MUIcip3q\n\
7v9eROPnKM8Zh4dzkWnCleirW8EKFEm+4+A8pDqirMooiQqkkMesaJDV361UCoVo\n\
fRhqfK+Prx0BaJK/5ZHN4tmgU5Tmq+z2m7aIKwOImj6VF3somVvmh0G/othnU2MH\n\
GB7qFrIUMZc5VhrAwmmSA2Z/w4+0ToiR+IrdGmDKz3cVany3EZAzWRJUARaId9FH\n\
v/ymA1xcFAKmfxsjGNlNpXd7b8UElS8+ccKL9m207k++IIjc0jUPgrM70rU3cv5M\n\
Kevp971eHLhpWa9vrjbz/urDzBg3Dm8XEN09qwmABfIEnhm6f7oz2bYXjz73ImYj\n\
rZsogz+Jsx3NWhHFUD42iA4ZnxHIEgchD/TAihpbdrEhgmdvAgMBAAGjgacwgaQw\n\
EgYDVR0TAQH/BAgwBgEB/wIBAjAmBgNVHREEHzAdgRthZG1pbkBmaWxlemlsbGEt\n\
cHJvamVjdC5vcmcwDwYDVR0PAQH/BAUDAwcGADAdBgNVHQ4EFgQUd4w2verFjXAn\n\
CrNLor39nFtemNswNgYDVR0fBC8wLTAroCmgJ4YlaHR0cHM6Ly9jcmwuZmlsZXpp\n\
bGxhLXByb2plY3Qub3JnL2NybDALBgkqhkiG9w0BAQ0DggIBAF3fmV/Bs4amV78d\n\
uhe5PkW7yTO6iCfKJVDB22kXPvL0rzZn4SkIZNoac8Xl5vOoRd6k+06i3aJ78w+W\n\
9Z0HK1jUdjW7taYo4bU58nAp3Li+JwjE/lUBNqSKSescPjdZW0KzIIZls91W30yt\n\
tGq85oWAuyVprHPlr2uWLg1q4eUdF6ZAz4cZ0+9divoMuk1HiWxi1Y/1fqPRzUFf\n\
UGK0K36iPPz2ktzT7qJYXRfC5QDoX7tCuoDcO5nccVjDypRKxy45O5Ucm/fywiQW\n\
NQfz/yQAmarQSCfDjNcHD1rdJ0lx9VWP6xi+Z8PGSlR9eDuMaqPVAE1DLHwMMTTZ\n\
93PbfP2nvgbElgEki28LUalyVuzvrKcu/rL1LnCJA4jStgE/xjDofpYwgtG4ZSnE\n\
KgNy48eStvNZbGhwn2YvrxyKmw58WSQG9ArOCHoLcWnpedSZuTrPTLfgNUx7DNbo\n\
qJU36tgxiO0XLRRSetl7jkSIO6U1okVH0/tvstrXEWp4XwdlmoZf92VVBrkg3San\n\
fA5hBaI2gpQwtpyOJzwLzsd43n4b1YcPiyzhifJGcqRCBZA1uArNsH5iG6z/qHXp\n\
KjuMxZu8aM8W2gp8Yg8QZfh5St/nut6hnXb5A8Qr+Ixp97t34t264TBRQD6MuZc3\n\
PqQuF7sJR6POArUVYkRD/2LIWsB7\n\
-----END CERTIFICATE-----\n\
");


void version_information::update_available()
{
	if( !nightly_.url_.empty() && COptions::Get()->GetOptionVal(OPTION_UPDATECHECK_CHECKBETA) == 2 ) {
		available_ = nightly_;
	}
	else if( !beta_.version_.empty() && COptions::Get()->GetOptionVal(OPTION_UPDATECHECK_CHECKBETA) != 0 ) {
		available_ = beta_;
	}
	else if( !stable_.version_.empty() ) {
		available_ = stable_;
	}
	else {
		available_ = build();
	}
}


class CUpdaterOptions : public COptionsBase
{
public:
	CUpdaterOptions()
		: m_use_internal_rootcert(true)
	{
	}

	virtual int GetOptionVal(unsigned int nID)
	{
		return COptions::Get()->GetOptionVal(nID);
	}

	virtual wxString GetOption(unsigned int nID)
	{
		if (nID == OPTION_INTERNAL_ROOTCERT && m_use_internal_rootcert)
			return s_update_cert;

		return COptions::Get()->GetOption(nID);
	}

	virtual bool SetOption(unsigned int nID, int value)
	{
		return COptions::Get()->SetOption(nID, value);
	}

	virtual bool SetOption(unsigned int nID, wxString value)
	{
		return COptions::Get()->SetOption(nID, value);
	}

	bool m_use_internal_rootcert;
};

CUpdater::CUpdater(CUpdateHandler& parent)
	: state_(idle)
	, engine_(new CFileZillaEngine)
	, update_options_(new CUpdaterOptions())
{
	AddHandler(parent);
	engine_->Init(this, update_options_);

	raw_version_information_ = COptions::Get()->GetOption( OPTION_UPDATECHECK_NEWVERSION );
	ParseData();

	if( !version_information_.empty() ) {
		SetState(newversion);
	}

	Run();
}

CUpdater::~CUpdater()
{
	delete engine_;
	delete update_options_;
}

wxString CUpdater::GetUrl()
{
	wxString host = CBuildInfo::GetHostname();
	if (host == _T(""))
		host = _T("unknown");

	wxString version(PACKAGE_VERSION, wxConvLocal);
	version.Replace(_T(" "), _T("%20"));

	wxString url = wxString::Format(_T("https://update.filezilla-project.org/update.php?platform=%s&version=%s"), host.c_str(), version.c_str());
#if defined(__WXMSW__) || defined(__WXMAC__)
	// Makes not much sense to submit OS version on Linux, *BSD and the likes, too many flavours.
	wxString osVersion = wxString::Format(_T("&osversion=%d.%d"), wxPlatformInfo::Get().GetOSMajorVersion(), wxPlatformInfo::Get().GetOSMinorVersion());
	url += osVersion;
#endif

#ifdef __WXMSW__
	if (wxIsPlatform64Bit())
		url += _T("&osarch=64");
	else
		url += _T("&osarch=32");
#endif
	return url;
}

bool CUpdater::Run()
{
	if( state_ != idle && state_ != failed && state_ != newversion && state_ != newversion_ready ) {
		return false;
	}

	local_file_.clear();
	SetState(checking);

	update_options_->m_use_internal_rootcert = true;
	int res = Download(GetUrl(), _T(""));

	if (res != FZ_REPLY_WOULDBLOCK) {
		SetState(failed);
	}
	raw_version_information_.clear();

	return state_ == checking;
}

int CUpdater::Download(wxString const& url, wxString const& local_file)
{
	if( state_ != checking && state_ != newversion_downloading && state_ != checking ) {
		return FZ_REPLY_INTERNALERROR;
	}

	engine_->Command(CDisconnectCommand());
	int res = SendConnectCommand(url);
	if( res == FZ_REPLY_OK ) {
		res = SendTransferCommand(url, local_file);
	}

	return res;
}

int CUpdater::SendConnectCommand(wxString const& url)
{
	if( state_ != checking && state_ != newversion_downloading && state_ != checking ) {
		return FZ_REPLY_INTERNALERROR;
	}

	CServer s;
	CServerPath path;
	wxString error;
	if( !s.ParseUrl( url, 0, _T(""), _T(""), error, path ) || (s.GetProtocol() != HTTP && s.GetProtocol() != HTTPS) ) {
		return FZ_REPLY_ERROR;
	}

	return engine_->Command(CConnectCommand(s));
}

int CUpdater::SendTransferCommand(wxString const& url, wxString const& local_file)
{
	if( state_ != checking && state_ != newversion_downloading && state_ != checking ) {
		return FZ_REPLY_INTERNALERROR;
	}

	CFileTransferCommand::t_transferSettings transferSettings;

	CServer s;
	CServerPath path;
	wxString error;
	if( !s.ParseUrl( url, 0, _T(""), _T(""), error, path ) || (s.GetProtocol() != HTTP && s.GetProtocol() != HTTPS) ) {
		return FZ_REPLY_ERROR;
	}
	wxString file = path.GetLastSegment();
	path = path.GetParent();

	CFileTransferCommand cmd(local_file, path, file, true, transferSettings);
	int res = engine_->Command(cmd);

	wxASSERT(res != FZ_REPLY_OK);
	return res;
}

void CUpdater::OnEngineEvent(wxEvent& ev)
{
	if (!engine_)
		return;

	CNotification *notification; 
	while( (notification = engine_->GetNextNotification()) ) {
		ProcessNotification(notification);
		delete notification;
	}
}

void CUpdater::ProcessNotification(CNotification* notification)
{
	if (state_ != checking && state_ != newversion_downloading) {
		return;
	}

	switch (notification->GetID())
	{
	case nId_asyncrequest:
		{
			CAsyncRequestNotification* pData = reinterpret_cast<CAsyncRequestNotification *>(notification);
			if (pData->GetRequestID() == reqId_fileexists) {
				reinterpret_cast<CFileExistsNotification *>(pData)->overwriteAction = CFileExistsNotification::resume;
			}
			else if (pData->GetRequestID() == reqId_certificate) {
				CCertificateNotification* pCertNotification = (CCertificateNotification*)pData;
				pCertNotification->m_trusted = true;
			}
			engine_->SetAsyncRequestReply(pData);
		}
		break;
	case nId_data:
		ProcessData(notification);
		break;
	case nId_operation:
		ProcessOperation(notification);
		break;
	default:
		break;
	}
}

void CUpdater::ProcessOperation(CNotification* notification)
{
	if( state_ != checking && state_ != newversion_downloading ) {
		return;
	}

	UpdaterState s = failed;

	COperationNotification* operation = reinterpret_cast<COperationNotification*>(notification);
	if (operation->nReplyCode != FZ_REPLY_OK) {
		if( state_ != checking ) {
			s = newversion;
		}
	}
	else if( state_ == checking ) {
		ParseData();

		if( version_information_.available_.version_.empty() ) {
			s = idle;
		}
		else if( !version_information_.available_.url_.empty() ) {

			wxString const temp = GetTempFile();
			wxString const local_file = GetLocalFile(version_information_.available_, true);
			if( !local_file.empty() && CLocalFileSystem::GetFileType(local_file) != CLocalFileSystem::unknown) {
				local_file_ = local_file;
				s = newversion_ready;
			}
			else {
				// We got a checksum over a secure channel already.
				update_options_->m_use_internal_rootcert = false;

				if( temp.empty() || local_file.empty() ) {
					s = newversion;
				}
				else {
					s = newversion_downloading;
					wxLongLong size = CLocalFileSystem::GetSize(temp);
					if( size >= 0 && static_cast<unsigned long long>(size.GetValue()) >= version_information_.available_.size_ ) {
						s = ProcessFinishedDownload();
					}
					else if( Download( version_information_.available_.url_, GetTempFile() ) != FZ_REPLY_WOULDBLOCK ) {
						s = newversion;
					}
				}
			}
		}
		else {
			s = newversion;
		}
	}
	else {
		s = ProcessFinishedDownload();
	}
	SetState(s);
}

UpdaterState CUpdater::ProcessFinishedDownload()
{
	UpdaterState s = newversion;

	wxString const temp = GetTempFile();
	if( temp.empty() ) {
		s = newversion;
	}
	else if( !VerifyChecksum( temp, version_information_.available_.size_, version_information_.available_.hash_ ) ) {
		wxLogNull log;
		wxRemoveFile(temp);
		s = newversion;
	}
	else {
		s = newversion_ready;

		wxString local_file = GetLocalFile( version_information_.available_, false );

		wxLogNull log;
		if (local_file.empty() || !wxRenameFile( temp, local_file, false ) ) {
			s = newversion;
			wxRemoveFile( temp );
		}
		else {
			local_file_ = local_file;
		}
	}
	return s;
}

wxString CUpdater::GetLocalFile( build const& b, bool allow_existing ) const
{
	wxString const fn = GetFilename( b.url_ );
	wxString const dl = GetDownloadDir().GetPath();
	
	int i = 1;
	wxString f = dl + fn;

	while( CLocalFileSystem::GetFileType(f) != CLocalFileSystem::unknown && (!allow_existing || !VerifyChecksum(f, b.size_, b.hash_))) {
		if( ++i > 99 ) {
			return _T("");
		}
		wxString ext;
		int pos;
		if( !fn.Right(8).CmpNoCase(_T(".tar.bz2")) ) {
			pos = fn.size() - 8;
		}
		else {
			pos = fn.Find('.', true);
		}

		if( pos == -1 ) {
			f = dl + fn + wxString::Format(_T(" (%d)"), i);
		}
		else {
			f = dl + fn.Left(pos) + wxString::Format(_T(" (%d)"), i) + fn.Mid(pos);
		}
	}

	return f;
}

void CUpdater::ProcessData(CNotification* notification)
{
	if( state_ != checking ) {
		return;
	}
	
	CDataNotification* pData = reinterpret_cast<CDataNotification*>(notification);

	int len;
	char* data = pData->Detach(len);

	if( raw_version_information_.size() + len > 131072 ) {
		engine_->Command(CCancelCommand());
		SetState(failed);
	}
	else {
		for (int i = 0; i < len; i++) {
			if (data[i] < 10 || (unsigned char)data[i] > 127) {
				SetState(failed);
				engine_->Command(CCancelCommand());
				break;
			}
		}
	}

	if( state_ == checking ) {
		raw_version_information_ += wxString(data, wxConvUTF8, len);
	}
	delete [] data;
}

void CUpdater::ParseData()
{
	const wxLongLong ownVersionNumber = CBuildInfo::ConvertToVersionNumber(CBuildInfo::GetVersion());
	version_information_ = version_information();

	wxString raw_version_information = raw_version_information_;

	while( !raw_version_information.empty() ) {
		wxString line;
		int pos = raw_version_information.Find('\n');
		if (pos != -1)
		{
			line = raw_version_information.Left(pos);
			raw_version_information = raw_version_information.Mid(pos + 1);
		}
		else
		{
			line = raw_version_information;
			raw_version_information = _T("");
		}

		wxStringTokenizer tokens(line, _T(" \t\n"),  wxTOKEN_STRTOK);
		if( !tokens.CountTokens() ) {
			// After empty line, changelog follows
			version_information_.changelog = raw_version_information;
			version_information_.changelog.Trim(true);
			version_information_.changelog.Trim(false);
			break;
		}

		if( tokens.CountTokens() != 2 && tokens.CountTokens() != 6 ) {
			continue;
		}

		wxString type = tokens.GetNextToken();
		wxString versionOrDate = tokens.GetNextToken();

		if (type == _T("nightly"))
		{
			wxDateTime nightlyDate;
			if (!nightlyDate.ParseDate(versionOrDate))
				continue;

			wxDateTime buildDate = CBuildInfo::GetBuildDate();
			if (!buildDate.IsValid() || !nightlyDate.IsValid() || nightlyDate <= buildDate)
				continue;
		}
		else
		{
			wxLongLong v = CBuildInfo::ConvertToVersionNumber(versionOrDate);
			if (v <= ownVersionNumber)
				continue;
		}

		build* b = 0;
		if( type == _T("nightly") ) {
			b = &version_information_.nightly_;
		}
		else if( type == _T("release") ) {
			b = &version_information_.stable_;
		}
		else if( type == _T("beta") ) {
			b = &version_information_.beta_;
		}
		
		if( b ) {
			b->version_ = versionOrDate;

			if( tokens.CountTokens() == 4 ) {
				wxString url = tokens.GetNextToken();
				wxString sizestr = tokens.GetNextToken();
				wxString hash_algo = tokens.GetNextToken();
				wxString hash = tokens.GetNextToken();

				if( GetFilename(url).empty() ) {
					continue;
				}

				if( hash_algo.CmpNoCase(_T("sha512")) ) {
					continue;
				}

				unsigned long long l = 0;
				if( !sizestr.ToULongLong(&l) ) {
					continue;
				}
				
				b->url_ = url;
				b->size_ = l;
				b->hash_ = hash;
			}
		}
	}

	version_information_.update_available();
	if( version_information_.empty() ) {
		raw_version_information_.clear();
	}
	
	COptions::Get()->SetOption( OPTION_UPDATECHECK_NEWVERSION, raw_version_information_ );
}

void CUpdater::OnTimer(wxTimerEvent& ev)
{
}

bool CUpdater::VerifyChecksum( wxString const& file, wxULongLong size, wxString const& checksum ) const
{
	if( file.empty() || checksum.empty() ) {
		return false;
	}

	wxLongLong filesize = CLocalFileSystem::GetSize(file);
	if( filesize < 0 || static_cast<unsigned long long>(filesize.GetValue()) != size ) {
		return false;
	}

	SHA512_State state;
	SHA512_Init(&state);

	{
		wxLogNull null;
		wxFile f;
		if (!f.Open(file)) {
			return false;
		}
		char buffer[65536];
		size_t read;
		while ((read = f.Read(buffer, sizeof(buffer))) > 0) {
			SHA512_Bytes(&state, buffer, read);
		}
		if (read < 0) {
			return false;
		}
	}

	unsigned char raw_digest[64];
	SHA512_Final(&state, raw_digest);

	wxString digest;
	for( unsigned int i = 0; i < sizeof(raw_digest); i++ ) {
		unsigned char l = raw_digest[i] >> 4;
		unsigned char r = raw_digest[i] & 0x0F;

		if (l > 9)
			digest += 'a' + l - 10;
		else
			digest += '0' + l;

		if (r > 9)
			digest += 'a' + r - 10;
		else
			digest += '0' + r;
	}

	if (checksum.CmpNoCase(digest)) {
		return false;
	}

	return true;
}

wxString CUpdater::GetTempFile() const
{
	wxASSERT( !version_information_.available_.hash_.empty() );
	wxString ret = wxFileName::GetTempDir();
	if( !ret.empty() ) {
		if( ret.Last() != wxFileName::GetPathSeparator() ) {
			ret += wxFileName::GetPathSeparator();
		}

		ret += _T("fzupdate_") + version_information_.available_.hash_.Left(16) + _T(".tmp");
	}

	return ret;
}

#ifdef __WXMSW__
// See comment a few lines below
GUID VISTASHIT_FOLDERID_Downloads = { 0x374de290, 0x123f, 0x4565, { 0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b } };
extern "C" typedef HRESULT (WINAPI *tSHGetKnownFolderPath)(const GUID& rfid, DWORD dwFlags, HANDLE hToken, PWSTR *ppszPath);
#endif

CLocalPath CUpdater::GetDownloadDir() const
{
#ifdef __WXMSW__
	// Old Vista has a profile directory for downloaded files,
	// need to get it using SHGetKnownFolderPath which we need to
	// load dynamically to preserve forward compatibility with the
	// upgrade to Windows XP.
	wxDynamicLibrary lib(_T("shell32.dll"));
	if (lib.IsLoaded() && lib.HasSymbol(_T("SHGetKnownFolderPath"))) {
		tSHGetKnownFolderPath pSHGetKnownFolderPath = (tSHGetKnownFolderPath)lib.GetSymbol(_T("SHGetKnownFolderPath"));

		PWSTR path;
		HRESULT result = pSHGetKnownFolderPath(VISTASHIT_FOLDERID_Downloads, 0, 0, &path);
		if(result == S_OK) {
			wxString dir = path;
			CoTaskMemFree(path);
			return CLocalPath(dir);
		}
	}
#endif
	return CLocalPath(wxStandardPaths::Get().GetDocumentsDir());
}

wxString CUpdater::GetFilename( wxString const& url) const
{
	wxString ret;
	int pos = url.Find('/', true);
	if( pos != -1 ) {
		ret = url.Mid(pos + 1);
	}
	pos = url.find_first_of(_T("?#"));
	if( pos != -1 ) {
		ret = url.Left(pos);
	}
#ifdef __WXMSW__
	ret.Replace(_T(":"), _T("_"));
#endif

	return ret;
}

void CUpdater::SetState( UpdaterState s )
{
	if( s != state_ ) {
		state_ = s;
		build b = version_information_.available_;
		for( std::vector<CUpdateHandler*>::iterator it = handlers_.begin(); it != handlers_.end(); ++it ) {
			(*it)->UpdaterStateChanged( s, b );
		}
	}
}

wxString CUpdater::DownloadedFile() const
{
	wxString ret;
	if( state_ == newversion_ready ) {
		ret = local_file_;
	}
	return ret;
}

void CUpdater::AddHandler( CUpdateHandler& handler )
{
	RemoveHandler(handler);
	handlers_.push_back(&handler);
}

void CUpdater::RemoveHandler( CUpdateHandler& handler )
{
	for( std::vector<CUpdateHandler*>::iterator it = handlers_.begin(); it != handlers_.end(); ++it ) {
		if( *it == &handler ) {
			handlers_.erase(it);
			return;
		}
	}
}

#endif
