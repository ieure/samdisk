// Bit buffer to hold decoded flux data for scanning

#include "SAMdisk.h"
#include "BitBuffer.h"

BitBuffer::BitBuffer (DataRate datarate_, int revs)
	: datarate(datarate_)
{
	// Estimate size from double the data bitrate @300rpm, plus 10%.
	// This should be enough for most FM/MFM tracks.
	auto bitlen = bits_per_second(datarate) * revs / 5 * 2 * 110 / 100;
	m_data.resize((bitlen + 31) / 32);
}

BitBuffer::BitBuffer (DataRate datarate_, const uint8_t *pb, int bitlen)
	: datarate(datarate_)
{
	m_data.resize((bitlen + 31) / 32);
	std::memcpy(m_data.data(), pb, (bitlen + 7) / 8);
	m_bitsize = bitlen;
}

BitBuffer::BitBuffer (DataRate datarate_, FluxDecoder &decoder)
	: datarate(datarate_), m_data(decoder.flux_count())
{
	auto bitlen = bits_per_second(datarate) * decoder.flux_revs() / 5 * 2 * 110 / 100;
	m_data.resize((bitlen + 31) / 32);

	for (;;)
	{
		int bit = decoder.next_bit();
		if (bit < 0)
			break;

		if (decoder.sync_lost())
		{
			if (opt.debug) util::cout << "sync lost at offset " << tell() << " (" << track_offset(tell()) << ")\n";
			sync_lost();
		}

		add(bit ? 1 : 0);

		if (decoder.index())
			index();
	}
}

bool BitBuffer::wrapped () const
{
	return m_wrapped || m_bitsize == 0;
}

int BitBuffer::size () const
{
	return m_bitsize;
}

int BitBuffer::remaining () const
{
	return size() - tell();
}

int BitBuffer::tell () const
{
	return m_bitpos;
}

bool BitBuffer::seek (int offset)
{
	m_wrapped = false;
	m_bitpos = std::min(offset, m_bitsize);
	return m_bitpos == offset;
}

void BitBuffer::index ()
{
	m_indexes.push_back(m_bitpos);
}

void BitBuffer::sync_lost ()
{
	m_sync_losses.push_back(m_bitpos);
}

void BitBuffer::add (uint8_t bit)
{
	size_t offset = m_bitpos / 32;
	uint32_t bit_value = 1 << (m_bitpos & 31);

	// Double the size if we run out of space
	if (offset >= m_data.size())
	{
		assert(m_data.size() != 0);
		m_data.resize(m_data.size() * 2);
		if (opt.debug) util::cout << "BitBuffer size grown to " << m_data.size() << "\n";
	}

	if (bit)
		m_data[offset] |= bit_value;
	else
		m_data[offset] &= ~bit_value;

	m_bitsize = std::max(m_bitsize, ++m_bitpos);
}

uint8_t BitBuffer::read1 ()
{
	uint8_t bit = (m_data[m_bitpos / 32] >> (m_bitpos & 31)) & 1;

	if (++m_bitpos == m_bitsize)
	{
		m_bitpos = 0;
		m_wrapped = true;
	}

	return bit;
}

uint16_t BitBuffer::read16 ()
{
	uint16_t word = 0;

	for (auto i = 0; i < 16; ++i)
		word = (word << 1) | read1();

	return word;
}

uint32_t BitBuffer::read32()
{
	uint32_t dword = 0;

	for (auto i = 0; i < 32; ++i)
		dword = (dword << 1) | read1();

	return dword;
}

uint8_t BitBuffer::read_byte ()
{
	uint8_t data = 0;

	if (encoding == Encoding::FM)
	{
		for (auto i = 0; i < 8; ++i)
		{
			read1();
			read1();
			data = (data << 1) | read1();
			read1();
		}
	}
	else
	{
		for (auto i = 0; i < 8; ++i)
		{
			read1();
			data = (data << 1) | read1();
		}
	}

	return data;
}

int BitBuffer::track_bitsize () const
{
	return m_indexes.size() ? m_indexes[0] : m_bitsize;
}

int BitBuffer::track_offset (int bitpos) const
{
	for (auto it = m_indexes.rbegin(); it != m_indexes.rend(); ++it)
	{
		if (bitpos >= *it)
			return bitpos - *it;
	}

	return bitpos;
}

bool BitBuffer::sync_lost (int begin, int end) const
{
	for (auto pos : m_sync_losses)
		if (begin < pos && end >= pos)
			return true;

	return false;
}
