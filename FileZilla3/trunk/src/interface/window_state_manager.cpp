#include "FileZilla.h"
#include "window_state_manager.h"
#include "Options.h"
#if wxUSE_DISPLAY
#include <wx/display.h>
#endif
#include <wx/tokenzr.h>

CWindowStateManager::CWindowStateManager(wxTopLevelWindow* pWindow)
{
	m_pWindow = pWindow;

	m_lastMaximized = false;

	m_pWindow->Connect(wxID_ANY, wxEVT_SIZE, wxSizeEventHandler(CWindowStateManager::OnSize), 0, this);
	m_pWindow->Connect(wxID_ANY, wxEVT_MOVE, wxMoveEventHandler(CWindowStateManager::OnMove), 0, this);
}

CWindowStateManager::~CWindowStateManager()
{
	m_pWindow->Disconnect(wxID_ANY, wxEVT_SIZE, wxSizeEventHandler(CWindowStateManager::OnSize), 0, this);
	m_pWindow->Disconnect(wxID_ANY, wxEVT_MOVE, wxMoveEventHandler(CWindowStateManager::OnMove), 0, this);
}

void CWindowStateManager::Remember(unsigned int optionId)
{
	if (!m_lastWindowSize.IsFullySpecified())
		return;

	wxString posString;

	// is_maximized
	posString += wxString::Format(_T("%d "), m_lastMaximized ? 1 : 0);

	// pos_x pos_y
	posString += wxString::Format(_T("%d %d "), m_lastWindowPosition.x, m_lastWindowPosition.y);

	// pos_width pos_height
	posString += wxString::Format(_T("%d %d "), m_lastWindowSize.x, m_lastWindowSize.y);

	COptions::Get()->SetOption(optionId, posString);
}

bool CWindowStateManager::Restore(unsigned int optionId)
{
	if (wxGetKeyState(WXK_SHIFT) && wxGetKeyState(WXK_ALT) && wxGetKeyState(WXK_CONTROL))
	{
		m_pWindow->CenterOnScreen(wxBOTH);
		return false;
	}

	wxRect screen_size = GetScreenDimensions();

	// Fields:
	// - maximized (1 or 0)
	// - x position
	// - y position
	// - width
	// - height
	const wxString layout = COptions::Get()->GetOption(optionId);
	wxStringTokenizer tokens(layout, _T(" "));
	int count = tokens.CountTokens();
	if (count != 5)
	{
		m_pWindow->CenterOnScreen(wxBOTH);
		return false;
	}
	
	long values[5];
	for (int i = 0; i < count; i++)
	{
		wxString token = tokens.GetNextToken();
		if (!token.ToLong(values + i))
		{
			m_pWindow->CenterOnScreen(wxBOTH);
			return false;
		}
	}

	// Make sure position is (somewhat) sane
	int pos_x = wxMin(screen_size.GetRight() - 30, values[1]);
	int pos_y = wxMin(screen_size.GetBottom() - 30, values[2]);
	int client_width = values[3];
	int client_height = values[4];

	if (pos_x + client_width - 30 < screen_size.GetLeft())
		pos_x = screen_size.GetLeft();
	if (pos_y + client_height - 30 < screen_size.GetTop())
		pos_y = screen_size.GetTop();

	if (values[0])
	{
		// We need to move so it appears on the proper display on multi-monitor systems
		m_pWindow->Move(pos_x, pos_y);

		// We need to call SetClientSize here too. Since window isn't yet shown here, Maximize
		// doesn't actually resize the window till it is shown

		// The slight off-size is needed to ensure the client sizes gets changed at least once.
		// Otherwise all the splitters would have default size still.
		m_pWindow->SetClientSize(client_width + 1, client_height);

		// A 2nd call is neccessary, for some reason the first call
		// doesn't fully set the height properly at least under wxMSW
		m_pWindow->SetClientSize(client_width, client_height);

#ifdef __WXMSW__
		m_pWindow->Show();
#endif
		m_pWindow->Maximize();
	}
	else
	{
		m_pWindow->Move(pos_x, pos_y);

		// The slight off-size is needed to ensure the client sizes gets changed at least once.
		// Otherwise all the splitters would have default size still.
		m_pWindow->SetClientSize(client_width + 1, client_height);

		// A 2nd call is neccessary, for some reason the first call
		// doesn't fully set the height properly at least under wxMSW
		m_pWindow->SetClientSize(client_width, client_height);
	}

	return true;
}

void CWindowStateManager::OnSize(wxSizeEvent& event)
{
	if (!m_pWindow->IsIconized())
	{
		m_lastMaximized = m_pWindow->IsMaximized();
		if (!m_lastMaximized)
		{
			m_lastWindowPosition = m_pWindow->GetPosition();
			m_lastWindowSize = m_pWindow->GetClientSize();
		}
	}
	event.Skip();
}

void CWindowStateManager::OnMove(wxMoveEvent& event)
{
	if (!m_pWindow->IsIconized() && !m_pWindow->IsMaximized())
	{
		m_lastWindowPosition = m_pWindow->GetPosition();
		m_lastWindowSize = m_pWindow->GetClientSize();
	}
	event.Skip();
}

wxRect CWindowStateManager::GetScreenDimensions()
{
#if wxUSE_DISPLAY
	wxRect screen_size(0, 0, 0, 0);

	// Get bounding rectangle of virtual screen
	for (unsigned int i = 0; i < wxDisplay::GetCount(); i++)
	{
		wxDisplay display(i);
		wxRect rect = display.GetGeometry();
		screen_size.Union(rect);
	}
	if (screen_size.GetWidth() <= 0 || screen_size.GetHeight() <= 0)
		screen_size = wxRect(0, 0, 1000000000, 1000000000);
#else
	wxRect screen_size(0, 0, 1000000000, 1000000000);
#endif

	return screen_size;
}
