#include <filezilla.h>
#include "sizeformatting.h"
#include "Options.h"
#ifndef __WXMSW__
#include <langinfo.h>
#endif

namespace
{
	const wxChar prefix[] = { ' ', 'K', 'M', 'G', 'T', 'P', 'E' };
}

wxString CSizeFormat::Format(const wxLongLong& size, bool add_bytes_suffix, enum CSizeFormat::_format format, bool thousands_separator, int num_decimal_places)
{
	wxASSERT(format != formats_count);

	if (format == bytes)
	{
		wxString result = FormatNumber(size, &thousands_separator);

		if (!add_bytes_suffix)
			return result;
		else
		{
			// wxPLURAL has no support for wxLongLong
			int last;
			if (size > 1000000000)
				last = (1000000000 + (size % 1000000000)).GetLo();
			else
				last = size.GetLo();
			return wxString::Format(wxPLURAL("%s byte", "%s bytes", last), result.c_str());
		}
	}

	wxString places;

	int divider;
	if (format == si1000)
		divider = 1000;
	else
		divider = 1024;

	// Exponent (2^(10p) or 10^(3p) depending on option
	int p = 0;

	wxLongLong r = size;
	int remainder = 0;
	bool clipped = false;
	while (r > divider && p < 6)
	{
		const wxLongLong rr = r / divider;
		if (remainder != 0)
			clipped = true;
		remainder = (r - rr * divider).GetLo();
		r = rr;
		p++;
	}
	if (!num_decimal_places)
	{
		if (remainder != 0 || clipped)
			r++;
	}
	else if (p) // Don't add decimal places on exact bytes
	{
		if (format != si1000)
		{
			// Binary, need to convert 1024 into range from 1-1000
			if (clipped)
			{
				remainder++;
				clipped = false;
			}
			remainder = (int)ceil((double)remainder * 1000 / 1024);
		}

		int max;
		switch (num_decimal_places)
		{
		default:
		case 1:
			max = 9;
			divider = 100;
			break;
		case 2:
			max = 99;
			divider = 10;
			break;
		case 3:
			max = 999;
			break;
		}

		if (num_decimal_places != 3)
		{
			if (remainder % divider)
				clipped = true;
			remainder /= divider;
		}

		if (clipped)
			remainder++;
		if (remainder > max)
		{
			r++;
			remainder = 0;
		}

		places.Printf(_T("%d"), remainder);
		const int len = places.Len();
		for (int i = len; i < num_decimal_places; i++)
			places = _T("0") + places;
	}

	wxString result = r.ToString();
	if (places != _T(""))
	{
		const wxString& sep = GetRadixSeparator();

		result += sep;
		result += places;
	}
	result += ' ';

	static wxChar byte_unit = 0;
	if (!byte_unit)
	{
		wxString t = _("B <Unit symbol for bytes. Only translate first letter>");
		byte_unit = t[0];
	}

	if (!p)
		return result + byte_unit;

	result += prefix[p];
	if (format == iec)
		result += 'i';

	result += byte_unit;

	return result;
}

wxString CSizeFormat::Format(const wxLongLong& size, bool add_bytes_suffix /*=false*/)
{
	COptions* const pOptions = COptions::Get();
	const _format format = _format(pOptions->GetOptionVal(OPTION_SIZE_FORMAT));
	const bool thousands_separator = pOptions->GetOptionVal(OPTION_SIZE_USETHOUSANDSEP) != 0;
	const int num_decimal_places = pOptions->GetOptionVal(OPTION_SIZE_DECIMALPLACES);

	return Format(size, add_bytes_suffix, format, thousands_separator, num_decimal_places);
}

wxString CSizeFormat::FormatUnit(const wxLongLong& size, CSizeFormat::_unit unit, int base /*=1024*/)
{
	_format format = _format(COptions::Get()->GetOptionVal(OPTION_SIZE_FORMAT));
	if (base == 1000)
		format = si1000;
	else if (format != si1024)
		format = iec;
	return wxString::Format(_T("%s %s"), FormatNumber(size).c_str(), GetUnit(unit, format).c_str());
}

wxString CSizeFormat::GetUnitWithBase(_unit unit, int base)
{
	_format format = _format(COptions::Get()->GetOptionVal(OPTION_SIZE_FORMAT));
	if (base == 1000)
		format = si1000;
	else if (format != si1024)
		format = iec;
	return GetUnit(unit, format);
}

wxString CSizeFormat::GetUnit(_unit unit, CSizeFormat::_format format /*=formats_count*/)
{
	wxString ret;
	if (unit != byte)
		ret = prefix[unit];

	if (format == formats_count)
		format = _format(COptions::Get()->GetOptionVal(OPTION_SIZE_FORMAT));
	if (format == iec || format == bytes)
		ret += 'i';

	static wxChar byte_unit = 0;
	if (!byte_unit)
	{
		wxString t = _("B <Unit symbol for bytes. Only translate first letter>");
		byte_unit = t[0];
	}

	ret += byte_unit;

	return ret;
}

const wxString& CSizeFormat::GetThousandsSeparator()
{
	static wxString sep;
	static bool separator_initialized = false;
	if (!separator_initialized)
	{
		separator_initialized = true;
#ifdef __WXMSW__
		wxChar tmp[5];
		int count = ::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, tmp, 5);
		if (count)
			sep = tmp;
#else
		char* chr = nl_langinfo(THOUSEP);
		if (chr && *chr)
		{
#if wxUSE_UNICODE
			sep = wxString(chr, wxConvLibc);
#else
			sep = chr;
#endif
		}
#endif
	}

	return sep;
}

const wxString& CSizeFormat::GetRadixSeparator()
{
	static wxString sep;
	static bool separator_initialized = false;
	if (!separator_initialized)
	{
		separator_initialized = true;

#ifdef __WXMSW__
		wxChar tmp[5];
		int count = ::GetLocaleInfo(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, tmp, 5);
		if (!count)
			sep = _T(".");
		else
		{
			tmp[4] = 0;
			sep = tmp;
		}
#else
		char* chr = nl_langinfo(RADIXCHAR);
		if (!chr || !*chr)
			sep = _T(".");
		else
		{
#if wxUSE_UNICODE
			sep = wxString(chr, wxConvLibc);
#else
			sep = chr;
#endif
		}
#endif
	}

	return sep;
}

wxString CSizeFormat::FormatNumber(const wxLongLong& size, bool* thousands_separator /*=0*/)
{
	if ((thousands_separator && !*thousands_separator) || COptions::Get()->GetOptionVal(OPTION_SIZE_USETHOUSANDSEP) == 0)
		return size.ToString();

	const wxString& sep = CSizeFormat::GetThousandsSeparator();
	if (sep.empty())
		return size.ToString();

	wxString tmp = size.ToString();
	const int len = tmp.Len();
	if (len <= 3)
		return tmp;

	wxString result;
	int i = (len - 1) % 3 + 1;
	result = tmp.Left(i);
	while (i < len)
	{
		result += sep + tmp.Mid(i, 3);
		i += 3;
	}

	return result;
}
