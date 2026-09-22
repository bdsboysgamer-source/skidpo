#pragma once

#include "../common/Types.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

namespace fb {

enum class CaptureStatus {
    Ok,          // a new frame was captured into outFrame
    NoNewFrame,  // poll timed out or only the cursor changed; not an error
    DeviceLost,  // duplication interface lost; caller should Reinitialize()
    Failed,      // unrecoverable-ish failure; see LastError()
};

// Wraps DXGI Desktop Duplication for low-latency capture of a small screen
// sub-rectangle. Only the requested rect (ROI + background flank) is ever
// copied to CPU memory - the full desktop frame stays in GPU memory.
class DesktopDuplication {
public:
    DesktopDuplication() = default;
    ~DesktopDuplication();

    DesktopDuplication(const DesktopDuplication&) = delete;
    DesktopDuplication& operator=(const DesktopDuplication&) = delete;

    // Selects the DXGI output whose desktop bounds contain the ROI's
    // screen-space top-left point, creates a D3D11 device against its
    // adapter, and duplicates that output.
    bool Initialize(const RoiConfig& roi, std::wstring& outError);

    void Shutdown();
    bool IsInitialized() const { return m_duplication != nullptr; }

    // Blocks up to timeoutMs for a new desktop frame, then copies only the
    // roi.CaptureWidth() x roi.CaptureHeight() sub-rect (screen-space
    // origin roi.CaptureOriginScreenX/Y) into outFrame.
    CaptureStatus AcquireFrame(const RoiConfig& roi, CapturedFrame& outFrame, int timeoutMs);

    const std::wstring& LastError() const { return m_lastError; }

private:
    bool CreateStagingTexture(int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    int m_stagingWidth = 0;
    int m_stagingHeight = 0;

    RECT m_monitorBoundsScreen{0, 0, 0, 0};
    std::wstring m_lastError;
    uint64_t m_frameCounter = 0;
};

} // namespace fb
