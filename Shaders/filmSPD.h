
// Piecewise film response curve for spectral rendering; significantly deeper than surface spectra because we only expect to support one camera
#define FILM_SPD_NUM_SAMPLES 256
#define MAX_FILM_CURVE_CONSTRAINT (FILM_SPD_NUM_SAMPLES - 1)

#ifdef _WIN32
#pragma once
#endif

struct FilmSPD_Piecewise
{
	float4 spd_sample[FILM_SPD_NUM_SAMPLES];
};