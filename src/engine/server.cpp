#include "FileZilla.h"

CServer::CServer()
{
	m_Port = 21;
	m_Protocol = FTP;
	m_Type = DEFAULT;
	m_logonType = ANONYMOUS;
}

bool CServer::ParseUrl(wxString host, int port, wxString user, wxString pass, wxString &error, CServerPath &path)
{
	m_Type = DEFAULT;

	if (host == _T(""))
	{
		error = _("No host given, please enter a host.");
		return false;
	}

	int pos = host.Find(_T("://"));
	if (pos != -1)
	{
		wxString protocol = host.Left(pos).Lower();
		host = host.Mid(pos + 3);
		if (protocol.Left(3) == _T("fz_"))
			protocol = protocol.Mid(3);
		if (protocol == _T("ftp"))
			m_Protocol = FTP;
		else
		{
			error = _("Invalid protocol specified. Only valid protocol is ftp:// at the moment.");
			return false;
		}
	}
	else
		m_Protocol = FTP;

	pos = host.Find('@');
	if (pos != -1)
	{
		user = host.Left(pos);
		host = host.Mid(pos + 1);

		pos = user.Find(':');
		if (pos != -1)
		{
			pass = user.Mid(pos + 1);
			user = user.Left(pos);
		}

		if (user == _T(""))
		{
			error = _("Invalid username given.");
			return false;
		}
	}
	else
	{
		if (user == _T(""))
		{
			user = _T("anonymous");
			pass = _T("anonymous@example.com");
		}
	}

	pos = host.Find('/');
	if (pos != -1)
	{
		path = CServerPath(host.Mid(pos));
		host = host.Left(pos);
	}

	pos = host.Find(':');
	if (pos != -1)
	{
		if (!pos)
		{
			error = _("No host given, please enter a host.");
			return false;
		}

		long tmp;
		if (!host.Mid(pos + 1).ToLong(&tmp) || tmp < 1 || tmp > 65535)
		{
			error = _("Invalid port given. The port has to be a value from 1 to 65535.");
			return false;
		}
		port = tmp;

		host = host.Left(pos);
	}
	else
	{
		if (port == -1)
		{
			if (m_Protocol == FTP)
				port = 21;
			else
				port = 21;
		}
		else if (port < 1 || port > 65535)
		{
			error = _("Invalid port given. The port has to be a value from 1 to 65535.");
			return false;
		}
	}

	if (host == _T(""))
	{
		error = _("No host given, please enter a host.");
		return false;
	}

	m_Host = host;
	m_Port = port;
	m_User = user;
	m_Pass = pass;
	if (m_User == _T("") || m_User == _T("anonymous"))
		m_logonType = ANONYMOUS;
	else
		m_logonType = NORMAL;

	return true;
}

ServerProtocol CServer::GetProtocol() const
{
	return m_Protocol;
}

ServerType CServer::GetType() const
{
	return m_Type;
}

wxString CServer::GetHost() const
{
	return m_Host;
}

int CServer::GetPort() const
{
	return m_Port;
}

wxString CServer::GetUser() const
{
	if (m_logonType == ANONYMOUS)
		return _T("anonymous");
	
	return m_User;
}

wxString CServer::GetPass() const
{
	if (m_logonType == ANONYMOUS)
		return _T("anon@localhost");
	
	return m_Pass;
}

CServer& CServer::operator=(const CServer &op)
{
	m_Protocol = op.m_Protocol;
	m_Type = op.m_Type;
	m_Host = op.m_Host;
	m_Port = op.m_Port;
	m_logonType = op.m_logonType;
	m_User = op.m_User;
	m_Pass = op.m_Pass;

	return *this;
}

bool CServer::operator==(const CServer &op) const
{
	if (m_Protocol != op.m_Protocol)
		return false;
	else if (m_Type != op.m_Type)
		return false;
	else if (m_Host != op.m_Host)
		return false;
	else if (m_Port != op.m_Port)
		return false;
	else if (m_logonType != op.m_logonType)
		return false;
	else if (m_logonType != ANONYMOUS)
	{
		if (m_User != op.m_User)
			return false;
		else if (m_Pass != op.m_Pass)
			return false;
	}

	return true;
}

bool CServer::operator!=(const CServer &op) const
{
	return !(*this == op);
}

CServer::CServer(enum ServerProtocol protocol, enum ServerType type, wxString host, int port, wxString user, wxString pass /*=_T("")*/)
{
	m_Protocol = protocol;
	m_Type = type;
	m_Host = host;
	m_Port = port;
	m_logonType = NORMAL;
	m_User = user;
	m_Pass = pass;
}

CServer::CServer(enum ServerProtocol protocol, enum ServerType type, wxString host, int port)
{
	m_Protocol = protocol;
	m_Type = type;
	m_Host = host;
	m_Port = port;
	m_logonType = ANONYMOUS;
}

void CServer::SetType(enum ServerType type)
{
	m_Type = type;
}

enum LogonType CServer::GetLogonType() const
{
	return m_logonType;
}
