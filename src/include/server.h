#ifndef FILEZILLA_ENGINE_SERVER_HEADER
#define FILEZILLA_ENGINE_SERVER_HEADER

enum ServerProtocol
{
	// Never change any existing values or user's saved sites will become
	// corrupted.
	UNKNOWN = -1,
	FTP, // FTP, attempts AUTH TLS
	SFTP,
	HTTP,
	FTPS, // Implicit SSL
	FTPES, // Explicit SSL
	HTTPS,
	INSECURE_FTP, // Insecure, as the name suggests

	MAX_VALUE = INSECURE_FTP
};

enum ServerType
{
	DEFAULT,
	UNIX,
	VMS,
	DOS, // Backslashes as preferred separator
	MVS,
	VXWORKS,
	ZVM,
	HPNONSTOP,
	DOS_VIRTUAL,
	CYGWIN,
	DOS_FWD_SLASHES, // Forwardslashes as preferred separator

	SERVERTYPE_MAX
};

enum LogonType
{
	ANONYMOUS,
	NORMAL,
	ASK, // ASK should not be sent to the engine, it's intended to be used by the interface
	INTERACTIVE,
	ACCOUNT,
	KEY,

	LOGONTYPE_MAX
};

enum PasvMode
{
	MODE_DEFAULT,
	MODE_ACTIVE,
	MODE_PASSIVE
};

enum class ServerFormat
{
	host_only,
	with_optional_port,
	with_user_and_optional_port,
	url,
	url_with_password
};

enum CharsetEncoding
{
	ENCODING_AUTO,
	ENCODING_UTF8,
	ENCODING_CUSTOM
};

class CServerPath;
class CServer final
{
public:

	// No error checking is done in the constructors
	CServer();
	CServer(wxString host, unsigned int);
	CServer(ServerProtocol protocol, ServerType type, std::wstring const& host, unsigned int);
	CServer(ServerProtocol protocol, ServerType type, std::wstring const& host, unsigned int, std::wstring const& user, std::wstring const& pass = std::wstring(), std::wstring const& account = std::wstring());

	void SetType(ServerType type);

	ServerProtocol GetProtocol() const;
	ServerType GetType() const;
	std::wstring GetHost() const;
	unsigned int GetPort() const;
	LogonType GetLogonType() const;
	std::wstring GetUser() const;
	std::wstring GetPass() const;
	std::wstring GetAccount() const;
	std::wstring GetKeyFile() const;
	int GetTimezoneOffset() const;
	PasvMode GetPasvMode() const;
	int MaximumMultipleConnections() const;
	bool GetBypassProxy() const;

	// Return true if URL could be parsed correctly, false otherwise.
	// If parsing fails, pError is filled with the reason and the CServer instance may be left an undefined state.
	bool ParseUrl(wxString host, unsigned int port, wxString user, wxString pass, wxString &error, CServerPath &path);
	bool ParseUrl(wxString host, wxString port, wxString user, wxString pass, wxString &error, CServerPath &path);

	void SetProtocol(ServerProtocol serverProtocol);
	bool SetHost(std::wstring const& host, unsigned int port);

	void SetLogonType(LogonType logonType);
	bool SetUser(std::wstring const& user, std::wstring const& pass = std::wstring());
	bool SetAccount(std::wstring const& account);
	bool SetKeyFile(std::wstring const& keyFile);

	CServer& operator=(const CServer &op);
	bool operator==(const CServer &op) const;
	bool operator<(const CServer &op) const;
	bool operator!=(const CServer &op) const;
	bool EqualsNoPass(const CServer &op) const;

	bool SetTimezoneOffset(int minutes);
	void SetPasvMode(PasvMode pasvMode);
	void MaximumMultipleConnections(int maximum);

	std::wstring Format(ServerFormat formatType) const;

	bool SetEncodingType(CharsetEncoding type, std::wstring const& encoding = std::wstring());
	bool SetCustomEncoding(std::wstring const& encoding);
	CharsetEncoding GetEncodingType() const;
	wxString GetCustomEncoding() const;

	static unsigned int GetDefaultPort(ServerProtocol protocol);
	static ServerProtocol GetProtocolFromPort(unsigned int port, bool defaultOnly = false);

	static wxString GetProtocolName(ServerProtocol protocol);
	static ServerProtocol GetProtocolFromName(const wxString& name);

	static ServerProtocol GetProtocolFromPrefix(const wxString& prefix);
	static wxString GetPrefixFromProtocol(const ServerProtocol protocol);

	// Some protocol distinguish between ASCII and binary files for line-ending
	// conversion.
	static bool ProtocolHasDataTypeConcept(ServerProtocol const protocol);

	// These commands will be executed after a successful login.
	std::vector<std::wstring> const& GetPostLoginCommands() const { return m_postLoginCommands; }
	bool SetPostLoginCommands(std::vector<std::wstring> const& postLoginCommands);
	static bool SupportsPostLoginCommands(ServerProtocol const protocol);

	void SetBypassProxy(bool val);

	// Abstract server name.
	// Not compared in ==, < and related operators
	void SetName(std::wstring const& name) { m_name = name; }
	std::wstring GetName() const { return m_name; }

	static std::wstring GetNameFromServerType(ServerType type);
	static ServerType GetServerTypeFromName(std::wstring const& name);

	static std::wstring GetNameFromLogonType(LogonType type);
	static LogonType GetLogonTypeFromName(std::wstring const& name);

	explicit operator bool() const { return !m_host.empty(); }

protected:
	void Initialize();

	ServerProtocol m_protocol;
	ServerType m_type;
	std::wstring m_host;
	unsigned int m_port;
	LogonType m_logonType;
	std::wstring m_user;
	std::wstring m_pass;
	std::wstring m_account;
	std::wstring m_keyFile;
	int m_timezoneOffset;
	PasvMode m_pasvMode;
	int m_maximumMultipleConnections;
	CharsetEncoding m_encodingType;
	std::wstring m_customEncoding;
	std::wstring m_name;

	std::vector<std::wstring> m_postLoginCommands;
	bool m_bypassProxy;
};

#endif
