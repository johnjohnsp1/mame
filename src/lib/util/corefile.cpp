// license:BSD-3-Clause
// copyright-holders:Aaron Giles, Vas Crabb
/***************************************************************************

    corefile.c

    File access functions.

***************************************************************************/

#include <assert.h>

#include "corefile.h"
#include "unicode.h"
#include <zlib.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <ctype.h>



namespace util {

namespace {

/***************************************************************************
    VALIDATION
***************************************************************************/

#if !defined(CRLF) || (CRLF < 1) || (CRLF > 3)
#error CRLF undefined: must be 1 (CR), 2 (LF) or 3 (CR/LF)
#endif



/***************************************************************************
    TYPE DEFINITIONS
***************************************************************************/

class zlib_data
{
public:
	typedef std::unique_ptr<zlib_data> ptr;

	static int start_compression(int level, std::uint64_t offset, ptr &data)
	{
		ptr result(new zlib_data(offset));
		result->reset_output();
		auto const zerr = deflateInit(&result->m_stream, level);
		result->m_compress = (Z_OK == zerr);
		if (result->m_compress) data = std::move(result);
		return zerr;
	}
	static int start_decompression(std::uint64_t offset, ptr &data)
	{
		ptr result(new zlib_data(offset));
		auto const zerr = inflateInit(&result->m_stream);
		result->m_decompress = (Z_OK == zerr);
		if (result->m_decompress) data = std::move(result);
		return zerr;
	}

	~zlib_data()
	{
		if (m_compress) deflateEnd(&m_stream);
		else if (m_decompress) inflateEnd(&m_stream);
	}

	std::size_t buffer_size() const { return sizeof(m_buffer); }
	void const *buffer_data() const { return m_buffer; }
	void *buffer_data() { return m_buffer; }

	// general-purpose output buffer manipulation
	bool output_full() const { return 0 == m_stream.avail_out; }
	std::size_t output_space() const { return m_stream.avail_out; }
	void set_output(void *data, std::uint32_t size)
	{
		m_stream.next_out = reinterpret_cast<Bytef *>(data);
		m_stream.avail_out = size;
	}

	// working with output to the internal buffer
	bool has_output() const { return m_stream.avail_out != sizeof(m_buffer); }
	std::size_t output_size() const { return sizeof(m_buffer) - m_stream.avail_out; }
	void reset_output()
	{
		m_stream.next_out = m_buffer;
		m_stream.avail_out = sizeof(m_buffer);
	}

	// general-purpose input buffer manipulation
	bool has_input() const { return 0 != m_stream.avail_in; }
	std::size_t input_size() const { return m_stream.avail_in; }
	void set_input(void const *data, std::uint32_t size)
	{
		m_stream.next_in = const_cast<Bytef *>(reinterpret_cast<Bytef const *>(data));
		m_stream.avail_in = size;
	}

	// working with input from the internal buffer
	void reset_input(std::uint32_t size)
	{
		m_stream.next_in = m_buffer;
		m_stream.avail_in = size;
	}

	int compress() { assert(m_compress); return deflate(&m_stream, Z_NO_FLUSH); }
	int finalise() { assert(m_compress); return deflate(&m_stream, Z_FINISH); }
	int decompress() { assert(m_decompress); return inflate(&m_stream, Z_SYNC_FLUSH); }

	std::uint64_t realoffset() const { return m_realoffset; }
	void add_realoffset(std::uint32_t increment) { m_realoffset += increment; }

	bool is_nextoffset(std::uint64_t value) const { return m_nextoffset == value; }
	void add_nextoffset(std::uint32_t increment) { m_nextoffset += increment; }

private:
	zlib_data(std::uint64_t offset)
		: m_compress(false)
		, m_decompress(false)
		, m_realoffset(offset)
		, m_nextoffset(offset)
	{
		m_stream.zalloc = Z_NULL;
		m_stream.zfree = Z_NULL;
		m_stream.opaque = Z_NULL;
	}

	bool            m_compress, m_decompress;
	z_stream        m_stream;
	std::uint8_t    m_buffer[1024];
	std::uint64_t   m_realoffset;
	std::uint64_t   m_nextoffset;
};


class core_proxy_file : public core_file
{
public:
	core_proxy_file(core_file &file) : m_file(file) { }
	virtual ~core_proxy_file() override { }
	virtual file_error compress(int level) override { return m_file.compress(level); }

	virtual int seek(std::int64_t offset, int whence) override { return m_file.seek(offset, whence); }
	virtual std::uint64_t tell() const override { return m_file.tell(); }
	virtual bool eof() const override { return m_file.eof(); }
	virtual std::uint64_t size() const override { return m_file.size(); }

	virtual std::uint32_t read(void *buffer, std::uint32_t length) override { return m_file.read(buffer, length); }
	virtual int getc() override { return m_file.getc(); }
	virtual int ungetc(int c) override { return m_file.ungetc(c); }
	virtual char *gets(char *s, int n) override { return m_file.gets(s, n); }
	virtual const void *buffer() override { return m_file.buffer(); }

	virtual std::uint32_t write(const void *buffer, std::uint32_t length) override { return m_file.write(buffer, length); }
	virtual int puts(const char *s) override { return m_file.puts(s); }
	virtual int vprintf(const char *fmt, va_list va) override { return m_file.vprintf(fmt, va); }
	virtual file_error truncate(std::uint64_t offset) override { return m_file.truncate(offset); }

	virtual file_error flush() override { return m_file.flush(); }

private:
	core_file &m_file;
};


class core_text_file : public core_file
{
public:
	enum class text_file_type
	{
		OSD,        // OSD dependent encoding format used when BOMs missing
		UTF8,       // UTF-8
		UTF16BE,    // UTF-16 (big endian)
		UTF16LE,    // UTF-16 (little endian)
		UTF32BE,    // UTF-32 (UCS-4) (big endian)
		UTF32LE     // UTF-32 (UCS-4) (little endian)
	};

	virtual int getc() override;
	virtual int ungetc(int c) override;
	virtual char *gets(char *s, int n) override;
	virtual int puts(char const *s) override;
	virtual int vprintf(char const *fmt, va_list va) override;

protected:
	core_text_file(std::uint32_t openflags)
		: m_openflags(openflags)
		, m_text_type(text_file_type::OSD)
		, m_back_char_head(0)
		, m_back_char_tail(0)
	{
	}

	bool read_access() const { return 0U != (m_openflags & OPEN_FLAG_READ); }
	bool write_access() const { return 0U != (m_openflags & OPEN_FLAG_WRITE); }
	bool no_bom() const { return 0U != (m_openflags & OPEN_FLAG_NO_BOM); }

	bool has_putback() const { return m_back_char_head != m_back_char_tail; }
	void clear_putback() { m_back_char_head = m_back_char_tail = 0; }

private:
	std::uint32_t const m_openflags;                    // flags we were opened with
	text_file_type      m_text_type;                    // text output format
	char                m_back_chars[UTF8_CHAR_MAX];    // buffer to hold characters for ungetc
	int                 m_back_char_head;               // head of ungetc buffer
	int                 m_back_char_tail;               // tail of ungetc buffer
};


class core_in_memory_file : public core_text_file
{
public:
	core_in_memory_file(std::uint32_t openflags, void const *data, std::size_t length, bool copy)
		: core_text_file(openflags)
		, m_data_allocated(false)
		, m_data(copy ? nullptr : data)
		, m_offset(0)
		, m_length(length)
	{
		if (copy)
		{
			void *const buf = allocate();
			if (buf) std::memcpy(buf, data, length);
		}
	}

	~core_in_memory_file() override { purge(); }
	virtual file_error compress(int level) override { return FILERR_INVALID_ACCESS; }

	virtual int seek(std::int64_t offset, int whence) override;
	virtual std::uint64_t tell() const override { return m_offset; }
	virtual bool eof() const override;
	virtual std::uint64_t size() const override { return m_length; }

	virtual std::uint32_t read(void *buffer, std::uint32_t length) override;
	virtual void const *buffer() override { return m_data; }

	virtual std::uint32_t write(void const *buffer, std::uint32_t length) override { return 0; }
	virtual file_error truncate(std::uint64_t offset) override;
	virtual file_error flush() override { clear_putback(); return FILERR_NONE; }

protected:
	core_in_memory_file(std::uint32_t openflags, std::uint64_t length)
		: core_text_file(openflags)
		, m_data_allocated(false)
		, m_data(nullptr)
		, m_offset(0)
		, m_length(length)
	{
	}

	bool is_loaded() const { return nullptr != m_data; }
	void *allocate()
	{
		if (m_data) return nullptr;
		void *data = malloc(m_length);
		if (data)
		{
			m_data_allocated = true;
			m_data = data;
		}
		return data;
	}
	void purge()
	{
		if (m_data && m_data_allocated) free(const_cast<void *>(m_data));
		m_data_allocated = false;
		m_data = nullptr;
	}

	std::uint64_t offset() const { return m_offset; }
	void add_offset(std::uint32_t increment) { m_offset += increment; m_length = (std::max)(m_length, m_offset); }
	std::uint64_t length() const { return m_length; }
	void set_length(std::uint64_t value) { m_length = value; m_offset = (std::min)(m_offset, m_length); }

	static std::size_t safe_buffer_copy(
			void const *source, std::size_t sourceoffs, std::size_t sourcelen,
			void *dest, std::size_t destoffs, std::size_t destlen);

private:
	bool            m_data_allocated;   // was the data allocated by us?
	void const *    m_data;             // file data, if RAM-based
	std::uint64_t   m_offset;           // current file offset
	std::uint64_t   m_length;           // total file length
};


class core_osd_file : public core_in_memory_file
{
public:
	core_osd_file(std::uint32_t openmode, osd_file *file, std::uint64_t length)
		: core_in_memory_file(openmode, length)
		, m_file(file)
		, m_zdata()
		, m_bufferbase(0)
		, m_bufferbytes(0)
	{
	}
	~core_osd_file() override;

	virtual file_error compress(int level) override;

	virtual int seek(std::int64_t offset, int whence) override;

	virtual std::uint32_t read(void *buffer, std::uint32_t length) override;
	virtual void const *buffer() override;

	virtual std::uint32_t write(void const *buffer, std::uint32_t length) override;
	virtual file_error truncate(std::uint64_t offset) override;
	virtual file_error flush() override;

protected:

	bool is_buffered() const { return (offset() >= m_bufferbase) && (offset() < (m_bufferbase + m_bufferbytes)); }

private:
	static constexpr std::size_t FILE_BUFFER_SIZE = 512;

	file_error osd_or_zlib_read(void *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual);
	file_error osd_or_zlib_write(void const *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual);

	osd_file *      m_file;                     // OSD file handle
	zlib_data::ptr  m_zdata;                    // compression data
	std::uint64_t   m_bufferbase;               // base offset of internal buffer
	std::uint32_t   m_bufferbytes;              // bytes currently loaded into buffer
	std::uint8_t    m_buffer[FILE_BUFFER_SIZE]; // buffer data
};



/***************************************************************************
    INLINE FUNCTIONS
***************************************************************************/

/*-------------------------------------------------
    is_directory_separator - is a given character
    a directory separator? The following logic
    works for most platforms
-------------------------------------------------*/

static inline int is_directory_separator(char c)
{
	return (c == '\\' || c == '/' || c == ':');
}



/***************************************************************************
    core_text_file
***************************************************************************/

/*-------------------------------------------------
    getc - read a character from a file
-------------------------------------------------*/

int core_text_file::getc()
{
	int result;

	// refresh buffer, if necessary
	if (m_back_char_head == m_back_char_tail)
	{
		// do we need to check the byte order marks?
		if (tell() == 0)
		{
			std::uint8_t bom[4];
			int pos = 0;

			if (read(bom, 4) == 4)
			{
				if (bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf)
				{
					m_text_type = text_file_type::UTF8;
					pos = 3;
				}
				else if (bom[0] == 0x00 && bom[1] == 0x00 && bom[2] == 0xfe && bom[3] == 0xff)
				{
					m_text_type = text_file_type::UTF32BE;
					pos = 4;
				}
				else if (bom[0] == 0xff && bom[1] == 0xfe && bom[2] == 0x00 && bom[3] == 0x00)
				{
					m_text_type = text_file_type::UTF32LE;
					pos = 4;
				}
				else if (bom[0] == 0xfe && bom[1] == 0xff)
				{
					m_text_type = text_file_type::UTF16BE;
					pos = 2;
				}
				else if (bom[0] == 0xff && bom[1] == 0xfe)
				{
					m_text_type = text_file_type::UTF16LE;
					pos = 2;
				}
				else
				{
					m_text_type = text_file_type::OSD;
					pos = 0;
				}
			}
			seek(pos, SEEK_SET);
		}

		// fetch the next character
		utf16_char utf16_buffer[UTF16_CHAR_MAX];
		unicode_char uchar = unicode_char(~0);
		switch (m_text_type)
		{
		default:
		case text_file_type::OSD:
			{
				char default_buffer[16];
				auto const readlen = read(default_buffer, sizeof(default_buffer));
				if (readlen > 0)
				{
					auto const charlen = osd_uchar_from_osdchar(&uchar, default_buffer, readlen / sizeof(default_buffer[0]));
					seek(std::int64_t(charlen * sizeof(default_buffer[0])) - readlen, SEEK_CUR);
				}
			}
			break;

		case text_file_type::UTF8:
			{
				char utf8_buffer[UTF8_CHAR_MAX];
				auto const readlen = read(utf8_buffer, sizeof(utf8_buffer));
				if (readlen > 0)
				{
					auto const charlen = uchar_from_utf8(&uchar, utf8_buffer, readlen / sizeof(utf8_buffer[0]));
					seek(std::int64_t(charlen * sizeof(utf8_buffer[0])) - readlen, SEEK_CUR);
				}
			}
			break;

		case text_file_type::UTF16BE:
			{
				auto const readlen = read(utf16_buffer, sizeof(utf16_buffer));
				if (readlen > 0)
				{
					auto const charlen = uchar_from_utf16be(&uchar, utf16_buffer, readlen / sizeof(utf16_buffer[0]));
					seek(std::int64_t(charlen * sizeof(utf16_buffer[0])) - readlen, SEEK_CUR);
				}
			}
			break;

		case text_file_type::UTF16LE:
			{
				auto const readlen = read(utf16_buffer, sizeof(utf16_buffer));
				if (readlen > 0)
				{
					auto const charlen = uchar_from_utf16le(&uchar, utf16_buffer, readlen / sizeof(utf16_buffer[0]));
					seek(std::int64_t(charlen * sizeof(utf16_buffer[0])) - readlen, SEEK_CUR);
				}
			}
			break;

		case text_file_type::UTF32BE:
			if (read(&uchar, sizeof(uchar)) == sizeof(uchar))
				uchar = BIG_ENDIANIZE_INT32(uchar);
			break;

		case text_file_type::UTF32LE:
			if (read(&uchar, sizeof(uchar)) == sizeof(uchar))
				uchar = LITTLE_ENDIANIZE_INT32(uchar);
			break;
		}

		if (uchar != ~0)
		{
			// place the new character in the ring buffer
			m_back_char_head = 0;
			m_back_char_tail = utf8_from_uchar(m_back_chars, ARRAY_LENGTH(m_back_chars), uchar);
			//assert(file->back_char_tail != -1);
		}
	}

	// now read from the ring buffer
	if (m_back_char_head == m_back_char_tail)
		result = EOF;
	else
	{
		result = m_back_chars[m_back_char_head++];
		m_back_char_head %= ARRAY_LENGTH(m_back_chars);
	}

	return result;
}


/*-------------------------------------------------
    ungetc - put back a character read from a
    file
-------------------------------------------------*/

int core_text_file::ungetc(int c)
{
	m_back_chars[m_back_char_tail++] = char(c);
	m_back_char_tail %= ARRAY_LENGTH(m_back_chars);
	return c;
}


/*-------------------------------------------------
    gets - read a line from a text file
-------------------------------------------------*/

char *core_text_file::gets(char *s, int n)
{
	char *cur = s;

	// loop while we have characters
	while (n > 0)
	{
		int const c = getc();
		if (c == EOF)
			break;
		else if (c == 0x0d) // if there's a CR, look for an LF afterwards
		{
			int const c2 = getc();
			if (c2 != 0x0a)
				ungetc(c2);
			*cur++ = 0x0d;
			n--;
			break;
		}
		else if (c == 0x0a) // if there's an LF, reinterp as a CR for consistency
		{
			*cur++ = 0x0d;
			n--;
			break;
		}
		else // otherwise, pop the character in and continue
		{
			*cur++ = c;
			n--;
		}
	}

	// if we put nothing in, return NULL
	if (cur == s)
		return nullptr;

	/* otherwise, terminate */
	if (n > 0)
		*cur++ = 0;
	return s;
}


/*-------------------------------------------------
    puts - write a line to a text file
-------------------------------------------------*/

int core_text_file::puts(char const *s)
{
	char convbuf[1024];
	char *pconvbuf = convbuf;
	int count = 0;

	// is this the beginning of the file?  if so, write a byte order mark
	if (tell() == 0 && !no_bom())
	{
		*pconvbuf++ = char(0xef);
		*pconvbuf++ = char(0xbb);
		*pconvbuf++ = char(0xbf);
	}

	// convert '\n' to platform dependant line endings
	while (*s != '\0')
	{
		if (*s == '\n')
		{
			if (CRLF == 1)      // CR only
				*pconvbuf++ = 13;
			else if (CRLF == 2) // LF only
				*pconvbuf++ = 10;
			else if (CRLF == 3) // CR+LF
			{
				*pconvbuf++ = 13;
				*pconvbuf++ = 10;
			}
		}
		else
			*pconvbuf++ = *s;
		s++;

		// if we overflow, break into chunks
		if (pconvbuf >= convbuf + ARRAY_LENGTH(convbuf) - 10)
		{
			count += write(convbuf, pconvbuf - convbuf);
			pconvbuf = convbuf;
		}
	}

	// final flush
	if (pconvbuf != convbuf)
		count += write(convbuf, pconvbuf - convbuf);

	return count;
}


/*-------------------------------------------------
    vprintf - vfprintf to a text file
-------------------------------------------------*/

int core_text_file::vprintf(char const *fmt, va_list va)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, va);
	return puts(buf);
}



/***************************************************************************
    core_in_memory_file
***************************************************************************/

/*-------------------------------------------------
    seek - seek within a file
-------------------------------------------------*/

int core_in_memory_file::seek(std::int64_t offset, int whence)
{
	// flush any buffered char
	clear_putback();

	// switch off the relative location
	switch (whence)
	{
	case SEEK_SET:
		m_offset = offset;
		break;

	case SEEK_CUR:
		m_offset += offset;
		break;

	case SEEK_END:
		m_offset = m_length + offset;
		break;
	}
	return 0;
}


/*-------------------------------------------------
    eof - return 1 if we're at the end of file
-------------------------------------------------*/

bool core_in_memory_file::eof() const
{
	// check for buffered chars
	if (has_putback())
		return 0;

	// if the offset == length, we're at EOF
	return (m_offset >= m_length);
}


/*-------------------------------------------------
    read - read from a file
-------------------------------------------------*/

std::uint32_t core_in_memory_file::read(void *buffer, std::uint32_t length)
{
	clear_putback();

	// handle RAM-based files
	auto const bytes_read = safe_buffer_copy(m_data, std::size_t(m_offset), std::size_t(m_length), buffer, 0, length);
	m_offset += bytes_read;
	return bytes_read;
}


/*-------------------------------------------------
    truncate - truncate a file
-------------------------------------------------*/

file_error core_in_memory_file::truncate(std::uint64_t offset)
{
	if (m_length < offset)
		return FILERR_FAILURE;

	// adjust to new length and offset
	set_length(offset);
	return FILERR_NONE;
}


/*-------------------------------------------------
    safe_buffer_copy - copy safely from one
    bounded buffer to another
-------------------------------------------------*/

std::size_t core_in_memory_file::safe_buffer_copy(
		void const *source, std::size_t sourceoffs, std::size_t sourcelen,
		void *dest, std::size_t destoffs, std::size_t destlen)
{
	auto const sourceavail = sourcelen - sourceoffs;
	auto const destavail = destlen - destoffs;
	auto const bytes_to_copy = (std::min)(sourceavail, destavail);
	if (bytes_to_copy > 0)
	{
		std::memcpy(
				reinterpret_cast<std::uint8_t *>(dest) + destoffs,
				reinterpret_cast<std::uint8_t const *>(source) + sourceoffs,
				bytes_to_copy);
	}
	return bytes_to_copy;
}



/***************************************************************************
    core_osd_file
***************************************************************************/

/*-------------------------------------------------
    closes a file
-------------------------------------------------*/

core_osd_file::~core_osd_file()
{
	// close files and free memory
	if (m_zdata)
		compress(FCOMPRESS_NONE);
	if (m_file)
		osd_close(m_file);
}


/*-------------------------------------------------
    compress - enable/disable streaming file
    compression via zlib; level is 0 to disable
    compression, or up to 9 for max compression
-------------------------------------------------*/

file_error core_osd_file::compress(int level)
{
	file_error result = FILERR_NONE;

	// can only do this for read-only and write-only cases
	if (read_access() && write_access())
		return FILERR_INVALID_ACCESS;

	// if we have been compressing, flush and free the data
	if (m_zdata && (level == FCOMPRESS_NONE))
	{
		int zerr = Z_OK;

		// flush any remaining data if we are writing
		while (write_access() != 0 && (zerr != Z_STREAM_END))
		{
			// deflate some more
			zerr = m_zdata->finalise();
			if (zerr != Z_STREAM_END && zerr != Z_OK)
			{
				result = FILERR_INVALID_DATA;
				break;
			}

			// write the data
			if (m_zdata->has_output())
			{
				std::uint32_t actualdata;
				auto const filerr = osd_write(m_file, m_zdata->buffer_data(), m_zdata->realoffset(), m_zdata->output_size(), &actualdata);
				if (filerr != FILERR_NONE)
					break;
				m_zdata->add_realoffset(actualdata);
				m_zdata->reset_output();
			}
		}

		// free memory
		m_zdata.reset();
	}

	// if we are just starting to compress, allocate a new buffer
	if (!m_zdata && (level > FCOMPRESS_NONE))
	{
		int zerr;

		// initialize the stream and compressor
		if (write_access())
			zerr = zlib_data::start_compression(level, offset(), m_zdata);
		else
			zerr = zlib_data::start_decompression(offset(), m_zdata);

		// on error, return an error
		if (zerr != Z_OK)
			return FILERR_OUT_OF_MEMORY;

		// flush buffers
		m_bufferbytes = 0;
	}

	return result;
}


/*-------------------------------------------------
    seek - seek within a file
-------------------------------------------------*/

int core_osd_file::seek(std::int64_t offset, int whence)
{
	// error if compressing
	if (m_zdata)
		return 1;
	else
		return core_in_memory_file::seek(offset, whence);
}


/*-------------------------------------------------
    read - read from a file
-------------------------------------------------*/

std::uint32_t core_osd_file::read(void *buffer, std::uint32_t length)
{
	if (!m_file || is_loaded())
		return core_in_memory_file::read(buffer, length);

	// flush any buffered char
	clear_putback();

	std::uint32_t bytes_read = 0;

	// if we're within the buffer, consume that first
	if (is_buffered())
		bytes_read += safe_buffer_copy(m_buffer, offset() - m_bufferbase, m_bufferbytes, buffer, bytes_read, length);

	// if we've got a small amount left, read it into the buffer first
	if (bytes_read < length)
	{
		if ((length - bytes_read) < (sizeof(m_buffer) / 2))
		{
			// read as much as makes sense into the buffer
			m_bufferbase = offset() + bytes_read;
			m_bufferbytes = 0;
			osd_or_zlib_read(m_buffer, m_bufferbase, sizeof(m_buffer), m_bufferbytes);

			// do a bounded copy from the buffer to the destination
			bytes_read += safe_buffer_copy(m_buffer, 0, m_bufferbytes, buffer, bytes_read, length);
		}
		else
		{
			// read the remainder directly from the file
			std::uint32_t new_bytes_read = 0;
			osd_or_zlib_read(reinterpret_cast<std::uint8_t *>(buffer) + bytes_read, offset() + bytes_read, length - bytes_read, new_bytes_read);
			bytes_read += new_bytes_read;
		}
	}

	// return the number of bytes read
	add_offset(bytes_read);
	return bytes_read;
}


/*-------------------------------------------------
    buffer - return a pointer to the file buffer;
    if it doesn't yet exist, load the file into
    RAM first
-------------------------------------------------*/

void const *core_osd_file::buffer()
{
	// if we already have data, just return it
	if (!is_loaded() && length())
	{
		// allocate some memory
		void *buf = allocate();
		if (!buf) return nullptr;

		// read the file
		std::uint32_t read_length = 0;
		auto const filerr = osd_or_zlib_read(buf, 0, length(), read_length);
		if ((filerr != FILERR_NONE) || (read_length != length()))
			purge();
		else
		{
			// close the file because we don't need it anymore
			osd_close(m_file);
			m_file = nullptr;
		}
	}
	return core_in_memory_file::buffer();
}


/*-------------------------------------------------
    write - write to a file
-------------------------------------------------*/

std::uint32_t core_osd_file::write(void const *buffer, std::uint32_t length)
{
	// can't write to RAM-based stuff
	if (is_loaded())
		return core_in_memory_file::write(buffer, length);

	// flush any buffered char
	clear_putback();

	// invalidate any buffered data
	m_bufferbytes = 0;

	// do the write
	std::uint32_t bytes_written = 0;
	osd_or_zlib_write(buffer, offset(), length, bytes_written);

	// return the number of bytes written
	add_offset(bytes_written);
	return bytes_written;
}


/*-------------------------------------------------
    truncate - truncate a file
-------------------------------------------------*/

file_error core_osd_file::truncate(std::uint64_t offset)
{
	if (is_loaded())
		return core_in_memory_file::truncate(offset);

	// truncate file
	auto const err = osd_truncate(m_file, offset);
	if (err != FILERR_NONE)
		return err;

	// and adjust to new length and offset
	set_length(offset);
	return FILERR_NONE;
}


/*-------------------------------------------------
    flush - flush file buffers
-------------------------------------------------*/

file_error core_osd_file::flush()
{
	if (is_loaded())
		return core_in_memory_file::flush();

	// flush any buffered char
	clear_putback();

	// invalidate any buffered data
	m_bufferbytes = 0;

	return osd_fflush(m_file);
}


/*-------------------------------------------------
    osd_or_zlib_read - wrapper for osd_read that
    handles zlib-compressed data
-------------------------------------------------*/

file_error core_osd_file::osd_or_zlib_read(void *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual)
{
	// if no compression, just pass through
	if (!m_zdata)
		return osd_read(m_file, buffer, offset, length, &actual);

	// if the offset doesn't match the next offset, fail
	if (m_zdata->is_nextoffset(offset))
		return FILERR_INVALID_ACCESS;

	// set up the destination
	m_zdata->set_output(buffer, length);
	while (!m_zdata->output_full())
	{
		int zerr = Z_OK;

		// if we didn't make progress, report an error or the end
		if (m_zdata->has_input())
			zerr = m_zdata->decompress();
		if (zerr != Z_OK)
		{
			actual = length - m_zdata->output_space();
			m_zdata->add_nextoffset(actual);
			return (zerr == Z_STREAM_END) ? FILERR_NONE : FILERR_INVALID_DATA;
		}

		// fetch more data if needed
		if (!m_zdata->has_input())
		{
			std::uint32_t actualdata;
			auto const filerr = osd_read(m_file, m_zdata->buffer_data(), m_zdata->realoffset(), m_zdata->buffer_size(), &actualdata);
			if (filerr != FILERR_NONE)
				return filerr;
			m_zdata->add_realoffset(actual);
			m_zdata->reset_input(actualdata);
		}
	}

	// we read everything
	actual = length;
	m_zdata->add_nextoffset(actual);
	return FILERR_NONE;
}


/*-------------------------------------------------
    osd_or_zlib_write - wrapper for osd_write that
    handles zlib-compressed data
-------------------------------------------------*/

/**
 * @fn  file_error osd_or_zlib_write(void const *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t actual)
 *
 * @brief   OSD or zlib write.
 *
 * @param   buffer          The buffer.
 * @param   offset          The offset.
 * @param   length          The length.
 * @param [in,out]  actual  The actual.
 *
 * @return  A file_error.
 */

file_error core_osd_file::osd_or_zlib_write(void const *buffer, std::uint64_t offset, std::uint32_t length, std::uint32_t &actual)
{
	// if no compression, just pass through
	if (!m_zdata)
		return osd_write(m_file, buffer, offset, length, &actual);

	// if the offset doesn't match the next offset, fail
	if (!m_zdata->is_nextoffset(offset))
		return FILERR_INVALID_ACCESS;

	// set up the source
	m_zdata->set_input(buffer, length);
	while (m_zdata->has_input())
	{
		// if we didn't make progress, report an error or the end
		auto const zerr = m_zdata->compress();
		if (zerr != Z_OK)
		{
			actual = length - m_zdata->input_size();
			m_zdata->add_nextoffset(actual);
			return FILERR_INVALID_DATA;
		}

		// write more data if we are full up
		if (m_zdata->output_full())
		{
			std::uint32_t actualdata;
			auto const filerr = osd_write(m_file, m_zdata->buffer_data(), m_zdata->realoffset(), m_zdata->output_size(), &actualdata);
			if (filerr != FILERR_NONE)
				return filerr;
			m_zdata->add_realoffset(actualdata);
			m_zdata->reset_output();
		}
	}

	// we wrote everything
	actual = length;
	m_zdata->add_nextoffset(actual);
	return FILERR_NONE;
}

} // anonymous namespace



/***************************************************************************
    core_file
***************************************************************************/

/*-------------------------------------------------
    open - open a file for access and
    return an error code
-------------------------------------------------*/

file_error core_file::open(char const *filename, std::uint32_t openflags, ptr &file)
{
	// attempt to open the file
	osd_file *f = nullptr;
	std::uint64_t length = 0;
	auto const filerr = osd_open(filename, openflags, &f, &length);
	if (filerr != FILERR_NONE)
		return filerr;

	try
	{
		file = std::make_unique<core_osd_file>(openflags, f, length);
		return FILERR_NONE;
	}
	catch (...)
	{
		osd_close(f);
		return FILERR_OUT_OF_MEMORY;
	}
}


/*-------------------------------------------------
    open_ram - open a RAM-based buffer for file-
    like access and return an error code
-------------------------------------------------*/

file_error core_file::open_ram(void const *data, std::size_t length, std::uint32_t openflags, ptr &file)
{
	// can only do this for read access
	if ((openflags & OPEN_FLAG_WRITE) || (openflags & OPEN_FLAG_CREATE))
		return FILERR_INVALID_ACCESS;

	try
	{
		file.reset(new core_in_memory_file(openflags, data, length, false));
		return FILERR_NONE;
	}
	catch (...)
	{
		return FILERR_OUT_OF_MEMORY;
	}
}


/*-------------------------------------------------
    open_ram_copy - open a copy of a RAM-based
    buffer for file-like access and return an
    error code
-------------------------------------------------*/

file_error core_file::open_ram_copy(void const *data, std::size_t length, std::uint32_t openflags, ptr &file)
{
	// can only do this for read access
	if ((openflags & OPEN_FLAG_WRITE) || (openflags & OPEN_FLAG_CREATE))
		return FILERR_INVALID_ACCESS;

	try
	{
		ptr result(new core_in_memory_file(openflags, data, length, true));
		if (!result->buffer())
			return FILERR_OUT_OF_MEMORY;

		file = std::move(result);
		return FILERR_NONE;
	}
	catch (...)
	{
		return FILERR_OUT_OF_MEMORY;
	}
}


/*-------------------------------------------------
    open_proxy - open a proxy to an existing file
    object and return an error code
-------------------------------------------------*/

file_error core_file::open_proxy(core_file &file, ptr &proxy)
{
	try
	{
		proxy = std::make_unique<core_proxy_file>(file);
		return FILERR_NONE;
	}
	catch (...)
	{
		return FILERR_OUT_OF_MEMORY;
	}
}


/*-------------------------------------------------
    closes a file
-------------------------------------------------*/

core_file::~core_file()
{
}


/*-------------------------------------------------
    load - open a file with the specified
    filename, read it into memory, and return a
    pointer
-------------------------------------------------*/

file_error core_file::load(char const *filename, void **data, std::uint32_t &length)
{
	ptr file;

	// attempt to open the file
	auto const err = open(filename, OPEN_FLAG_READ, file);
	if (err != FILERR_NONE)
		return err;

	// get the size
	auto const size = file->size();
	if (std::uint32_t(size) != size)
		return FILERR_OUT_OF_MEMORY;

	// allocate memory
	*data = osd_malloc(size);
	length = std::uint32_t(size);

	// read the data
	if (file->read(*data, size) != size)
	{
		free(*data);
		return FILERR_FAILURE;
	}

	// close the file and return data
	return FILERR_NONE;
}

file_error core_file::load(char const *filename, dynamic_buffer &data)
{
	ptr file;

	// attempt to open the file
	auto const err = open(filename, OPEN_FLAG_READ, file);
	if (err != FILERR_NONE)
		return err;

	// get the size
	auto const size = file->size();
	if (std::uint32_t(size) != size)
		return FILERR_OUT_OF_MEMORY;

	// allocate memory
	data.resize(size);

	// read the data
	if (file->read(&data[0], size) != size)
	{
		data.clear();
		return FILERR_FAILURE;
	}

	// close the file and return data
	return FILERR_NONE;
}


/*-------------------------------------------------
    printf - printf to a text file
-------------------------------------------------*/

int CLIB_DECL core_file::printf(char const *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	auto const rc = vprintf(fmt, va);
	va_end(va);
	return rc;
}


/*-------------------------------------------------
    protected constructor
-------------------------------------------------*/

core_file::core_file()
{
}

} // namespace util



/***************************************************************************
    FILENAME UTILITIES
***************************************************************************/

/*-------------------------------------------------
    core_filename_extract_base - extract the base
    name from a filename; note that this makes
    assumptions about path separators
-------------------------------------------------*/

std::string core_filename_extract_base(const char *name, bool strip_extension)
{
	/* find the start of the name */
	const char *start = name + strlen(name);
	while (start > name && !util::is_directory_separator(start[-1]))
		start--;

	/* copy the rest into an astring */
	std::string result(start);

	/* chop the extension if present */
	if (strip_extension)
		result = result.substr(0, result.find_last_of('.'));
	return result;
}


/*-------------------------------------------------
    core_filename_ends_with - does the given
    filename end with the specified extension?
-------------------------------------------------*/

int core_filename_ends_with(const char *filename, const char *extension)
{
	int namelen = strlen(filename);
	int extlen = strlen(extension);
	int matches = TRUE;

	/* work backwards checking for a match */
	while (extlen > 0)
		if (tolower((UINT8)filename[--namelen]) != tolower((UINT8)extension[--extlen]))
		{
			matches = FALSE;
			break;
		}

	return matches;
}
