#include "FileZilla.h"
#include "Options.h"
#include "../tinyxml/tinyxml.h"
#include "xmlfunctions.h"
#include "filezillaapp.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

COptions* COptions::m_theOptions = 0;

enum Type
{
	string,
	number
};

struct t_Option
{
	char name[30];
	enum Type type;
	wxString defaultValue; // Default values are stored as string even for numerical options
};

static const t_Option options[OPTIONS_NUM] =
{
	// Engine settings
	{ "Use Pasv mode", number, _T("1") },
	{ "Limit local ports", number, _T("0") },
	{ "Limit ports low", number, _T("6000") },
	{ "Limit ports high", number, _T("7000") },
	{ "External IP mode", number, _T("0") },
	{ "External IP", string, _T("") },
	{ "External address resolver", string, _T("http://ip.filezilla-project.org/ip.php") },
	{ "Last resolved IP", string, _T("") },
	{ "No external ip on local conn", number, _T("1") },
	{ "Pasv reply fallback mode", number, _T("0") },
	{ "Timeout", number, _T("15") },
	{ "Logging Debug Level", number, _T("0") },
	{ "Logging Raw Listing", number, _T("0") },
	{ "fzsftp executable", string, _T("") },
	{ "Allow transfermode fallback", number, _T("1") },
	{ "Reconnect count", number, _T("2") },
	{ "Reconnect delay", number, _T("5") },
	{ "Speedlimit inbound", number, _T("0") },
	{ "Speedlimit outbound", number, _T("0") },

	// Interface settings
	{ "Number of Transfers", number, _T("2") },
	{ "Transfer Retry Count", number, _T("5") },
	{ "Ascii Binary mode", number, _T("0") },
	{ "Auto Ascii files", string, _T("am|asp|bat|c|cfm|cgi|conf|cpp|css|dhtml|diz|h|hpp|htm|html|in|inc|js|m4|mak|nfo|nsi|pas|patch|php|phtml|pl|po|py|qmail|sh|shtml|sql|tcl|tpl|txt|vbs|xml|xrc") },
	{ "Auto Ascii no extension", number, _T("1") },
	{ "Auto Ascii dotfiles", number, _T("1") },
	{ "Theme", string, _T("") },
	{ "Language", string, _T("") },
	{ "Last Server Path", string, _T("") },
	{ "Max Concurrent Uploads", number, _T("0") },
	{ "Max Concurrent Downloads", number, _T("0") },
	{ "Update Check", number, _T("1") },
	{ "Update Check Interval", number, _T("7") },
	{ "Last automatic update check", string, _T("") },
	{ "Update Check New Version", string, _T("") },
	{ "Update Check Package URL", string, _T("") },
	{ "Show debug menu", number, _T("0") },
	{ "File exists action download", number, _T("0") },
	{ "File exists action upload", number, _T("0") },
	{ "Allow ascii resume", number, _T("0") },
	{ "Greeting version", string, _T("") },
	{ "Onetime Dialogs", string, _T("") },
	{ "Show Tree Local", number, _T("1") },
	{ "Show Tree Remote", number, _T("1") },
	{ "File Pane Layout", number, _T("0") },
	{ "File Panw Swap", number, _T("0") }
};

COptions::COptions()
{
	m_pLastServer = 0;

	m_acquired = false;

	for (unsigned int i = 0; i < OPTIONS_NUM; i++)
		m_optionsCache[i].cached = false;

	wxFileName file = wxFileName(wxGetApp().GetSettingsDir(), _T("filezilla.xml"));
	m_pXmlDocument = GetXmlFile(file)->GetDocument();

	if (!m_pXmlDocument)
	{
		wxString msg = wxString::Format(_("Could not load \"%s\", make sure the file is valid.\nFor this session, default settings will be used and any changes to the settings are not persistent."), file.GetFullPath().c_str());
		wxMessageBox(msg, _("Error loading xml file"), wxICON_ERROR);
	}
	else
		CreateSettingsXmlElement();
}

COptions::~COptions()
{
	delete m_pLastServer;
	delete m_pXmlDocument;
}

int COptions::GetOptionVal(unsigned int nID)
{
	if (nID >= OPTIONS_NUM)
		return 0;

	if (options[nID].type != number)
		return 0;

	if (m_optionsCache[nID].cached)
		return m_optionsCache[nID].numValue;

	wxString value;
	long numValue = 0;
	if (!GetXmlValue(nID, value))
		options[nID].defaultValue.ToLong(&numValue);
	else
	{
		value.ToLong(&numValue);
		numValue = Validate(nID, numValue);
	}

	m_optionsCache[nID].numValue = numValue;
	m_optionsCache[nID].cached = true;

	return numValue;
}

wxString COptions::GetOption(unsigned int nID)
{
	if (nID >= OPTIONS_NUM)
		return _T("");

	if (options[nID].type != string)
		return wxString::Format(_T("%d"), GetOptionVal(nID));

	if (m_optionsCache[nID].cached)
		return m_optionsCache[nID].strValue;

	wxString value;
	if (!GetXmlValue(nID, value))
		value = options[nID].defaultValue;
	else
		Validate(nID, value);

	m_optionsCache[nID].strValue = value;
	m_optionsCache[nID].cached = true;

	return value;
}

bool COptions::SetOption(unsigned int nID, int value)
{
	if (nID >= OPTIONS_NUM)
		return false;

	if (options[nID].type != number)
		return false;

	Validate(nID, value);

	m_optionsCache[nID].cached = true;
	m_optionsCache[nID].numValue = value;

	if (m_pXmlDocument)
	{
		SetXmlValue(nID, wxString::Format(_T("%d"), value));
		wxFileName file = wxFileName(wxGetApp().GetSettingsDir(), _T("filezilla.xml"));
		m_pXmlDocument->SaveFile(file.GetFullPath().mb_str());
	}

	return true;
}

bool COptions::SetOption(unsigned int nID, wxString value)
{
	if (nID >= OPTIONS_NUM)
		return false;

	if (options[nID].type != string)
	{
		long tmp;
		if (!value.ToLong(&tmp))
			return false;

		return SetOption(nID, tmp);
	}

	Validate(nID, value);

	m_optionsCache[nID].cached = true;
	m_optionsCache[nID].strValue = value;

	if (m_pXmlDocument)
	{
		SetXmlValue(nID, value);
		wxFileName file = wxFileName(wxGetApp().GetSettingsDir(), _T("filezilla.xml"));
		m_pXmlDocument->SaveFile(file.GetFullPath().mb_str());
	}

	return true;
}

void COptions::CreateSettingsXmlElement()
{
	if (!m_pXmlDocument)
		return;

	if (m_pXmlDocument->FirstChildElement("FileZilla3")->FirstChildElement("Settings"))
		return;

	TiXmlElement *element = m_pXmlDocument->FirstChildElement("FileZilla3");
	TiXmlElement settings("Settings");
	element->InsertEndChild(settings);

	for (int i = 0; i < OPTIONS_NUM; i++)
	{
		m_optionsCache[i].cached = true;
		if (options[i].type == string)
			m_optionsCache[i].strValue = options[i].defaultValue;
		else
		{
			long numValue = 0;
			options[i].defaultValue.ToLong(&numValue);
			m_optionsCache[i].numValue = numValue;
		}
		SetXmlValue(i, options[i].defaultValue);
	}

	wxFileName file = wxFileName(wxGetApp().GetSettingsDir(), _T("filezilla.xml"));
	m_pXmlDocument->SaveFile(file.GetFullPath().mb_str());
}

void COptions::SetXmlValue(unsigned int nID, wxString value)
{
	wxASSERT(m_pXmlDocument);
	if (!m_pXmlDocument)
		return;

	// No checks are made about the validity of the value, that's done in SetOption

	char *utf8 = ConvUTF8(value);
	if (!utf8)
		return;

	TiXmlElement *settings = m_pXmlDocument->FirstChildElement("FileZilla3")->FirstChildElement("Settings");
	if (!settings)
	{
		TiXmlNode *node = m_pXmlDocument->FirstChildElement("FileZilla3")->InsertEndChild(TiXmlElement("Settings"));
		if (!node)
		{
			delete [] utf8;
			return;
		}
		settings = node->ToElement();
		if (!settings)
		{
			delete [] utf8;
			return;
		}
	}
	else
	{
		TiXmlNode *node = 0;
		while ((node = settings->IterateChildren("Setting", node)))
		{
			TiXmlElement *setting = node->ToElement();
			if (!setting)
				continue;

			const char *attribute = setting->Attribute("name");
			if (!attribute)
				continue;
			if (strcmp(attribute, options[nID].name))
				continue;

			setting->RemoveAttribute("type");
			setting->Clear();
			setting->SetAttribute("type", (options[nID].type == string) ? "string" : "number");
			setting->InsertEndChild(TiXmlText(utf8));

			delete [] utf8;
			return;
		}
	}
	TiXmlElement setting("Setting");
	setting.SetAttribute("name", options[nID].name);
	setting.SetAttribute("type", (options[nID].type == string) ? "string" : "number");
	setting.InsertEndChild(TiXmlText(utf8));
	settings->InsertEndChild(setting);

	delete [] utf8;
}

bool COptions::GetXmlValue(unsigned int nID, wxString &value)
{
	wxASSERT(m_pXmlDocument);
	if (!m_pXmlDocument)
		return false;

	TiXmlElement *settings = m_pXmlDocument->FirstChildElement("FileZilla3")->FirstChildElement("Settings");
	if (!settings)
	{
		TiXmlNode *node = m_pXmlDocument->FirstChildElement("FileZilla3")->InsertEndChild(TiXmlElement("Settings"));
		if (!node)
			return false;
		settings = node->ToElement();
		if (!settings)
			return false;
	}

	TiXmlNode *node = 0;
	while ((node = settings->IterateChildren("Setting", node)))
	{
		TiXmlElement *setting = node->ToElement();
		if (!setting)
			continue;

		const char *attribute = setting->Attribute("name");
		if (!attribute)
			continue;
		if (strcmp(attribute, options[nID].name))
			continue;

		TiXmlNode *text = setting->FirstChild();
		if (!text || !text->ToText())
			return false;

		value = ConvLocal(text->Value());

		return true;
	}

	return false;
}

int COptions::Validate(unsigned int nID, int value)
{
	switch (nID)
	{
	case OPTION_UPDATECHECK_INTERVAL:
		if (value < 7 || value > 9999)
			value = 7;
		break;
	case OPTION_LOGGING_DEBUGLEVEL:
		if (value < 0 || value > 4)
			value = 0;
		break;
	case OPTION_RECONNECTCOUNT:
		if (value < 0 || value > 99)
			value = 5;
		break;
	case OPTION_RECONNECTDELAY:
		if (value < 0 || value > 999)
			value = 5;
		break;
	case OPTION_FILEPANE_LAYOUT:
		if (value < 0 || value > 2)
			value = 0;
		break;
	}
	return value;
}

wxString COptions::Validate(unsigned int nID, wxString value)
{
	return value;
}

TiXmlElement *COptions::GetXml()
{
	if (m_acquired)
		return 0;

	if (!m_pXmlDocument)
		return 0;

	return m_pXmlDocument->FirstChildElement("FileZilla3");
}

void COptions::FreeXml(bool save)
{
	if (!m_pXmlDocument)
		return;

	m_acquired = false;

	if (save)
	{
		wxFileName file = wxFileName(wxGetApp().GetSettingsDir(), _T("filezilla.xml"));
		m_pXmlDocument->SaveFile(file.GetFullPath().mb_str());
	}
}

void COptions::SetServer(wxString path, const CServer& server)
{
	if (path == _T(""))
		return;

	TiXmlElement *element = GetXml();

	while (path != _T(""))
	{
		wxString sub;
		int pos = path.Find('/');
		if (pos != -1)
		{
			sub = path.Left(pos);
			path = path.Mid(pos + 1);
		}
		else
		{
			sub = path;
			path = _T("");
		}
		char *utf8 = ConvUTF8(sub);
		if (!utf8)
			return;
		TiXmlElement *newElement = element->FirstChildElement(utf8);
		delete [] utf8;
		if (newElement)
			element = newElement;
		else
		{
			char *utf8 = ConvUTF8(sub);
			if (!utf8)
				return;
			TiXmlNode *node = element->InsertEndChild(TiXmlElement(utf8));
			delete [] utf8;
			if (!node || !node->ToElement())
				return;
			element = node->ToElement();
		}
	}

	::SetServer(element, server);

	FreeXml(true);
}

bool COptions::GetServer(wxString path, CServer& server)
{
	if (path == _T(""))
		return false;

	TiXmlElement *element = GetXml();

	while (path != _T(""))
	{
		wxString sub;
		int pos = path.Find('/');
		if (pos != -1)
		{
			sub = path.Left(pos);
			path = path.Mid(pos + 1);
		}
		else
		{
			sub = path;
			path = _T("");
		}
		char *utf8 = ConvUTF8(sub);
		if (!utf8)
			return false;
		element = element->FirstChildElement(utf8);
		delete [] utf8;
		if (!element)
			return false;
	}

	bool res = ::GetServer(element, server);

	FreeXml(false);

	return res;
}

void COptions::SetLastServer(const CServer& server)
{
	if (!m_pLastServer)
		m_pLastServer = new CServer(server);
	else
		*m_pLastServer = server;
	SetServer(_T("Settings/LastServer"), server);
}

bool COptions::GetLastServer(CServer& server)
{
	if (!m_pLastServer)
	{
		bool res = GetServer(_T("Settings/LastServer"), server);
		if (res)
			m_pLastServer = new CServer(server);
		return res;
	}
	else
	{
		server = *m_pLastServer;
		return true;
	}
}

void COptions::Init()
{
	if (!m_theOptions)
		m_theOptions = new COptions();
}

void COptions::Destroy()
{
	if (!m_theOptions)
		return;

	delete m_theOptions;
	m_theOptions = 0;
}

COptions* COptions::Get()
{
	return m_theOptions;
}
