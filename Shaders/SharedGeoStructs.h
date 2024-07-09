
#if defined(VERTEX) || defined(_WIN32) || defined(COMPUTE)
struct Vertex2D
{
	#ifdef _WIN32
		float4 pos; // Throwing everything in POSITION will confuse the rasterizer
		float4 uv; // XY active, ZW unused
	#else
    	float4 pos : POSITION;
    	float4 uv : TEXCOORD;	
	#endif
};

// Not 100% sure normals & material settings should be included here - might be annoying for raytracing
// Not a concern until we get to DXR testing though, we just want to load/generate geometry for now
struct Vertex3D
{
	#ifdef _WIN32
		float4 pos; // W is unused
		float4 mat; // UVs in x,y, material/model ID in z, scattering function ID in w
		float4 normals; // W is unused
	#else
		float4 pos : POSITION; // W is unused
		float4 mat : TEXCOORD; // UVs in x,y, material/model ID in z, scattering function ID in w
		float4 normals : NORMAL; // W is unused
	#endif
};
#endif