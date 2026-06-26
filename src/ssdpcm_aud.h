#ifndef SSDPCM_AUD_H
#define SSDPCM_AUD_H
#include <cstdint>
#include <string>
#include <vector>

#define WAVEFLAG_SSDPCM_SS2 0x00000001
#define WAVEFLAG_SSDPCM_LOOP 0x00000002

#define SSDPCM_SS2_BLOCK_SAMPLES 128
#define SSDPCM_SS2_BLOCK_BYTES 34

//! Fallback mode-4 playback rate (Hz). The actual rate is taken from each
//! .aud's stored sample rate at key-on; this is only the initial default.
#define SSDPCM_MODE4_RATE 20480

#define SSDPCM_MODE_32000 0
#define SSDPCM_MODE_22050 1
#define SSDPCM_MODE_44100 2

struct SsdpcmAudInfo
{
	uint16_t sample_rate;
	uint16_t block_len;
	uint16_t block_size;
	uint8_t initial_sample;
	uint32_t num_blocks;
	uint32_t data_offset;
	uint32_t data_size;
};

//! Parse a ss2 SSDPCM .aud file and return ROM payload bytes (initial sample + blocks).
bool ssdpcm_aud_load(const std::string& filename, SsdpcmAudInfo& info, std::vector<uint8_t>& payload);

uint8_t ssdpcm_sample_rate_to_mode(uint32_t sample_rate);

#endif
