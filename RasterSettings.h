#pragma once

#include <stdint.h>

// Specialized settings for graphics pipelines - we could hardcode these, but they're important enough and useful enough that I'd prefer to expose them instead
// Even things like front/back culling or winding order can be helpful for geometry debugging
struct RasterSettings
{
	// Different types of per-sample stencil/depth test
	enum class DEPTH_STENCIL_TEST_TYPES
	{
		NEVER, // Pass never
		LESS, // Pass when stencil values are smaller than the current buffer sample
		EQUAL, // Pass when per-sample stencil values are equal to buffered values (as in a traditional drawing stencil)
		LESS_OR_EQUAL,
		GREATER, // Pass when stencil values are greater than the current buffer sample
		GREATER_OR_EQUAL,
		NOT_EQUAL, // Pass when per-sample stencil values are different to buffered values
		ALWAYS // Pass always
	};

	// Rasterizer fill modes
	enum FILL_MODES
	{
		FILL_WIREFRAME,
		FILL_SOLID
	};

	// Whether to cull front or back-facing triangles
	enum CULL_MODES
	{
		CULL_FRONT,
		CULL_BACK
	};

	// Do front-facing triangles have clockwise or counter-clockwise vertices?
	enum WINDING_MODE
	{
		WIND_CW,
		WIND_CCW
	};

	// What to do when stencil testing fails, passes, or passes despite depth-testing failing
	// (stencil testing for the very first draw should always pass, so these de-facto say whether the stencil should keep existing values, zero them,
	// replace them with the stencil value, increment
	enum STENCIL_OP_TYPES
	{
		STENCIL_OP_KEEP, // Preserve the stencil data under the current sample
		STENCIL_OP_ZERO, // Zero the stencil data under the current sample
		STENCIL_OP_INCREMENT_CLAMPED, // Increment & clamp the stencil data under the current sample (to the stencil buffer's range, 0-255)
		STENCIL_OP_DECREMENT_CLAMPED, // Decrement & clamp the stencil data under the current sample (to the stencil buffer's range, 0-255)
		STENCIL_OP_INVERT, // Invert the stencil data under the current sample
		STENCIL_OP_INCREMENT_WRAPPED, // Increment the current sample with regular unsigned overflow behaviour (256 wraps to zero)
		STENCIL_OP_DECREMENT_WRAPPED // Decrement the current sample with regular unsigned overflow behaviour (-1 wraps to 255)
	};

	struct StencilSettings
	{
		bool enabled = false;

		// Masks specifying which bits within the stencil-buffer contain the "stencil" used in the current frame
		uint8_t stencilReadMask = 0xff;
		uint8_t stencilWriteMask = 0xff;

		// A reference value for stencil operations
		// This is the core, actual "stencil" used by the stencil test - the idea is that you draw the geometry you want to use as your stencil in one pass
		// (with the stencil test set to ALWAYS to push the data through), and then in another pass you draw the geometry you want to test against the stencil
		uint8_t stencilValue = 0;

		// Just one of these passed in from user code; backface/frontface culling will determine whether its set for the front/back-face in the D3D12 renderer
		// The culled face (usually backfaces) will have no stencilling enabled (so KEEP, KEEP, KEEP, NEVER)
		struct StencilDesc
		{
			STENCIL_OP_TYPES stencilFailOp = STENCIL_OP_KEEP;
			STENCIL_OP_TYPES stencilPassOp = STENCIL_OP_KEEP;
			STENCIL_OP_TYPES depthFailStencilPassOp = STENCIL_OP_KEEP;
			DEPTH_STENCIL_TEST_TYPES stencilTest = DEPTH_STENCIL_TEST_TYPES::ALWAYS; // Always pass the stencil test by default
		};
		StencilDesc stencilOpDesc;
	};
	StencilSettings stencil;

	struct DepthSettings
	{
		bool enabled = true;
		DEPTH_STENCIL_TEST_TYPES depthTest = DEPTH_STENCIL_TEST_TYPES::LESS;
	};
	DepthSettings depth;

	struct CoreRasterSettings
	{
		// Enable far-plane clipping
		// Confusingly equivalent to DepthClipEnable, see:
		// https://github.com/gpuweb/gpuweb/issues/2100#:~:text=According%20to%20D3D12%20document%2C%20%22the%20hardware%20always%20performs,shader%20stage%3A%20discard%20the%20specific%20coordinates%20after%20rasterization.
		bool clipDistant = false;

		// Enable conservative rasterization (triangles often won't line up with the pixel grid, so rasterization will try to shade just the pixels within the area of the triangle (the lower boundary of the triangle's pixel area) - conservative
		// rasterization tells the rasterizer to shade the pixels just outside the triangle as well, or the upper boundary of the triangle's pixel area. That's why its called "conservative")
		bool conservativeRaster = false;

		// Active fillmode, cullmode, and winding-order/mode
		FILL_MODES fillMode;
		CULL_MODES cullMode;
		WINDING_MODE windMode;
	};
	CoreRasterSettings coreRaster;

	struct MSAASettings
	{
		bool enabled = false; // Enabling MSAA with wireframe fill-mode will enable anti-aliased line draws
		uint8_t forcedSamples = 0; // No minimum/enforced sample count per-pixel by default; can be used for custom AA schemes at the cost of some restrictions
								   // (see: https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc)
		uint8_t expectedSamples = 1; // No AA by default - just one sample per-pixel 
		uint8_t qualityTier = 0; // No multisampling by default
								 // The highest tier available for a given texture on the current adapter can be found by calling [GetMaxMSAAQualityLevelForTexture()] afrter setup (see below)
	};
	MSAASettings msaaSettings;
};