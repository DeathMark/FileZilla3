#include <filezilla.h>
#include "string_coalescer.h"

#include <wx/wx.h>

#include <unordered_map>
#include <algorithm>

namespace std {
using namespace tr1;
}

unsigned int CalculateCapacity()
{
#ifdef __WXMSW__
	MEMORYSTATUSEX s;
	s.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&s);

	if (s.ullTotalPhys >= 0x400000000)
		return 1024*1024*1024;
	else if (s.ullTotalPhys >= 0x200000000)
		return 512*1024*1024;
	else
		return 256*1024*1024;
#else
	return 256*1024*1024;
#endif
}

unsigned int GetCapacity()
{
	static unsigned int cap = CalculateCapacity();
	return cap;
}

struct backref;

// Can't use wxHashMap as it doesn't like forward declarations. That 
// contraption is actually implemented as a series of ugly macros and
// not using beautiful templates.

// Using tr1 unordered_map instead with wxStringHash, works sufficiently well.
typedef std::unordered_map<wxString, backref, wxStringHash> tree;
typedef std::list<tree::iterator> lru;

struct backref
{
	lru::const_iterator b;
};


tree tree_;
lru lru_;
unsigned int size_;

// Assumed per-string overhead
inline unsigned int GetOverhead()
{
	return sizeof(void*)*2 // List prev/next pointers;
		+ sizeof(void*)*3 // Three tree pointers
		+ sizeof(tree::const_iterator)
		+ sizeof(lru::const_iterator)
		+ sizeof(wxString); // m_pchData
}


void Coalesce(wxString& s)
{
	std::pair<tree::iterator, bool> r = tree_.insert(std::make_pair(s, backref()));
	tree::iterator& it = r.first;
	if( !r.second )
	{
		s = it->first;
		lru_.splice( lru_.end(), lru_, it->second.b );
		it->second.b = --lru_.end();
	}
	else
	{
		size_ += s.size() + GetOverhead();

		while (size_ > GetCapacity() && !lru_.empty())
		{
			size_ -= GetOverhead() + lru_.front()->first.size();
			tree_.erase( lru_.front() );
			lru_.pop_front();
		}

		tree::iterator it = tree_.insert(std::make_pair(s, backref())).first;
		lru_.push_back(it);
		it->second.b = --lru_.end();
	}
}

void ClearStringCoalescer()
{
	lru_.clear();
	tree_.clear();
}

void BenchmarkStringCoalescer()
{
	std::deque<wxString> s;
	for (tree::const_iterator it = tree_.begin(); it != tree_.end(); ++it)
	{
		s.push_back(it->first);
	}
	
	wxDateTime start = wxDateTime::UNow();
	for (int i = 0; i < 100; ++i)
	{
		for( std::deque<wxString>::iterator it = s.begin(); it != s.end(); ++it)
			Coalesce( *it );
	}
	wxDateTime stop = wxDateTime::UNow();

	std::vector<int> stringLengths;
	int sum = 0;
	for (tree::const_iterator it = tree_.begin(); it != tree_.end(); ++it) {
		stringLengths.push_back(it->first.size());
		sum += it->first.size();
	}
	std::sort(stringLengths.begin(), stringLengths.end());
	int median = stringLengths[stringLengths.size() / 2];
	float avg = (float)sum / tree_.size();

	wxMessageBox(wxString::Format(_T("Looking up all strings a hundred times took %dms.\n\nEntries: %u\nAssumed memory consumption: %u\nAverage string length: %0.3f\nMedian string length: %d"),
		(stop - start).GetMilliseconds().GetLo(),
		(unsigned int)tree_.size(),
		size_,
		avg,
		median
		));
}