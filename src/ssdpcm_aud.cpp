#include "ssdpcm_aud.h"
#include "wave.h"
#include "core.h"
#include <cstring>
#include <fstream>

static uint16_t read_u16_le(const uint8_t* p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool chunk_matches(const uint8_t* p, const char* id)
{
	return p[0] == (uint8_t)id[0] && p[1] == (uint8_t)id[1] &&
	       p[2] == (uint8_t)id[2] && p[3] == (uint8_t)id[3];
}

uint8_t ssdpcm_sample_rate_to_mode(uint32_t sample_rate)
{
	switch(sample_rate)
	{
		case 32000: return SSDPCM_MODE_32000;
		case 22050: return SSDPCM_MODE_22050;
		case 44100: return SSDPCM_MODE_44100;
		default: break;
	}
	if(sample_rate >= 36000)
		return SSDPCM_MODE_44100;
	if(sample_rate >= 27000)
		return SSDPCM_MODE_32000;
	return SSDPCM_MODE_22050;
}

bool ssdpcm_aud_load(const std::string& filename, SsdpcmAudInfo& info, std::vector<uint8_t>& payload)
{
	std::ifstream file(filename, std::ios::binary | std::ios::ate);
	if(!file)
		return false;

	auto file_size = file.tellg();
	if(file_size < 12)
		return false;

	std::vector<uint8_t> data((size_t)file_size);
	file.seekg(0);
	file.read((char*)data.data(), file_size);
	if(!file)
		return false;

	const uint8_t* aud = data.data();
	uint32_t len = (uint32_t)file_size;

	if(!chunk_matches(aud, "RIFF") || !chunk_matches(aud + 8, "WAVE"))
		return false;

	uint32_t pos = 12;
	uint32_t data_off = 0;
	uint32_t data_size = 0;
	bool got_fmt = false;
	bool got_data = false;
	uint16_t sample_rate = 0;
	uint16_t block_len = 0;
	uint16_t block_size = 0;
	uint8_t has_ref_every = 0;

	while(pos + 8 <= len)
	{
		uint32_t chunk_size = read_u32_le(aud + pos + 4);
		uint32_t chunk_data = pos + 8;
		if(chunk_data > len || chunk_data + chunk_size > len)
			return false;

		uint32_t next = chunk_data + chunk_size + (chunk_size & 1);
		if(next > len)
			next = len;

		if(chunk_matches(aud + pos, "fmt ") && chunk_size >= 56)
		{
			if(read_u16_le(aud + chunk_data) != 0xFFFE)
				return false;
			if(read_u16_le(aud + chunk_data + 2) != 1)
				return false;

			sample_rate = (uint16_t)read_u32_le(aud + chunk_data + 4);
			block_len = read_u16_le(aud + chunk_data + 52);
			block_size = read_u16_le(aud + chunk_data + 54);
			has_ref_every = aud[chunk_data + 51];

			if(!chunk_matches(aud + chunk_data + 44, "ss2 "))
				return false;
			if(block_len != SSDPCM_SS2_BLOCK_SAMPLES)
				return false;
			(void)block_size;
			if(has_ref_every)
				return false;

			got_fmt = true;
		}
		else if(chunk_matches(aud + pos, "data"))
		{
			data_off = chunk_data;
			data_size = chunk_size;
			got_data = true;
		}

		if(got_fmt && got_data)
			break;

		if(next <= pos)
			return false;
		pos = next;
	}

	if(!got_fmt || !got_data || data_size < 2)
		return false;

	uint32_t blocks = (data_size - 1) / SSDPCM_SS2_BLOCK_BYTES;
	if(blocks == 0)
		return false;

	uint32_t payload_size = 1 + blocks * SSDPCM_SS2_BLOCK_BYTES;
	if(data_size < payload_size)
		return false;

	if(data_off + payload_size > len)
		return false;

	info.sample_rate = sample_rate;
	info.block_len = block_len;
	info.block_size = block_size;
	info.initial_sample = aud[data_off];
	info.num_blocks = blocks;
	info.data_offset = data_off;
	info.data_size = payload_size;

	payload.assign(aud + data_off, aud + data_off + payload_size);
	return true;
}
