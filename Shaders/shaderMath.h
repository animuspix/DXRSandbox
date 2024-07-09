
#define SHADER_MATH

#define ONE_OVER_4PI 0.0795
#define ONE_OVER_2PI 0.1591

// SQT-style transform
// I'll never surrender to matrices !!!
struct transform
{
	float4 translationAndScale; // XYZ pos, W linear scale
	float4 rotation; // Quaternion rotation (sin x axis, cos(angle))
};