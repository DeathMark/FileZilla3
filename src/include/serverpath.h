#ifndef __SERVERPATH_H__
#define __SERVERPATH_H__

#include <libfilezilla/optional.hpp>
#include <libfilezilla/shared.hpp>

#include <deque>

class CServerPathData final
{
public:
	std::deque<std::wstring> m_segments;
	fz::sparse_optional<std::wstring> m_prefix;

	bool operator==(const CServerPathData& cmp) const;
};

class CServerPath final
{
public:
	CServerPath();
	explicit CServerPath(wxString const& path, ServerType type = DEFAULT);
	CServerPath(CServerPath const& path, std::wstring subdir); // Ignores parent on absolute subdir
	CServerPath(CServerPath const& path) = default;
	CServerPath(CServerPath && path) noexcept = default;

	CServerPath& operator=(CServerPath const& op) = default;
	CServerPath& operator=(CServerPath && op) noexcept = default;

	bool empty() const { return !m_data; }
	void clear();

	bool SetPath(std::wstring newPath);
	bool SetPath(std::wstring& newPath, bool isFile);
	bool SetSafePath(const wxString& path);

	// If ChangePath returns false, the object will be left
	// empty.
	bool ChangePath(std::wstring const& subdir);
	bool ChangePath(std::wstring &subdir, bool isFile);

	wxString GetPath() const;
	wxString GetSafePath() const;

	bool HasParent() const;
	CServerPath GetParent() const;
	wxString GetLastSegment() const;

	CServerPath GetCommonParent(const CServerPath& path) const;

	bool SetType(ServerType type);
	ServerType GetType() const;

	bool IsSubdirOf(const CServerPath &path, bool cmpNoCase) const;
	bool IsParentOf(const CServerPath &path, bool cmpNoCase) const;

	bool operator==(const CServerPath &op) const;
	bool operator!=(const CServerPath &op) const;
	bool operator<(const CServerPath &op) const;

	int CmpNoCase(const CServerPath &op) const;

	// omitPath is just a hint. For example dataset member names on MVS servers
	// always use absolute filenames including the full path
	std::wstring FormatFilename(std::wstring const& filename, bool omitPath = false) const;

	// Returns identity on all but VMS. On VMS it escapes dots
	wxString FormatSubdir(const wxString &subdir) const;

	bool AddSegment(const wxString& segment);

	size_t SegmentCount() const;
private:
	bool IsSeparator(wchar_t c) const;

	bool DoSetSafePath(const wxString& path);
	bool DoChangePath(std::wstring &subdir, bool isFile);

	ServerType m_type;

	typedef std::deque<std::wstring> tSegmentList;
	typedef tSegmentList::iterator tSegmentIter;
	typedef tSegmentList::const_iterator tConstSegmentIter;

	bool Segmentize(std::wstring const& str, tSegmentList& segments);
	bool SegmentizeAddSegment(std::wstring & segment, tSegmentList& segments, bool& append);
	bool ExtractFile(wxString& dir, wxString& file);

	static void EscapeSeparators(ServerType type, wxString& subdir);

	fz::shared_optional<CServerPathData> m_data;
};

#endif
