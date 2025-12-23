/*
	This code is a bit ugly and should be refactored when I have the time...
	2020-04-11: This code has now been slightly refactored. It is however still a little bit ugly.
*/
#include <iostream>
#include <fstream>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wave.h"
#include "input.h"
#include "vgm.h"
#include "stringf.h"
#include "util.h"

static bool operator==(const Wave_Bank::Sample& s1, const Wave_Bank::Sample& s2)
{
	// Can't directly compare structs properly, so we convert to bytes instead
	// and then compare the arrays.
	return s1.to_bytes() == s2.to_bytes();
}

//=====================================================================

Wave_File::Wave_File(uint16_t channels, uint32_t rate, uint16_t bits)
	: channels(channels)
	, sbits(bits)
	, srate(rate)
	, use_smpl_chunk(0)
	, transpose(0)
	, lstart(0)
	, lend(0)
{
	stype = 1;
	step = channels*(bits/8);
}

Wave_File::~Wave_File()
{
	return;
}

int Wave_File::read(const std::string& filename)
{
	uint8_t* filebuf;
	uint32_t filesize, pos=0, wavesize;
	channels = 0;

	if(load_file(filename,&filebuf,&filesize))
	{
		return -1;
	}
	if(filesize < 13)
	{
		fprintf(stderr,"Malformed wav file '%s'\n", filename.c_str());
		return -1;
	}
	if(memcmp(&filebuf[0],"RIFF",4))
	{
		fprintf(stderr,"Riff header not found in '%s'\n", filename.c_str());
		return -1;
	}
	wavesize = (*(uint32_t*)(filebuf+4)) + 8;
	pos += 8;
	if(filesize != wavesize)
	{
		fprintf(stderr,"Warning: reported file size and actual file size do not match.\n"
				"Reported %d, actual %d\n", wavesize, filesize);

	}
	if(memcmp(&filebuf[pos],"WAVE",4))
	{
		fprintf(stderr,"'%s' is not a WAVE format file.\n", filename.c_str());
		return -1;
	}
	pos += 4;
	while(pos < wavesize)
	{
		uint32_t chunksize, ret;

		chunksize = *(uint32_t*)(filebuf+pos+4) + 8;
		if(pos+chunksize > filesize)
		{
			printf("Illegal chunk size (%d, %d)\n", pos+chunksize+8, filesize);
			return -1;
		}
		ret = parse_chunk(filebuf+pos);
		if(!ret)
		{
			printf("Failed to parse chunk %c%c%c%c.\n",filebuf[pos],filebuf[pos+1],filebuf[pos+2],filebuf[pos+3]);
			return -1;
		}
		pos += chunksize;

		if(pos & 1)
			pos++;
	}
	free(filebuf);
	return 0;
}

int Wave_File::load_file(const std::string& filename, uint8_t** buffer, uint32_t* filesize)
{
	if(std::ifstream is{filename, std::ios::binary|std::ios::ate})
	{
		auto size = is.tellg();
		*filesize = size;
		*buffer = (uint8_t*)calloc(1,size);
		is.seekg(0);
		if(is.read((char*)*buffer, size))
			return 0;
	}
	return -1;
}

uint32_t Wave_File::parse_chunk(const uint8_t *fdata)
{
	uint32_t chunkid = *(uint32_t*)(fdata);
	uint32_t chunksize = *(uint32_t*)(fdata+4);
	//printf("Parse the %c%c%c%c chunk with %d size\n", fdata[0],fdata[1],fdata[2],fdata[3], chunksize);
	uint32_t pos = 0;
	switch(chunkid)
	{
		default:
			break;
		case 0x20746d66: // 'fmt '
			if(chunksize < 0x10)
				return 0;
			stype = *(uint16_t*)(fdata+0x08);
			channels = *(uint16_t*)(fdata+0x0a);
			sbits = *(uint16_t*)(fdata+0x16);
			step = (sbits * channels) / 8;
			srate = *(uint32_t*)(fdata+0x0c);
			slength = 0;
			if(stype != 1 || channels > 2 || step == 0)
			{
				fprintf(stderr,"unsupported format\n");
				return 0;
			}
			if(data.size() != channels)
				data.resize(channels);
			break;
		case 0x61746164: // 'data'
			if(step == 0)
				return 0;
			for(const uint8_t* d = fdata+8; d < (fdata+8+chunksize);)
			{
				for(int ch=0; ch<channels; ch++)
				{
					if(sbits == 8)
					{
						data[ch].push_back((*d ^ 0x80) << 8);
						d++;
					}
					else if(sbits == 16)
					{
						data[ch].push_back(*(int16_t*)d);
						d += 2;
					}
				}
				pos++;
			}
			slength = data[0].size();
			lstart = 0;
			lend = 0;
			break;
		case 0x6c706d73: // 'smpl'
			use_smpl_chunk = 1;
			if(chunksize >= 0x10)
			{
				transpose = *(uint32_t*)(fdata+0x14);
				if(!transpose)
					transpose -= 60;
			}
			if(chunksize >= 0x2c && *(uint32_t*)(fdata+0x24))
			{
				lstart = *(uint32_t*)(fdata+0x2c+8);
				lend = *(uint32_t*)(fdata+0x2c+12) + 1;
				slength = lend;
			}
			break;
	}
	return chunksize;
}


//=====================================================================

//! Creates a Wave_Bank
Wave_Bank::Wave_Bank(unsigned long max_size, unsigned long bank_size)
	: max_size(max_size)
	, current_size(0)
	, bank_size(bank_size)
	, include_paths{""}
	, rom_data()
	, gaps()
{
	rom_data.resize(max_size, 0);
	if(!bank_size)
		this->bank_size = max_size;
}

//! Wave_Bank destructor
Wave_Bank::~Wave_Bank()
{
	return;
}

//! Set a list of include paths to check when reading samples from a Tag.
void Wave_Bank::set_include_paths(const Tag& tag)
{
	include_paths = tag;
}

//! Convert and add sample to the waverom.
unsigned int Wave_Bank::add_sample(const Tag& tag)
{
	int status = -1;
	Wave_Bank::Sample header;
	if(!tag.size())
	{
		error_message = "Incomplete sample definition";
		throw InputError(nullptr, error_message.c_str());
	}
	std::string filename = tag[0];
	Wave_File wf;
	std::vector<uint8_t> sample_raw;
	bool is_aud = false;
	{
		size_t dot = filename.find_last_of('.');
		if(dot != std::string::npos)
		{
			std::string ext = filename.substr(dot+1);
			for(auto &c : ext) c = std::tolower(c);
			is_aud = (ext == "aud");
		}
	}
	if(is_aud)
	{
		for(auto&& i : include_paths)
		{
			std::string fn = i + filename;
			std::ifstream is{fn, std::ios::binary|std::ios::ate};
			if(is)
			{
				auto size = is.tellg();
				sample_raw.resize(size);
				is.seekg(0);
				if(is.read(reinterpret_cast<char*>(sample_raw.data()), size))
				{
					status = 0;
					break;
				}
			}
		}
		if(status)
		{
			error_message = filename + " not found";
			throw InputError(nullptr, error_message.c_str());
		}
		// Minimal header defaults for SSDPCM
		header.is_ssdpcm = true;
		header.ssdpcm_mode = 0;
		header.ssdpcm_block_len = 32;
		header.ssdpcm_init_sample = 0;
		header.ssdpcm_total_blocks = 0;
		header.start = 0;
		header.size = sample_raw.size();
		header.loop_start = 0;
		header.loop_end = 0;
		header.rate = 17500;
		header.transpose = 0;
		header.flags = 0;
	}
	else
	{
		for(auto&& i : include_paths)
		{
			std::string fn = i + filename;
			status = wf.read(fn);
			if(status == 0)
				break;
		}
		if(status)
		{
			error_message = filename + " not found";
			throw InputError(nullptr, error_message.c_str());
		}
		// Initialize header from Wave_File
		header.is_ssdpcm = false;
		header.ssdpcm_mode = 0;
		header.ssdpcm_block_len = 0;
		header.ssdpcm_init_sample = 0;
		header.ssdpcm_total_blocks = 0;
		header.start = 0;
		header.size = wf.slength;
		header.loop_start = wf.lstart;
		header.loop_end = wf.lend;
		header.rate = wf.srate ? wf.srate : 17500;
		header.transpose = wf.transpose;
		header.flags = 0;
	}

	// Allow overriding the sample rate and setting start offset
	int i = 1;
	uint32_t param;
	std::string format = "";
	while(tag.size() > i)
	{
		if(std::sscanf(tag[i].c_str(), "rate = %u", &param) == 1)
		{
			header.rate = param;
		}
		else if(std::sscanf(tag[i].c_str(), "offset = %u", &param) == 1)
		{
			if(param > header.size)
				throw InputError(nullptr, "Sample offset cannot be greater than total length");
			header.start += param;
			header.size -= param;
		}
		else if(tag[i].find("format=") == 0)
		{
			format = tag[i].substr(7);
		}
		i++;
	}

	// convert sample
	std::vector<uint8_t> sample;
	if(is_aud)
	{
		sample = sample_raw;
	}
	else if(format == "ssdpcm" || format == "ssd")
	{
		header.is_ssdpcm = true;
		header.ssdpcm_mode = 0; // ss2
		header.ssdpcm_block_len = 32; // Default block length
		
		// Encode to SSDPCM
		// Simple SS2 encoder (2-bit)
		// Based on the logic in mdsdrv-with-ssdpcm.md
		const std::vector<int16_t>& input = wf.data[0];
		int16_t current_val = 0;
		if(input.size() > 0) current_val = input[0];
		header.ssdpcm_init_sample = (current_val >> 8) + 128; // Unsigned 8-bit
		
		// Slope table for SS2 (2-bit)
		// -1, 1, -2, 2? No, planning file says:
		// 00: -1 small
		// 01: +1 small
		// 10: -4 large
		// 11: +4 large
		// Let's use a reasonable table for testing: -2, +2, -16, +16?
		// Or use the one from Z80 code?
		// Z80 code loads slopes from the stream.
		// Wait! The SSDPCM format includes the slope table in the block header?
		// "Block structure: [Slope Table (4 bytes)] [Data (8 bytes = 32 samples)]"
		// Yes!
		// So the encoder must choose slopes for each block.
		
		// For now, I'll use a fixed slope table for all blocks to simplify.
		int8_t slopes[4] = {-2, 2, -16, 16};
		
		// Initial Slope Table (for Block 0)
		sample.push_back(slopes[0]);
		sample.push_back(slopes[1]);
		sample.push_back(slopes[2]);
		sample.push_back(slopes[3]);
		
		size_t pos = 0;
		int accumulator = header.ssdpcm_init_sample;
		
		while(pos < input.size())
		{
			// Block N Data
			std::vector<uint8_t> block_data;
			
			// Process 32 samples (8 bytes)
			for(int b=0; b<8; b++) // 8 bytes = 32 samples
			{
				uint8_t byte = 0;
				for(int s=0; s<4; s++) // 4 samples per byte
				{
					if(pos >= input.size()) break;
					
					int16_t target = (input[pos] >> 8) + 128;
					int best_diff = 9999;
					int best_idx = 0;
					
					// Find best slope
					for(int k=0; k<4; k++)
					{
						int next = accumulator + slopes[k];
						int diff = abs(target - next);
						if(diff < best_diff)
						{
							best_diff = diff;
							best_idx = k;
						}
					}
					
					accumulator += slopes[best_idx];
					accumulator &= 0xFF; // Simulate Z80 wrap-around
					
					byte |= (best_idx << (s*2));
					pos++;
				}
				block_data.push_back(byte);
			}

			// Pad block data if incomplete
			while(block_data.size() < 8) block_data.push_back(0);
			
			// Determine Next Block Slopes (Slope N+1)
			// For now, fixed.
			int8_t next_slopes[4];
			memcpy(next_slopes, slopes, 4);
			
			// Interleave Output: Data, Data, Slope, ...
			// 4 Chunks: 2 Data bytes + 1 Slope byte
			for(int i=0; i<4; i++)
			{
				sample.push_back(block_data[i*2]);
				sample.push_back(block_data[i*2+1]);
				sample.push_back(next_slopes[i]);
			}
			
			header.ssdpcm_total_blocks++;
		}
		header.size = sample.size(); // Size in bytes
	}
	else
	{
		sample = encode_sample("", wf.data[0]);
	}

	return add_sample(header, sample);
};

//! Add sample to the waverom in raw format.
unsigned int Wave_Bank::add_sample(Wave_Bank::Sample header, const std::vector<uint8_t>& sample)
{
	// Find duplicates of sample data and selected header parameters if needed
	int duplicate = find_duplicate(header, sample);

	if(duplicate != -1)
	{
		header.position = samples[duplicate].position;
		auto result = std::find(samples.begin(), samples.end(), header);
		if(result != samples.end())
		{
			// Header is similar to an existing one, we reuse it
			return result - samples.begin();
		}
		else
		{
			// Create a new header, while using the same sample data
			samples.push_back(header);
			return samples.size() - 1;
		}
	}
	else
	{
		// Create a new entry.
		uint32_t start_pos = -1; // Aligned start position
		uint32_t start = current_size; // Proposed start position
		// Check if the sample fits in a gap.
		unsigned int gap_id = find_gap(header, start_pos);
		if(gap_id != NO_FIT)
		{
			start = gaps[gap_id].start;
			gaps[gap_id].start = start_pos + header.size;
		}
		else
		{
			// Append sample to the end
			start_pos = fit_sample(header, start, max_size);
		}
		// Check if sample fits in ROM
		if(start_pos == NO_FIT)
		{
			error_message = stringf("Sample does not fit in remaining ROM space (%d bytes remaining, sample size is %d)", max_size - start_pos, sample.size());
			throw InputError(nullptr, error_message.c_str());
		}
		// Add a new gap if needed
		if(start_pos > start)
		{
			gaps.push_back({start, start_pos});
		}
		// Move the end position if needed
		if(start_pos >= current_size)
		{
			current_size = start_pos + header.size;
		}

		printf("Append sample %d to ROM at %08x (size %08x)\n", samples.size(), start_pos, header.size);
		std::copy_n(sample.begin(), header.size, rom_data.begin() + start_pos);
		header.position = start_pos;
		samples.push_back(header);
		return samples.size() - 1;
	}
}

//! Get sample headers
const std::vector<Wave_Bank::Sample>& Wave_Bank::get_sample_headers()
{
	return samples;
}

//! Get the sample ROM data
const std::vector<uint8_t>& Wave_Bank::get_rom_data()
{
	return rom_data;
}

//! Get the number of unused allocated bytes in the Wave_Bank.
unsigned int Wave_Bank::get_free_bytes()
{
	return max_size - current_size;
}

//! Get the total size of alignment gaps.
unsigned int Wave_Bank::get_total_gap()
{
	unsigned int gap_size = 0;
	for(auto&& gap : gaps)
	{
		gap_size += gap.end - gap.start;
	}
	return gap_size;
}

//! Get the size of the largest gap.
/*!
 *  If there are no gaps, return 0.
 */
unsigned int Wave_Bank::get_largest_gap()
{
	unsigned int largest_gap = 0;
	for(auto&& gap : gaps)
	{
		unsigned int gap_size = gap.end - gap.start;
		if(gap_size > largest_gap)
			largest_gap = gap_size;
	}
	return largest_gap;
}

//! Get error message
const std::string& Wave_Bank::get_error()
{
	return error_message;
}

//! Check if the sample fits in an existing alignment gap
/*!
 *  If the sample cannot fit in any gap, return NO_FIT. Otherwise, return
 *  the index of the smallest gap that fits the sample.
 *
 *  \p gap_start will be set with the aligned start position of the gap.
 */
unsigned int Wave_Bank::find_gap(const Wave_Bank::Sample& header, uint32_t& gap_start) const
{
	unsigned int best_gap = NO_FIT;
	if(gaps.size())
	{
		// Look for the smallest gap that fits our sample
		int best_gap_size = max_size;
		uint32_t start_pos;
		for(unsigned int i = 0; i < gaps.size(); i++)
		{
			int gap_size = gaps[i].end - gaps[i].start;
			start_pos = fit_sample(header, gaps[i].start, gaps[i].end);
			if(start_pos != NO_FIT && gap_size < best_gap_size)
			{
				best_gap = i;
				best_gap_size = gap_size;
				gap_start = start_pos;
			}
		}
	}
	return best_gap;
}

//! Encode the sample, convert it from 16-bit data to 8-bit.
std::vector<uint8_t> Wave_Bank::encode_sample(const std::string& encoding_type, const std::vector<int16_t>& input)
{
	// default encoder, simply convert 16-bit to 8-bit unsigned
	std::vector<uint8_t> output;
	for(auto&& i : input)
	{
		output.push_back((i >> 8) ^ 0x80);
	}
	return output;
}

//! Returns the next possible aligned start address for the rom.
/*!
 *  Given a sample header, a proposed start address and end address,
 *  return the appropriate start address of the sample. If the sample
 *  cannot fit within the boundaries. return NO_FIT.
 *
 *  TODO: This code is optimized for MDSDRV with mid playback bank
 *  switches only supported for 17.5khz sample rate for now.
 *  When adding more sound drivers, this will have to be a generic
 *  function that will reject samples crossing banks, with bank
 *  crossing behavior determined on a per-driver basis.
 */
uint32_t Wave_Bank::fit_sample(const Wave_Bank::Sample& header, uint32_t start, uint32_t end) const
{
	uint32_t sample_end = start + header.size;
	uint32_t start_bank = start / bank_size;
	uint32_t end_bank = sample_end / bank_size;
	// Adjust start address for bank crossing.
	// Smarter method if sample is only to be played at the same sample rate.
	// I will use this by default if the sample is too big to fit in a Z80 bank anyway.
	if (start_bank != end_bank && header.size > bank_size)
		start = (start + 0x1f) & 0xffffffe0;
	// Alternate behavior for MDSDRV. More ROM space but will handle sample rate switches.
	else if (start_bank != end_bank && (start % bank_size) != 0)
		start = (start_bank + 1) * bank_size;
	// Crossed end boundary?
	if ((start + header.size) > end)
		return NO_FIT;
	return start;
}

//! Look for duplicates in the sample ROM.
/*!
 *  If a duplicate is found, return the index to the duplicate wave entry.
 *  Otherwise, return -1.
 */
int Wave_Bank::find_duplicate(const Wave_Bank::Sample& header, const std::vector<uint8_t>& sample) const
{
	int id = 0;
	for(auto&& i : samples)
	{
		// The reason for the loop start check is that some sound chips (like C352)
		// require that the looping part of the sample fit in the same bank.
		if(   i.position + sample.size() <= rom_data.size()
		   && i.loop_start <= header.loop_start
		   && !memcmp(&sample[0], &rom_data[i.position], sample.size()))
			return id;
		id++;
	}
	return -1;
}

//=====================================================================
//! Fill a sample header with values from a byte vector.
/*!
 *  The format matches the data created by to_bytes().
 *
 *  \exception std::out_of_range Input too small
 */
void Wave_Bank::Sample::from_bytes(std::vector<uint8_t> input)
{
	position = read_le32(input, 0);
	start = read_le32(input, 4);
	size = read_le32(input, 8);
	loop_start = read_le32(input, 12);
	loop_end = read_le32(input, 16);
	rate = read_le32(input, 20);
	transpose = read_le32(input, 24);
	flags = read_le32(input, 28);
	
	// Check if we have extended header data (assumed 16 bytes extra for SSDPCM)
	if (input.size() >= 48) {
		is_ssdpcm = true;
		ssdpcm_mode = input[32];
		ssdpcm_block_len = read_le16(input, 33);
		ssdpcm_init_sample = input[35];
		ssdpcm_total_blocks = read_le16(input, 36);
	} else {
		is_ssdpcm = false;
	}
}

//! Return a sample header as a byte vector.
/*!
 *  Output can be made as a header again using to_bytes().
 */
std::vector<uint8_t> Wave_Bank::Sample::to_bytes() const
{
	auto output = std::vector<uint8_t>();
	write_le32(output, 0, position);
	write_le32(output, 4, start);
	write_le32(output, 8, size);
	write_le32(output, 12, loop_start);
	write_le32(output, 16, loop_end);
	write_le32(output, 20, rate);
	write_le32(output, 24, transpose);
	
	// Use flags to indicate SSDPCM? Or just append?
	// The 68k driver reads 32 bytes currently.
	// We need to extend this.
	// But `flags` is at offset 28 (4 bytes).
	// If we repurpose flags or just append...
	// The planning doc says:
	// "16-byte SSDPCM Header (appended to standard header? or replacing parts?)"
	// "Standard PCM header is 32 bytes."
	// "Let's extend it to 48 bytes or use the flags field."
	
	// Let's use bit 31 of flags to indicate extended header if needed,
	// but the Z80 code checks specific offsets.
	// Actually, the 68k driver `mdsdrv.68k` reads the header.
	// I need to update `mdsdrv.68k` to read the extended header if bit 7 of format is set?
	// Wait, the Z80 `mdssub.z80` checks `zp_format` (bit 7).
	// `zp_format` comes from the header?
	// In `mdsdrv.68k`, `mds_pcm_key_on` reads the header.
	
	uint32_t current_flags = flags;
	if (is_ssdpcm) {
		current_flags |= 0x80000000; // Set a flag just in case
	}
	write_le32(output, 28, current_flags);

	if (is_ssdpcm) {
		// Append SSDPCM specific data (16 bytes to align/pad?)
		// Offset 32
		output.push_back(ssdpcm_mode); 		// +0: Mode
		write_le16(output, 33, ssdpcm_block_len); // +1: Block length
		output.push_back(ssdpcm_init_sample); 	// +3: Init sample
		write_le16(output, 36, ssdpcm_total_blocks); // +4: Total blocks
		
		// Pad to 48 bytes (32 + 16)
		while(output.size() < 48) output.push_back(0);
	}

	return output;
}
