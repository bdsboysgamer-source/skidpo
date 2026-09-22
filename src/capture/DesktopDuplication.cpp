#include "DesktopDuplication.h"

#include <dxgi1_2.h>
#include <d3d11.h>
#include <algorithm>
#include <sstream>
#include <iterator>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace fb {

namespace {

std::wstring HResultToString(const wchar_t* what, HRESULT hr) {
    std::wstringstream ss;
    ss << what << L" (hr=0x" << std::hex << static_cast<unsigned long>(hr) << L")";
    return ss.str();
}

} // namespace

DesktopDuplication::~DesktopDuplication() {
    Shutdown();
}

void DesktopDuplication::Shutdown() {
    m_duplication.Reset();
    m_stagingTexture.Reset();
    m_context.Reset();
    m_device.Reset();
    m_stagingWidth = 0;
    m_stagingHeight = 0;
}

bool DesktopDuplication::Initialize(const RoiConfig& roi, std::wstring& outError) {
    Shutdown();

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL obtainedLevel{};

    POINT roiTopLeft{ roi.screenX, roi.screenY };

    // Enumerate adapters/outputs looking for the one whose desktop bounds
    // contain the ROI's top-left point. We must create the D3D11 device
    // against that output's adapter for DuplicateOutput to succeed.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        outError = HResultToString(L"CreateDXGIFactory1 failed", hr);
        m_lastError = outError;
        return false;
    }

    ComPtr<IDXGIAdapter1> chosenAdapter;
    ComPtr<IDXGIOutput> chosenOutput;
    DXGI_OUTPUT_DESC chosenDesc{};
    bool found = false;

    for (UINT adapterIndex = 0; !found; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT outputIndex = 0; ; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                if (PtInRect(&desc.DesktopCoordinates, roiTopLeft)) {
                    chosenAdapter = adapter;
                    chosenOutput = output;
                    chosenDesc = desc;
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        // Fall back to the primary output on the primary adapter so the
        // bot can still run (with a config warning) rather than refuse to
        // start entirely.
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
            ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    chosenAdapter = adapter;
                    chosenOutput = output;
                    chosenDesc = desc;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        outError = L"No DXGI output found to duplicate";
        m_lastError = outError;
        return false;
    }

    hr = D3D11CreateDevice(chosenAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
                            static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
                            &m_device, &obtainedLevel, &m_context);
    if (FAILED(hr)) {
        outError = HResultToString(L"D3D11CreateDevice failed", hr);
        m_lastError = outError;
        Shutdown();
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    hr = chosenOutput.As(&output1);
    if (FAILED(hr)) {
        outError = HResultToString(L"IDXGIOutput1 QI failed", hr);
        m_lastError = outError;
        Shutdown();
        return false;
    }

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) {
        outError = HResultToString(L"DuplicateOutput failed", hr);
        m_lastError = outError;
        Shutdown();
        return false;
    }

    m_monitorBoundsScreen = chosenDesc.DesktopCoordinates;
    m_frameCounter = 0;
    return true;
}

bool DesktopDuplication::CreateStagingTexture(int width, int height) {
    if (m_stagingTexture && width == m_stagingWidth && height == m_stagingHeight) return true;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    m_stagingTexture.Reset();
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    if (FAILED(hr)) {
        m_lastError = HResultToString(L"CreateTexture2D (staging) failed", hr);
        m_stagingWidth = m_stagingHeight = 0;
        return false;
    }
    m_stagingWidth = width;
    m_stagingHeight = height;
    return true;
}

CaptureStatus DesktopDuplication::AcquireFrame(const RoiConfig& roi, CapturedFrame& outFrame, int timeoutMs) {
    if (!m_duplication) return CaptureStatus::Failed;

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    HRESULT hr = m_duplication->AcquireNextFrame(static_cast<UINT>(timeoutMs), &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return CaptureStatus::NoNewFrame;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        m_lastError = L"DXGI_ERROR_ACCESS_LOST";
        return CaptureStatus::DeviceLost;
    }
    if (FAILED(hr)) {
        m_lastError = HResultToString(L"AcquireNextFrame failed", hr);
        return CaptureStatus::Failed;
    }

    // A frame with no new desktop image content (e.g. only the cursor
    // moved) has LastPresentTime == 0. Skip re-processing the same pixels.
    if (frameInfo.LastPresentTime.QuadPart == 0) {
        m_duplication->ReleaseFrame();
        return CaptureStatus::NoNewFrame;
    }

    ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr)) {
        m_lastError = HResultToString(L"desktopResource QI failed", hr);
        m_duplication->ReleaseFrame();
        return CaptureStatus::Failed;
    }

    int monitorWidth = m_monitorBoundsScreen.right - m_monitorBoundsScreen.left;
    int monitorHeight = m_monitorBoundsScreen.bottom - m_monitorBoundsScreen.top;

    int localLeft = roi.CaptureOriginScreenX() - m_monitorBoundsScreen.left;
    int localTop = roi.CaptureOriginScreenY() - m_monitorBoundsScreen.top;
    int width = roi.CaptureWidth();
    int height = roi.CaptureHeight();

    int clampedLeft = std::clamp(localLeft, 0, std::max(0, monitorWidth - 1));
    int clampedTop = std::clamp(localTop, 0, std::max(0, monitorHeight - 1));
    int clampedRight = std::clamp(localLeft + width, clampedLeft + 1, monitorWidth);
    int clampedBottom = std::clamp(localTop + height, clampedTop + 1, monitorHeight);
    int clampedWidth = clampedRight - clampedLeft;
    int clampedHeight = clampedBottom - clampedTop;

    if (clampedWidth <= 0 || clampedHeight <= 0) {
        m_lastError = L"ROI capture rect falls outside the selected monitor";
        m_duplication->ReleaseFrame();
        return CaptureStatus::Failed;
    }

    if (!CreateStagingTexture(clampedWidth, clampedHeight)) {
        m_duplication->ReleaseFrame();
        return CaptureStatus::Failed;
    }

    D3D11_BOX srcBox{};
    srcBox.left = static_cast<UINT>(clampedLeft);
    srcBox.top = static_cast<UINT>(clampedTop);
    srcBox.front = 0;
    srcBox.right = static_cast<UINT>(clampedRight);
    srcBox.bottom = static_cast<UINT>(clampedBottom);
    srcBox.back = 1;

    m_context->CopySubresourceRegion(m_stagingTexture.Get(), 0, 0, 0, 0, desktopTexture.Get(), 0, &srcBox);

    // The GPU copy is queued; we can release the duplication frame now -
    // the staging texture copy is independent of the duplicated resource.
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        m_lastError = HResultToString(L"Map staging texture failed", hr);
        return CaptureStatus::Failed;
    }

    outFrame.width = clampedWidth;
    outFrame.height = clampedHeight;
    outFrame.strideBytes = clampedWidth * 4;
    outFrame.pixelsBgra.resize(static_cast<size_t>(outFrame.strideBytes) * clampedHeight);

    const uint8_t* srcBase = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < clampedHeight; ++y) {
        const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dstRow = outFrame.pixelsBgra.data() + static_cast<size_t>(y) * outFrame.strideBytes;
        memcpy(dstRow, srcRow, static_cast<size_t>(outFrame.strideBytes));
    }
    m_context->Unmap(m_stagingTexture.Get(), 0);

    outFrame.frameId = ++m_frameCounter;
    outFrame.timestamp = Clock::now();
    outFrame.captureOriginScreenX = m_monitorBoundsScreen.left + clampedLeft;
    outFrame.captureOriginScreenY = m_monitorBoundsScreen.top + clampedTop;

    return CaptureStatus::Ok;
}

} // namespace fb
