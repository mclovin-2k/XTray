#include "svg_icons.h"
#include "resource.h"
#include "win32.h"

#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <cwchar>

namespace xtray {
namespace {

using Microsoft::WRL::ComPtr;

void CheckGraphics(HRESULT result, const wchar_t* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(ToUtf8(WindowsError(operation, static_cast<DWORD>(result))));
    }
}

struct BitmapOwner {
    HBITMAP value = nullptr;
    ~BitmapOwner() { if (value != nullptr) { DeleteObject(value); } }
};

} // namespace

struct SvgIconRenderer::Impl {
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory6> factory;
    ComPtr<ID2D1Device5> device;
    ComPtr<ID2D1DeviceContext5> context;
    std::array<ComPtr<ID2D1SvgDocument>, 3> documents;

    Impl()
    {
        // WARP keeps tiny, infrequent icon renders independent of the GPU driver.
        CheckGraphics(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            d3d.GetAddressOf(), nullptr, nullptr),
            Text(L"Cannot create the SVG graphics device", L"无法创建 SVG 图形设备"));
        CheckGraphics(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory6), nullptr, reinterpret_cast<void**>(factory.GetAddressOf())),
            Text(L"Cannot create the SVG drawing factory", L"无法创建 SVG 绘制工厂"));
        ComPtr<IDXGIDevice> dxgi;
        CheckGraphics(d3d.As(&dxgi), Text(L"Cannot access the SVG graphics device", L"无法访问 SVG 图形设备"));
        CheckGraphics(factory->CreateDevice(dxgi.Get(), device.GetAddressOf()),
            Text(L"Cannot create the SVG drawing device", L"无法创建 SVG 绘制设备"));
        CheckGraphics(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context.GetAddressOf()),
            Text(L"Cannot create the SVG drawing context", L"无法创建 SVG 绘制上下文"));
        context->SetDpi(96.0f, 96.0f);
    }

    ID2D1SvgDocument* Document(SvgIcon icon)
    {
        const auto index = static_cast<size_t>(icon);
        constexpr std::array<int, 3> resources{IDR_SVG_XTRAY, IDR_SVG_CLEAR_LOGS, IDR_SVG_SCROLL_TO_END};
        if (index >= resources.size()) {
            throw std::runtime_error("Unknown SVG icon.");
        }
        if (!documents[index]) {
            const HMODULE module = GetModuleHandleW(nullptr);
            const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resources[index]), RT_RCDATA);
            const HGLOBAL loaded = resource != nullptr ? LoadResource(module, resource) : nullptr;
            const auto* bytes = loaded != nullptr ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
            const DWORD size = resource != nullptr ? SizeofResource(module, resource) : 0;
            if (bytes == nullptr || size == 0) {
                throw std::runtime_error(Text("An embedded SVG icon is missing.", "缺少内嵌 SVG 图标资源。"));
            }
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(bytes, size));
            if (!stream) {
                throw std::runtime_error(Text("Cannot read an embedded SVG icon.", "无法读取内嵌 SVG 图标。"));
            }
            const float extent = icon == SvgIcon::Application ? 256.0f : 24.0f;
            CheckGraphics(context->CreateSvgDocument(stream.Get(), D2D1::SizeF(extent, extent),
                documents[index].GetAddressOf()),
                Text(L"Cannot parse an embedded SVG icon", L"无法解析内嵌 SVG 图标"));
        }
        return documents[index].Get();
    }
};

SvgIconRenderer::SvgIconRenderer() : impl_(std::make_unique<Impl>()) {}
SvgIconRenderer::~SvgIconRenderer() = default;

IconPixels SvgIconRenderer::Render(SvgIcon icon, int size, COLORREF color)
{
    if (size <= 0 || size > 1024) {
        throw std::runtime_error("Invalid SVG icon size.");
    }
    auto* document = impl_->Document(icon);
    ComPtr<ID2D1SvgElement> foreground;
    CheckGraphics(document->FindElementById(L"foreground", foreground.GetAddressOf()),
        Text(L"Cannot locate the SVG foreground", L"无法定位 SVG 前景"));
    if (!foreground) {
        throw std::runtime_error(Text("The SVG icon has no foreground layer.", "SVG 图标缺少前景层。"));
    }
    wchar_t paint[8]{};
    swprintf_s(paint, L"#%02X%02X%02X", static_cast<unsigned>(GetRValue(color)),
        static_cast<unsigned>(GetGValue(color)), static_cast<unsigned>(GetBValue(color)));
    CheckGraphics(foreground->SetAttributeValue(icon == SvgIcon::ScrollToEnd ? L"stroke" : L"fill",
        D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, paint),
        Text(L"Cannot tint an SVG icon", L"无法为 SVG 图标着色"));

    const D2D1_SIZE_U dimensions{static_cast<UINT32>(size), static_cast<UINT32>(size)};
    D2D1_BITMAP_PROPERTIES1 properties{};
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = properties.dpiY = 96.0f;
    properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    ComPtr<ID2D1Bitmap1> target;
    CheckGraphics(impl_->context->CreateBitmap(dimensions, nullptr, 0, &properties, target.GetAddressOf()),
        Text(L"Cannot allocate SVG pixels", L"无法分配 SVG 像素"));
    impl_->context->SetTarget(target.Get());
    const float extent = icon == SvgIcon::Application ? 256.0f : 24.0f;
    const float scale = static_cast<float>(size) / extent;
    impl_->context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    impl_->context->BeginDraw();
    impl_->context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    impl_->context->DrawSvgDocument(document);
    const HRESULT drawn = impl_->context->EndDraw();
    impl_->context->SetTarget(nullptr);
    CheckGraphics(drawn, Text(L"Cannot render the SVG icon", L"无法绘制 SVG 图标"));

    properties.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
    ComPtr<ID2D1Bitmap1> readable;
    CheckGraphics(impl_->context->CreateBitmap(dimensions, nullptr, 0, &properties, readable.GetAddressOf()),
        Text(L"Cannot allocate SVG readback pixels", L"无法分配 SVG 像素读取缓冲"));
    CheckGraphics(readable->CopyFromBitmap(nullptr, target.Get(), nullptr),
        Text(L"Cannot copy SVG pixels", L"无法复制 SVG 像素"));
    IconPixels result{size, std::vector<BYTE>(static_cast<size_t>(size) * static_cast<size_t>(size) * 4)};
    D2D1_MAPPED_RECT mapped{};
    CheckGraphics(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped),
        Text(L"Cannot read SVG pixels", L"无法读取 SVG 像素"));
    for (int row = 0; row < size; ++row) {
        std::memcpy(result.bgra.data() + static_cast<size_t>(row) * static_cast<size_t>(size) * 4,
            mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(size) * 4);
    }
    readable->Unmap();
    return result;
}

HICON SvgIconRenderer::CreateIcon(SvgIcon icon, int size, COLORREF color)
{
    const auto pixels = Render(icon, size, color);
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = size;
    information.bmiHeader.biHeight = -size;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    void* data = nullptr;
    BitmapOwner bitmap;
    bitmap.value = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &data, nullptr, 0);
    if (bitmap.value == nullptr || data == nullptr) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot create an SVG icon bitmap", L"无法创建 SVG 图标位图"))));
    }
    std::memcpy(data, pixels.bgra.data(), pixels.bgra.size());
    const size_t stride = static_cast<size_t>((size + 15) / 16 * 2);
    const std::vector<BYTE> maskBytes(stride * static_cast<size_t>(size), 0);
    BitmapOwner mask;
    mask.value = CreateBitmap(size, size, 1, 1, maskBytes.data());
    if (mask.value == nullptr) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot create an SVG icon mask", L"无法创建 SVG 图标掩码"))));
    }
    ICONINFO details{TRUE, 0, 0, mask.value, bitmap.value};
    const HICON result = CreateIconIndirect(&details);
    if (result == nullptr) {
        throw std::runtime_error(ToUtf8(WindowsError(
            Text(L"Cannot create an SVG icon", L"无法创建 SVG 图标"))));
    }
    return result;
}

} // namespace xtray
