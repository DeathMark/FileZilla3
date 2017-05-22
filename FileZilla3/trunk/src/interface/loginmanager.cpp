#include <filezilla.h>
#include "loginmanager.h"

#include "dialogex.h"
#include "filezillaapp.h"
#include "Options.h"

#include <algorithm>

CLoginManager CLoginManager::m_theLoginManager;

std::list<CLoginManager::t_passwordcache>::iterator CLoginManager::FindItem(CServer const& server, std::wstring const& challenge)
{
	return std::find_if(m_passwordCache.begin(), m_passwordCache.end(), [&](t_passwordcache const& item)
		{
			return item.host == server.GetHost() && item.port == server.GetPort() && item.user == server.GetUser() && item.challenge == challenge;
		}
	);
}

bool CLoginManager::GetPassword(ServerWithCredentials &server, bool silent, std::wstring const& name, std::wstring const& challenge, bool canRemember)
{
	if (server.credentials.logonType_ != LogonType::ask && !server.credentials.encrypted_ && (server.credentials.logonType_ != LogonType::interactive || !server.server.GetUser().empty())) {
		return true;
	}

	if (server.credentials.encrypted_) {
		// FIXME
		auto priv = private_key::from_password("hello", server.credentials.encrypted_.salt_);
		return server.credentials.Unprotect(priv);
	}

	if (canRemember) {
		auto it = FindItem(server.server, challenge);
		if (it != m_passwordCache.end()) {
			server.credentials.SetPass(it->password);
			return true;
		}
	}
	if (silent) {
		return false;
	}

	return DisplayDialog(server, name, challenge, canRemember);
}

bool CLoginManager::DisplayDialog(ServerWithCredentials &server, std::wstring const& name, std::wstring const& challenge, bool canRemember)
{
	wxDialogEx pwdDlg;
	if (!pwdDlg.Load(wxGetApp().GetTopWindow(), _T("ID_ENTERPASSWORD"))) {
		return false;
	}

	if (name.empty()) {
		pwdDlg.GetSizer()->Show(XRCCTRL(pwdDlg, "ID_NAMELABEL", wxStaticText), false, true);
		pwdDlg.GetSizer()->Show(XRCCTRL(pwdDlg, "ID_NAME", wxStaticText), false, true);
	}
	else {
		XRCCTRL(pwdDlg, "ID_NAME", wxStaticText)->SetLabel(name);
	}
	if (challenge.empty()) {
		pwdDlg.GetSizer()->Show(XRCCTRL(pwdDlg, "ID_CHALLENGELABEL", wxStaticText), false, true);
		pwdDlg.GetSizer()->Show(XRCCTRL(pwdDlg, "ID_CHALLENGE", wxTextCtrl), false, true);
	}
	else {
		wxString displayChallenge = challenge;
		displayChallenge.Trim(true);
		displayChallenge.Trim(false);
#ifdef FZ_WINDOWS
		displayChallenge.Replace(_T("\n"), _T("\r\n"));
#endif
		XRCCTRL(pwdDlg, "ID_CHALLENGE", wxTextCtrl)->ChangeValue(displayChallenge);
		pwdDlg.GetSizer()->Show(XRCCTRL(pwdDlg, "ID_REMEMBER", wxCheckBox), canRemember, true);
		XRCCTRL(pwdDlg, "ID_CHALLENGE", wxTextCtrl)->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
	}
	XRCCTRL(pwdDlg, "ID_HOST", wxStaticText)->SetLabel(server.Format(ServerFormat::with_optional_port));

	if (server.server.GetUser().empty()) {
		XRCCTRL(pwdDlg, "ID_OLD_USER_LABEL", wxStaticText)->Hide();
		XRCCTRL(pwdDlg, "ID_OLD_USER", wxStaticText)->Hide();

		XRCCTRL(pwdDlg, "ID_HEADER_PASS", wxStaticText)->Hide();
		if (server.credentials .logonType_ == LogonType::interactive) {
			pwdDlg.SetTitle(_("Enter username"));
			XRCCTRL(pwdDlg, "ID_PASSWORD_LABEL", wxStaticText)->Hide();
			XRCCTRL(pwdDlg, "ID_PASSWORD", wxTextCtrl)->Hide();
			XRCCTRL(pwdDlg, "ID_REMEMBER", wxCheckBox)->Hide();
			XRCCTRL(pwdDlg, "ID_HEADER_BOTH", wxStaticText)->Hide();
		}
		else {
			pwdDlg.SetTitle(_("Enter username and password"));
			XRCCTRL(pwdDlg, "ID_HEADER_USER", wxStaticText)->Hide();
		}

		XRCCTRL(pwdDlg, "ID_NEW_USER", wxTextCtrl)->SetFocus();
	}
	else {
		XRCCTRL(pwdDlg, "ID_OLD_USER", wxStaticText)->SetLabel(server.server.GetUser());
		XRCCTRL(pwdDlg, "ID_NEW_USER_LABEL", wxStaticText)->Hide();
		XRCCTRL(pwdDlg, "ID_NEW_USER", wxTextCtrl)->Hide();
		XRCCTRL(pwdDlg, "ID_HEADER_USER", wxStaticText)->Hide();
		XRCCTRL(pwdDlg, "ID_HEADER_BOTH", wxStaticText)->Hide();
	}
	XRCCTRL(pwdDlg, "wxID_OK", wxButton)->SetId(wxID_OK);
	XRCCTRL(pwdDlg, "wxID_CANCEL", wxButton)->SetId(wxID_CANCEL);
	pwdDlg.GetSizer()->Fit(&pwdDlg);
	pwdDlg.GetSizer()->SetSizeHints(&pwdDlg);

	std::wstring user;
	while (user.empty()) {
		if (pwdDlg.ShowModal() != wxID_OK) {
			return false;
		}

		if (server.server.GetUser().empty()) {
			user = XRCCTRL(pwdDlg, "ID_NEW_USER", wxTextCtrl)->GetValue().ToStdWstring();
			if (user.empty()) {
				wxMessageBoxEx(_("No username given."), _("Invalid input"), wxICON_EXCLAMATION);
				continue;
			}
		}
		else {
			user = server.server.GetUser();
		}
	}
	
	server.SetUser(user);
	server.credentials.SetPass(XRCCTRL(pwdDlg, "ID_PASSWORD", wxTextCtrl)->GetValue().ToStdWstring());

	if (canRemember) {
		RememberPassword(server, challenge);
	}

	return true;
}

void CLoginManager::CachedPasswordFailed(CServer const& server, std::wstring const& challenge)
{
	auto it = FindItem(server, challenge);
	if (it != m_passwordCache.end()) {
		m_passwordCache.erase(it);
	}
}

void CLoginManager::RememberPassword(ServerWithCredentials & server, std::wstring const& challenge)
{
	if (server.credentials.logonType_ == LogonType::anonymous) {
		return;
	}

	auto it = FindItem(server.server, challenge);
	if (it != m_passwordCache.end()) {
		it->password = server.credentials.GetPass();
	}
	else {
		t_passwordcache entry;
		entry.host = server.server.GetHost();
		entry.port = server.server.GetPort();
		entry.user = server.server.GetUser();
		entry.password = server.credentials.GetPass();
		entry.challenge = challenge;
		m_passwordCache.push_back(entry);
	}
}
