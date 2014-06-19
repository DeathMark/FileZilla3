#ifndef __THEMEPROVIDER_H__
#define __THEMEPROVIDER_H__

#include <option_change_event_handler.h>
#include <memory>

enum iconSize
{
	iconSizeSmall,
	iconSizeNormal,
	iconSizeLarge
};

class CThemeProvider : public wxArtProvider, protected COptionChangeEventHandler
{
public:
	CThemeProvider();
	virtual ~CThemeProvider();

	static std::vector<wxString> GetThemes();
	static std::vector<std::unique_ptr<wxBitmap>> GetAllImages(const wxString& theme, const wxSize& size);
	static bool GetThemeData(const wxString& themePath, wxString& name, wxString& author, wxString& email);
	static std::vector<wxString> GetThemeSizes(const wxString& themePath);
	static wxIconBundle GetIconBundle(const wxArtID& id, const wxArtClient& client = wxART_OTHER);
	static bool ThemeHasSize(const wxString& themePath, const wxString& size);

	static wxSize GetIconSize(enum iconSize size);

	static CThemeProvider* Get();

	wxAnimation CreateAnimation(const wxArtID& id, const wxSize& size);
protected:
	static wxString GetThemePath();

	wxBitmap CreateBitmap(const wxArtID& id, const wxArtClient& /*client*/, const wxSize& size);

	std::list<wxString> GetSearchDirs(const wxSize& size);

	virtual void OnOptionChanged(int option);

	wxString m_themePath;
};

#endif //__THEMEPROVIDER_H__
