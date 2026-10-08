#include "generated_encoded_function.hpp"

#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace duckdb {
namespace duckdb_encodings {

//! First non-ASCII byte value; bytes below this are ASCII
static constexpr uint32_t ASCII_LIMIT = 0x80;
//! Mask selecting the high bit of every byte in a 64-bit word
static constexpr uint64_t HIGH_BITS = 0x8080808080808080ULL;

//! Length of the run of ASCII bytes starting at ptr[begin], bounded by end
static idx_t AsciiRunLength(const char *ptr, idx_t begin, idx_t end) {
	idx_t run = 0;
	while (begin + run + sizeof(uint64_t) <= end) {
		uint64_t word;
		std::memcpy(&word, ptr + begin + run, sizeof(uint64_t));
		if (word & HIGH_BITS) {
			break;
		}
		run += sizeof(uint64_t);
	}
	while (begin + run < end && static_cast<uint8_t>(ptr[begin + run]) < ASCII_LIMIT) {
		run++;
	}
	return run;
}

//! Index over a (sorted) conversion map: for every possible first byte, the single-byte entry that starts with it
//! (if any) and the contiguous range of multi-byte entries that start with it. This turns the per-byte binary search
//! over the whole table into a single array lookup for single-byte sequences (e.g. all of ASCII), and a search over
//! only the entries sharing the lead byte otherwise.
struct EncodingIndex {
	const map_entry_encoding *single[256] = {};
	uint32_t multi_begin[256] = {};
	uint32_t multi_end[256] = {};
	//! True when every byte 0x00-0x7F decodes to itself and starts no multi-byte sequence, so runs of ASCII can be
	//! copied in bulk (e.g. the latin/windows families, but not EBCDIC or ISO-2022)
	bool ascii_identity = true;

	EncodingIndex(const map_entry_encoding *map, size_t map_size) {
		for (uint32_t i = 0; i < map_size; i++) {
			const auto &entry = map[i];
			const auto first = static_cast<uint8_t>(entry.key[0]);
			if (entry.key_len == 1) {
				single[first] = &entry;
				continue;
			}
			// The map is sorted by key, so all multi-byte entries with the same first byte are contiguous
			if (multi_end[first] == multi_begin[first]) {
				multi_begin[first] = i;
			}
			multi_end[first] = i + 1;
		}
		for (uint32_t byte_value = 0; byte_value < ASCII_LIMIT; byte_value++) {
			const auto entry = single[byte_value];
			const bool maps_to_self = !entry || (entry->value_len == 1 && static_cast<uint8_t>(entry->value[0]) ==
			                                                                  static_cast<uint8_t>(byte_value));
			if (HasMultiByte(static_cast<uint8_t>(byte_value)) || !maps_to_self) {
				ascii_identity = false;
				break;
			}
		}
	}

	bool HasMultiByte(uint8_t first) const {
		return multi_end[first] > multi_begin[first];
	}
};

//! Indexes are built on first use and shared by every encoding name registered for the same table (aliases)
static const EncodingIndex &GetIndex(const EncodingFunction &encoding_function) {
	static std::mutex lock;
	static std::unordered_map<const map_entry_encoding *, unique_ptr<EncodingIndex>> indexes;
	std::lock_guard<std::mutex> guard(lock);
	auto &index = indexes[encoding_function.conversion_map];
	if (!index) {
		index = make_uniq<EncodingIndex>(encoding_function.conversion_map, encoding_function.map_size);
	}
	return *index;
}

void GeneratedEncodedFunction::Decode(CSVEncoderBuffer &encoded_buffer, char *target_buffer,
                                      idx_t &target_buffer_current_position, const idx_t target_buffer_size,
                                      char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                                      EncodingFunction *encoding_function) {
	const auto &index = GetIndex(*encoding_function);
	const auto map = encoding_function->conversion_map;
	const auto encoded_buffer_ptr = encoded_buffer.Ptr();
	const idx_t lookup_bytes = encoding_function->GetLookupBytes();
	const idx_t encoded_buffer_size = encoded_buffer.actual_encoded_buffer_size;
	while (encoded_buffer.cur_pos < encoded_buffer_size) {
		if (index.ascii_identity) {
			// Copy the run of ASCII bytes in bulk; ASCII is 1:1, so filling the target never splits a character
			const idx_t run = AsciiRunLength(encoded_buffer_ptr, encoded_buffer.cur_pos, encoded_buffer_size);
			if (run > 0) {
				const idx_t target_available = target_buffer_size - target_buffer_current_position;
				const idx_t to_copy = std::min(run, target_available);
				std::memcpy(target_buffer + target_buffer_current_position, encoded_buffer_ptr + encoded_buffer.cur_pos,
				            to_copy);
				target_buffer_current_position += to_copy;
				encoded_buffer.cur_pos += to_copy;
				if (to_copy == target_available || encoded_buffer.cur_pos == encoded_buffer_size) {
					return;
				}
			}
		}
		const idx_t available = encoded_buffer_size - encoded_buffer.cur_pos;
		if (available < lookup_bytes && !encoded_buffer.last_buffer) {
			// Not enough bytes to check; the remaining bytes are carried over to the next buffer by the caller
			return;
		}
		const char *current = &encoded_buffer_ptr[encoded_buffer.cur_pos];
		const auto first = static_cast<uint8_t>(*current);
		// Prefer the longest matching byte sequence
		const map_entry_encoding *match = nullptr;
		idx_t match_len = 1;
		if (index.HasMultiByte(first)) {
			const auto candidates = map + index.multi_begin[first];
			const size_t candidate_count = index.multi_end[first] - index.multi_begin[first];
			for (idx_t len = std::min<idx_t>(lookup_bytes, available); len > 1; len--) {
				match = FindEntry(candidates, candidate_count, current, len);
				if (match) {
					match_len = len;
					break;
				}
			}
		}
		if (!match) {
			match = index.single[first];
		}
		if (!match) {
			// No mapping for this byte: pass it through unchanged
			encoded_buffer.cur_pos++;
			if (target_buffer_current_position == target_buffer_size) {
				// The target buffer is full: hand the byte to the next chunk instead of writing past the end
				remaining_bytes_buffer[0] = *current;
				remaining_bytes_size = 1;
				return;
			}
			target_buffer[target_buffer_current_position++] = *current;
			continue;
		}
		encoded_buffer.cur_pos += match_len;
		for (idx_t byte_pos = 0; byte_pos < match->value_len; ++byte_pos) {
			if (target_buffer_current_position == target_buffer_size) {
				// The target buffer is full: store the rest of this character for the next chunk
				remaining_bytes_size = match->value_len - byte_pos;
				for (idx_t remaining_pos = 0; remaining_pos < remaining_bytes_size; ++remaining_pos) {
					remaining_bytes_buffer[remaining_pos] = static_cast<char>(match->value[byte_pos + remaining_pos]);
				}
				return;
			}
			target_buffer[target_buffer_current_position++] = match->value[byte_pos];
		}
	}
}

bool GeneratedEncodedFunction::KeyLess(const char *a, size_t a_len, const char *b, size_t b_len) {
	const size_t min_len = std::min(a_len, b_len);
	const int cmp = std::memcmp(a, b, min_len);
	if (cmp != 0) {
		return cmp < 0;
	}
	return a_len < b_len;
}

struct MapEntryComparator {
	bool operator()(const map_entry_encoding &entry, const std::pair<const char *, size_t> &key) const {
		return GeneratedEncodedFunction::KeyLess(entry.key, entry.key_len, key.first, key.second);
	}
};

const map_entry_encoding *GeneratedEncodedFunction::FindEntry(const map_entry_encoding *map, size_t map_size,
                                                              const char *search_key, size_t search_len) {
	const auto it =
	    std::lower_bound(map, map + map_size, std::make_pair(search_key, search_len), MapEntryComparator {});
	if (it != map + map_size && it->key_len == search_len && std::memcmp(it->key, search_key, search_len) == 0) {
		return it;
	}
	return nullptr;
}

} // namespace duckdb_encodings

} // namespace duckdb
