
// One stream (per- ~pixel/path) of progressive randomness, using the Xoshiro128+ generator (since no in-built random generators in DX12 atm)
// See:
// https://prng.di.unimi.it
// https://prng.di.unimi.it/xoshiro128plus.c
// Sampled/iterated on GPU, seeded on CPU
// Separately seeding channels will not guarantee non-overlapping sequences, so prefer seeding all channels to the same value and jumping n times for each, where n is one-based index of the channel

#define GPU_PRNG

#ifndef _WIN32
#define uint32_t uint
#endif

#define GPU_PRNG_STREAM_STATE_SIZE 4
struct GPU_PRNG_Channel
{
	uint32_t state[GPU_PRNG_STREAM_STATE_SIZE];
};

#ifdef _WIN32
#define GPU_PRNG_ChannelType GPU_PRNG_Channel&
#else
#define GPU_PRNG_ChannelType inout GPU_PRNG_Channel
#endif

// Iteration from xoshiro128+ implementation, here: https://prng.di.unimi.it/xoshiro128plus.c
uint GPU_PRNG_Next(GPU_PRNG_ChannelType channel)
{
	const uint32_t result = channel.state[0] + channel.state[3];
	const uint32_t t = channel.state[1] << 9;

	channel.state[2] ^= channel.state[0];
	channel.state[3] ^= channel.state[1];
	channel.state[1] ^= channel.state[2];
	channel.state[0] ^= channel.state[3];
	channel.state[2] ^= t;
	channel.state[3] = (channel.state[3] << 11) | (channel.state[3] >> (32 - 11));

	return result;
}

#ifndef _WIN32
// Convert full-range uint to float in 0...1
float iToFloat(uint i)
{
	return float(i) / 4294967295.0f;
}

float rand(GPU_PRNG_ChannelType channel)
{
	return iToFloat(GPU_PRNG_Next(channel));
}

float2 rand2d(GPU_PRNG_ChannelType channel)
{
	return float2(iToFloat(GPU_PRNG_Next(channel)),
				  iToFloat(GPU_PRNG_Next(channel)));
}

float3 rand3d(GPU_PRNG_ChannelType channel)
{
	return float3(iToFloat(GPU_PRNG_Next(channel)),
				  iToFloat(GPU_PRNG_Next(channel)),
				  iToFloat(GPU_PRNG_Next(channel)));
}
#endif
