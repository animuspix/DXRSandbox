#pragma once

// DXRSandbox uses GLM for Vulkan, DirectXMath for DX12

#include <cmath>

#ifdef VULKAN

#include "thirdparty/glm-0.9.9.8/glm/mat3x4.hpp"
using matrix = glm::mat4x4;
using vec4 = glm::vec4;
using float4 = glm::vec4;
using frac = glm::frac;

#define vec4FromFloat4(f4) f4

#elif DX12

#include <DirectXMath.h>

using matrix = DirectX::XMMATRIX;
using vec4 = DirectX::XMVECTOR;
using float4 = DirectX::XMFLOAT4;
using uint4 = DirectX::XMUINT4;
#define cross DirectX::XMVector3Cross

#define vec4FromFloat4(f4) DirectX::XMLoadFloat4(f4) 
#define float4FromVec4(f4, v4) DirectX::XMStoreFloat4(f4, v4) 

#define vec4Subtract(u, v) DirectX::XMVectorSubtract(u, v)
#define vec4Add(u, v) DirectX::XMVectorAdd(u, v)
#define vec4Div(u, v) DirectX::XMVectorDivide(u, v)

#define normalize(u) DirectX::XMVector3Normalize(u)

#endif

#ifndef SHADER_MATH
#include "Shaders/shaderMath.h"
#endif

struct uvec3
{
	uint32_t x = 0, y = 0, z = 0;
};

struct uvec4
{
	uint32_t x = 0, y = 0, z = 0, w = 0;
};
