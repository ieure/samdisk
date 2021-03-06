#include "SAMdisk.h"
#include "Track.h"

#include "IBMPC.h"


Track::Track (int num_sectors/*=0*/)
{
	m_sectors.reserve(num_sectors);
}

bool Track::empty() const
{
	return m_sectors.empty();
}

int Track::size() const
{
	return static_cast<int>(m_sectors.size());
}
/*
EncRate Track::encrate(EncRate preferred) const
{
std::map<EncRate, int> freq;

for (auto sector : m_sectors)
++freq[sector.encrate];

auto it = std::max_element(freq.begin(), freq.end(), [] (const std::pair<EncRate, int> &a, const std::pair<EncRate, int> &b) {
return a.second < b.second;
});

if (it == freq.end() || it->second == freq[preferred])
return preferred;

return it->first;
}
*/

const std::vector<Sector> &Track::sectors() const
{
	return m_sectors;
}

std::vector<Sector> &Track::sectors()
{
	return m_sectors;
}

const Sector &Track::operator [] (int index) const
{
	assert(index < static_cast<int>(m_sectors.size()));
	return m_sectors[index];
}

Sector &Track::operator [] (int index)
{
	assert(index < static_cast<int>(m_sectors.size()));
	return m_sectors[index];
}

int Track::index_of (const Sector &sector) const
{
	auto it = std::find_if(begin(), end(), [&] (const Sector &s) {
		return &s == &sector;
	});

	return (it == end()) ? -1 : static_cast<int>(std::distance(begin(), it));
}

int Track::data_extent (const Sector &sector) const
{
	auto it = find(sector);
	assert(it != end());

	auto drive_speed = (sector.datarate == DataRate::_300K) ? RPM_TIME_360 : RPM_TIME_300;
	auto track_len = tracklen ? tracklen : GetTrackCapacity(drive_speed, sector.datarate, sector.encoding);

	// Approximate distance to next ID header
	auto gap = (std::next(it) != end()) ? std::next(it)->offset : track_len - sector.offset;
	auto overhead = (std::next(it) != end()) ? GetSectorOverhead(sector.encoding) - GetSyncOverhead(sector.encoding) : 0;
	auto extent = (gap > overhead) ? gap - overhead : 0;

	return extent;
}

bool Track::is_mixed_encoding () const
{
	if (empty())
		return false;

	auto first_encoding = m_sectors[0].encoding;

	auto it = std::find_if(begin() + 1, end(), [&] (const Sector &s) {
		return s.encoding != first_encoding;
	});

	return it != end();
}

bool Track::is_8k_sector () const
{
	return size() == 1 && m_sectors[0].is_8k_sector();
}

bool Track::is_repeated (const Sector &sector) const
{
	for (const auto &s : sectors())
	{
		// Ignore ourselves in the list
		if (&s == &sector)
			continue;

		// Stop if we find match for data rate, encoding, and CHRN
		if (s.datarate == sector.datarate &&
			s.encoding == sector.encoding &&
			s.header == sector.header)
			return true;
	}

	return false;
}

bool Track::has_data_error () const
{
	auto it = std::find_if(begin(), end(), [] (const Sector &sector) {
		return !sector.has_data() || sector.has_baddatacrc();
	});

	return it != end();
}

void Track::clear ()
{
	m_sectors.clear();
	tracklen = 0;
}

void Track::add (Track &&track)
{
	// Ignore if no sectors to add
	if (!track.sectors().size())
		return;

	// Use longest track length and time
	tracklen = std::max(tracklen, track.tracklen);
	tracktime = std::max(tracktime, track.tracktime);

	// Merge modified status
	modified |= track.modified;

	// Merge supplied sectors into existing track
	for (auto &s : track.sectors())
	{
		assert(s.offset != 0);
		add(std::move(s));
	}
}

Track::AddResult Track::add (Sector &&sector)
{
	// If there's no positional information, simply append
	if (sector.offset == 0)
	{
		m_sectors.emplace_back(std::move(sector));
		return AddResult::Append;
	}
	else
	{
		// Find a sector close enough to the new offset to be the same one
		auto it = std::find_if(begin(), end(), [&] (const Sector &s) {
			auto offset_min = std::min(sector.offset, s.offset);
			auto offset_max = std::max(sector.offset, s.offset);
			auto distance = std::min(offset_max - offset_min, tracklen + offset_min - offset_max);

			// Sector must be close enough and have the same header
			if (distance <= COMPARE_TOLERANCE_BITS && sector.header == s.header)
				return true;

			return false;
		});

		// If that failed, we have a new sector with an offset
		if (it == end())
		{
			// Find the insertion point to keep the sectors in order
			it = std::find_if(begin(), end(), [&] (const Sector &s) {
				return sector.offset < s.offset;
			});
			m_sectors.emplace(it, std::move(sector));
			return AddResult::Insert;
		}
		else
		{
			// Merge details with the existing sector
			it->merge(std::move(sector));
			return AddResult::Merge;
		}
	}
}

Track &Track::format (const CylHead &cylhead, const Format &fmt)
{
	assert(fmt.sectors != 0);

	m_sectors.clear();
	m_sectors.reserve(fmt.sectors);

	for (auto id : fmt.get_ids(cylhead))
	{
		Header header(cylhead.cyl, cylhead.head ? fmt.head1 : fmt.head0, id, fmt.size);
		Sector sector(fmt.datarate, fmt.encoding, header, fmt.gap3);
		Data data(fmt.sector_size(), fmt.fill);

		sector.add(std::move(data));
		add(std::move(sector));
	}

	return *this;
}

Data::const_iterator Track::populate (Data::const_iterator it, Data::const_iterator itEnd)
{
	assert(std::distance(it, itEnd) >= 0);

	// Populate in sector number order, which requires sorting the track
	std::vector<Sector *> ptrs(m_sectors.size());
	std::transform(m_sectors.begin(), m_sectors.end(), ptrs.begin(), [] (Sector &s) { return &s; });
	std::sort(ptrs.begin(), ptrs.end(), [] (Sector *a, Sector *b) { return a->header.sector < b->header.sector; });

	for (auto sector : ptrs)
	{
		assert(sector->copies() == 1);
		auto bytes = std::min(sector->size(), static_cast<int>(std::distance(it, itEnd)));
		std::copy_n(it, bytes, sector->data_copy(0).begin());
		it += bytes;
	}

	return it;
}

Sector Track::remove (int index)
{
	assert(index < static_cast<int>(m_sectors.size()));

	auto it = m_sectors.begin() + index;
	auto sector = std::move(*it);
	m_sectors.erase(it);
	return sector;
}

std::vector<Sector>::iterator Track::find (const Sector &sector)
{
	return std::find_if(begin(), end(), [&] (const Sector &s) {
		return &s == &sector;
	});
}

std::vector<Sector>::iterator Track::find (const Header &header)
{
	return std::find_if(begin(), end(), [&] (const Sector &s) {
		return header.compare(s.header);
	});
}

std::vector<Sector>::const_iterator Track::find (const Sector &sector) const
{
	return std::find_if(begin(), end(), [&] (const Sector &s) {
		return &s == &sector;
	});
}

std::vector<Sector>::const_iterator Track::find (const Header &header) const
{
	return std::find_if(begin(), end(), [&] (const Sector &s) {
		return header.compare(s.header);
	});
}

const Sector &Track::get_sector (const Header &header) const
{
	auto it = find(header);
	if (it == end() || it->data_size() < header.sector_size())
		throw util::exception(CylHead(header.cyl, header.head), " sector ", header.sector, " not found");

	return *it;
}
