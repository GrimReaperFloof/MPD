/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "StringUtil.hxx"
#include "CharUtil.hxx"
#include "ASCII.hxx"

#include <algorithm>

#include <assert.h>
#include <string.h>

char *
CopyString(char *gcc_restrict dest, const char *gcc_restrict src,
	   size_t size) noexcept
{
	size_t length = strlen(src);
	if (length >= size)
		length = size - 1;

	char *p = std::copy_n(src, length, dest);
	*p = '\0';
	return p;
}

const char *
StripLeft(const char *p) noexcept
{
	while (IsWhitespaceNotNull(*p))
		++p;

	return p;
}

const char *
StripLeft(const char *p, const char *end) noexcept
{
	while (p < end && IsWhitespaceOrNull(*p))
		++p;

	return p;
}

const char *
StripRight(const char *p, const char *end) noexcept
{
	while (end > p && IsWhitespaceOrNull(end[-1]))
		--end;

	return end;
}

size_t
StripRight(const char *p, size_t length) noexcept
{
	while (length > 0 && IsWhitespaceOrNull(p[length - 1]))
		--length;

	return length;
}

void
StripRight(char *p) noexcept
{
	size_t old_length = strlen(p);
	size_t new_length = StripRight(p, old_length);
	p[new_length] = 0;
}

char *
Strip(char *p) noexcept
{
	p = StripLeft(p);
	StripRight(p);
	return p;
}

bool
StringArrayContainsCase(const char *const*haystack,
			const char *needle) noexcept
{
	assert(haystack != nullptr);
	assert(needle != nullptr);

	for (; *haystack != nullptr; ++haystack)
		if (StringEqualsCaseASCII(*haystack, needle))
			return true;

	return false;
}

void
ToUpperASCII(char *dest, const char *src, size_t size) noexcept
{
	assert(dest != nullptr);
	assert(src != nullptr);
	assert(size > 1);

	char *const end = dest + size - 1;

	do {
		char ch = *src++;
		if (ch == 0)
			break;

		*dest++ = ToUpperASCII(ch);
	} while (dest < end);

	*dest = 0;
}
