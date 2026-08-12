#include "generated_encoded_function.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"

#include <cstring>

namespace duckdb {
namespace duckdb_encodings {

namespace {

//! Number of distinct byte values, i.e. slots in a direct-lookup table.
constexpr idx_t DIRECT_TABLE_SIZE = 256;
//! First non-ASCII byte value; bytes below this are ASCII.
constexpr idx_t ASCII_LIMIT = 0x80;
//! Mask selecting the high bit of every byte in a 64-bit word.
constexpr uint64_t HIGH_BITS = 0x8080808080808080ULL;

//! Direct-lookup table entry for single-byte codecs. Entries are 16 bytes so
//! the hot loop can issue one unconditional 8-byte store and advance by len.
struct DirectEntry {
	char bytes[8];
	uint8_t len;
};

struct DirectTable {
	DirectEntry entries[DIRECT_TABLE_SIZE];
	//! True when every byte 0x00-0x7F maps to itself (latin/windows family).
	//! False for e.g. EBCDIC codecs, which are single-byte but not
	//! ASCII-compatible; those use the per-byte loop instead of span copies.
	bool ascii_identity;
};

mutex direct_cache_lock;
//! Keyed by the codec's static map pointer; nullptr caches "ineligible".
unordered_map<const map_entry_encoding *, unique_ptr<DirectTable>> direct_cache;

const DirectTable *GetDirectTable(const EncodingFunction &function) {
	lock_guard<mutex> guard(direct_cache_lock);
	const auto cache_entry = direct_cache.find(function.conversion_map);
	if (cache_entry != direct_cache.end()) {
		return cache_entry->second.get();
	}
	auto table = make_uniq<DirectTable>();
	// Bytes absent from the map pass through unchanged, matching the
	// generic path's !did_replacement behaviour.
	for (idx_t byte_value = 0; byte_value < DIRECT_TABLE_SIZE; byte_value++) {
		std::memset(table->entries[byte_value].bytes, 0, sizeof(table->entries[byte_value].bytes));
		table->entries[byte_value].bytes[0] = static_cast<char>(byte_value);
		table->entries[byte_value].len = 1;
	}
	bool eligible = true;
	for (idx_t entry_idx = 0; entry_idx < function.map_size; entry_idx++) {
		const auto &entry = function.conversion_map[entry_idx];
		if (entry.key_len != 1 || entry.value_len > sizeof(DirectEntry::bytes)) {
			eligible = false;
			break;
		}
		auto &slot = table->entries[static_cast<unsigned char>(entry.key[0])];
		std::memset(slot.bytes, 0, sizeof(slot.bytes));
		std::memcpy(slot.bytes, entry.value, entry.value_len);
		slot.len = static_cast<uint8_t>(entry.value_len);
	}
	if (eligible) {
		table->ascii_identity = true;
		for (idx_t byte_value = 0; byte_value < ASCII_LIMIT; byte_value++) {
			if (table->entries[byte_value].len != 1 ||
			    table->entries[byte_value].bytes[0] != static_cast<char>(byte_value)) {
				table->ascii_identity = false;
				break;
			}
		}
	}
	auto &cached = direct_cache[function.conversion_map];
	if (eligible) {
		cached = std::move(table);
	}
	return cached.get();
}

void DecodeSingleByte(const DirectTable &table, CSVEncoderBuffer &encoded_buffer, char *target_buffer,
                      idx_t &target_buffer_current_position, const idx_t target_buffer_size,
                      char *remaining_bytes_buffer, idx_t &remaining_bytes_size) {
	const auto encoded_ptr = encoded_buffer.Ptr();
	auto cur = encoded_buffer.cur_pos;
	const auto end = encoded_buffer.actual_encoded_buffer_size;
	auto pos = target_buffer_current_position;
	while (cur < end) {
		if (table.ascii_identity) {
			// Bulk-copy the run of ASCII bytes starting at cur.
			idx_t run = 0;
			while (cur + run + sizeof(uint64_t) <= end) {
				uint64_t word;
				std::memcpy(&word, encoded_ptr + cur + run, sizeof(uint64_t));
				if (word & HIGH_BITS) {
					break;
				}
				run += sizeof(uint64_t);
			}
			while (cur + run < end && static_cast<unsigned char>(encoded_ptr[cur + run]) < ASCII_LIMIT) {
				run++;
			}
			if (run) {
				const idx_t available = target_buffer_size - pos;
				if (run >= available) {
					// Target fills inside the ASCII run: copy what fits and
					// stop. ASCII is 1:1, so nothing spills.
					std::memcpy(target_buffer + pos, encoded_ptr + cur, available);
					pos += available;
					cur += available;
					encoded_buffer.cur_pos = cur;
					target_buffer_current_position = pos;
					return;
				}
				std::memcpy(target_buffer + pos, encoded_ptr + cur, run);
				pos += run;
				cur += run;
				if (cur >= end) {
					break;
				}
			}
		}
		// One byte through the table (high byte, or non-ASCII-identity codec).
		const auto &entry = table.entries[static_cast<unsigned char>(encoded_ptr[cur])];
		if (pos + sizeof(entry.bytes) <= target_buffer_size) {
			// Room for an unconditional 8-byte store; advance by real length.
			std::memcpy(target_buffer + pos, entry.bytes, sizeof(entry.bytes));
			pos += entry.len;
			cur++;
			continue;
		}
		if (pos + entry.len <= target_buffer_size) {
			// Near the end of the target: careful exact-length copy.
			for (uint8_t byte_pos = 0; byte_pos < entry.len; byte_pos++) {
				target_buffer[pos++] = entry.bytes[byte_pos];
			}
			cur++;
			continue;
		}
		// Target full mid-value: emit what fits, stash the rest for the next
		// chunk. Mirrors the generic path (input byte counts as consumed).
		uint8_t emitted = 0;
		while (pos < target_buffer_size) {
			target_buffer[pos++] = entry.bytes[emitted++];
		}
		remaining_bytes_size = entry.len - emitted;
		for (idx_t remaining_pos = 0; remaining_pos < remaining_bytes_size; remaining_pos++) {
			remaining_bytes_buffer[remaining_pos] = entry.bytes[emitted + remaining_pos];
		}
		cur++;
		encoded_buffer.cur_pos = cur;
		target_buffer_current_position = pos;
		return;
	}
	encoded_buffer.cur_pos = cur;
	target_buffer_current_position = pos;
}

} // namespace

void GeneratedEncodedFunction::Decode(CSVEncoderBuffer &encoded_buffer, char *target_buffer,
                                      idx_t &target_buffer_current_position, const idx_t target_buffer_size,
                                      char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                                      EncodingFunction *encoding_function) {
	if (encoding_function->GetLookupBytes() == 1) {
		const auto direct = GetDirectTable(*encoding_function);
		if (direct) {
			DecodeSingleByte(*direct, encoded_buffer, target_buffer, target_buffer_current_position, target_buffer_size,
			                 remaining_bytes_buffer, remaining_bytes_size);
			return;
		}
	}
	const auto encoded_buffer_ptr = encoded_buffer.Ptr();
	const int lookup_bytes = static_cast<int>(encoding_function->GetLookupBytes());
	while (encoded_buffer.cur_pos < encoded_buffer.actual_encoded_buffer_size) {
		// We need to use our map from the highest to lowest lookup bytes
		if (encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos < lookup_bytes &&
		    !encoded_buffer.last_buffer) {
			// Not enough bytes to check.
			return;
		}
		int byte_group = lookup_bytes;
		if (encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos < lookup_bytes) {
			byte_group = static_cast<int>(encoded_buffer.actual_encoded_buffer_size - encoded_buffer.cur_pos);
		}
		bool did_replacement = false;
		for (; byte_group > 0; --byte_group) {
			// We found a match, do the conversion
			const auto it = FindEntry(encoding_function->conversion_map, encoding_function->map_size,
			                          &encoded_buffer_ptr[encoded_buffer.cur_pos], byte_group);
			if (it != nullptr) {
				// We walk the byte group size from our buffer source
				did_replacement = true;
				encoded_buffer.cur_pos += byte_group;
				for (idx_t byte_pos = 0; byte_pos < it->value_len; ++byte_pos) {
					if (target_buffer_current_position == target_buffer_size) {
						// We are done, but we have to store one byte for the next chunk!
						remaining_bytes_size = it->value_len - byte_pos;
						for (idx_t remaining_pos = 0; remaining_pos < remaining_bytes_size; ++remaining_pos) {
							remaining_bytes_buffer[remaining_pos] =
							    static_cast<char>(it->value[byte_pos + remaining_pos]);
						}
						return;
					}
					target_buffer[target_buffer_current_position++] = it->value[byte_pos];
				}
				// We finished off this group
				break;
			}
		}
		if (!did_replacement) {
			// If we got here, it means replacements are not necessary
			target_buffer[target_buffer_current_position++] = encoded_buffer_ptr[encoded_buffer.cur_pos++];
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
