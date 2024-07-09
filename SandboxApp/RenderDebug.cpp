#include "RenderDebug.h"

#ifdef PROFILE
#include <string>
#include <Windows.h>
#include <cassert>
#include <chrono>
#include <pix3.h>

#ifdef DX12
void BeginPIXCap(wchar_t* captureTimeStr)
{
	std::wstring wstr(L"dxrSandboxCap");
	
	wstr += captureTimeStr;
	wstr += L".wpix";

	PIXCaptureParameters capParams = {};
	capParams.GpuCaptureParameters.FileName = wstr.c_str();

	HRESULT hr = PIXBeginCapture(PIX_CAPTURE_GPU, &capParams);
	assert(SUCCEEDED(hr));
}

void EndPIXCap()
{
	PIXEndCapture(false);
}
#endif

#ifdef VK
void BeginRenderdocCap(wchar_t* captureTimeStr)
{
	// Trigger capture through Renderdoc's Python API
}

void EndRenderdocCap()
{
	// Not sure if this is necessary or not
}
#endif

void RenderDebug::Init()
{
#ifdef DX12
	// Load PIX capture library in profile mode
	PIXLoadLatestWinPixGpuCapturerLibrary();
#else
	// Reboot application through Renderdoc via Python
#endif
}

void RenderDebug::BeginCapture()
{
	// Generate string of current time, to uniquely name each capture
	const auto t = std::chrono::high_resolution_clock::now();
	const uint64_t captureTime = t.time_since_epoch().count();
	
	wchar_t capture_time_charbuf[1024] = {};
	swprintf_s(capture_time_charbuf, L"%llu", captureTime);

#ifdef DX12
	// DX12 captures assume title launched from PIX
	BeginPIXCap(capture_time_charbuf);
#endif

#ifdef VK
	// Vulkan captures assume title launched from Renderdoc
	BeginRenderdocCap(capture_time_charbuf);
#endif
}

void RenderDebug::EndCapture()
{
#ifdef PROFILE
#ifdef DX12
	// DX12 captures assume title launched from PIX
	EndPIXCap();
#endif

#ifdef VK
	// Vulkan captures assume title launched from Renderdoc
	EndRenderdocCap();
#endif
#endif
}

#else

void RenderDebug::Init()
{
}

void RenderDebug::BeginCapture()
{
}

void RenderDebug::EndCapture()
{
}

#endif