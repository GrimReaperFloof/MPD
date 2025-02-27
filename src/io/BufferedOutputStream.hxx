// SPDX-License-Identifier: BSD-2-Clause
// author: Max Kellermann <max.kellermann@gmail.com>

#ifndef BUFFERED_OUTPUT_STREAM_HXX
#define BUFFERED_OUTPUT_STREAM_HXX

#include "util/DynamicFifoBuffer.hxx"

#include <fmt/core.h>
#if FMT_VERSION >= 80000 && FMT_VERSION < 90000
#include <fmt/format.h>
#endif

#include <cstddef>

#ifdef _UNICODE
#include <wchar.h>
#endif

class OutputStream;

/**
 * An #OutputStream wrapper that buffers its output to reduce the
 * number of OutputStream::Write() calls.
 *
 * All wchar_t based strings are converted to UTF-8.
 *
 * To make sure everything is written to the underlying #OutputStream,
 * call Flush() before destructing this object.
 */
class BufferedOutputStream {
	OutputStream &os;

	DynamicFifoBuffer<std::byte> buffer;

public:
	explicit BufferedOutputStream(OutputStream &_os,
				      size_t buffer_size=32768) noexcept
		:os(_os), buffer(buffer_size) {}

	/**
	 * Write the contents of a buffer.
	 */
	void Write(const void *data, std::size_t size);

	/**
	 * Write the given object.  Note that this is only safe with
	 * POD types.  Types with padding can expose sensitive data.
	 */
	template<typename T>
	void WriteT(const T &value) {
		Write(&value, sizeof(value));
	}

	/**
	 * Write one narrow character.
	 */
	void Write(const char &ch) {
		WriteT(ch);
	}

	/**
	 * Write a null-terminated string.
	 */
	void Write(const char *p);

	void VFmt(fmt::string_view format_str, fmt::format_args args);

	template<typename S, typename... Args>
	void Fmt(const S &format_str, Args&&... args) {
#if FMT_VERSION >= 90000
		VFmt(format_str,
		     fmt::make_format_args(args...));
#else
		VFmt(fmt::to_string_view(format_str),
		     fmt::make_args_checked<Args...>(format_str,
						     args...));
#endif
	}

#ifdef _UNICODE
	/**
	 * Write one narrow character.
	 */
	void Write(const wchar_t &ch) {
		WriteWideToUTF8(&ch, 1);
	}

	/**
	 * Write a null-terminated wide string.
	 */
	void Write(const wchar_t *p);
#endif

	/**
	 * Write buffer contents to the #OutputStream.
	 */
	void Flush();

	/**
	 * Discard buffer contents.
	 */
	void Discard() noexcept {
		buffer.Clear();
	}

private:
	bool AppendToBuffer(const void *data, std::size_t size) noexcept;

#ifdef _UNICODE
	void WriteWideToUTF8(const wchar_t *p, std::size_t length);
#endif
};

/**
 * Helper function which constructs a #BufferedOutputStream, calls the
 * given function and flushes the #BufferedOutputStream.
 */
template<typename F>
void
WithBufferedOutputStream(OutputStream &os, F &&f)
{
	BufferedOutputStream bos(os);
	f(bos);
	bos.Flush();
}

#endif
