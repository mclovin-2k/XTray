#include "svg_icons.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

using Microsoft::WRL::ComPtr;
using namespace xtray;

void Check(HRESULT result)
{
    if (FAILED(result)) {
        throw std::runtime_error("Windows PNG encoding failed: " + std::to_string(result));
    }
}

void U16(std::vector<BYTE>& bytes, unsigned value)
{
    bytes.push_back(static_cast<BYTE>(value));
    bytes.push_back(static_cast<BYTE>(value >> 8));
}

void U32(std::vector<BYTE>& bytes, unsigned value)
{
    U16(bytes, value);
    U16(bytes, value >> 16);
}

IconPixels StraightAlpha(IconPixels pixels)
{
    for (size_t position = 0; position < pixels.bgra.size(); position += 4) {
        const unsigned alpha = pixels.bgra[position + 3];
        for (size_t channel = 0; channel < 3; ++channel) {
            pixels.bgra[position + channel] = alpha == 0 ? 0 : static_cast<BYTE>((std::min)(255U,
                (static_cast<unsigned>(pixels.bgra[position + channel]) * 255U + alpha / 2U) / alpha));
        }
    }
    return pixels;
}

std::vector<BYTE> Bitmap(const IconPixels& pixels)
{
    const unsigned side = static_cast<unsigned>(pixels.size);
    const unsigned maskStride = (side + 31U) / 32U * 4U;
    std::vector<BYTE> bytes;
    U32(bytes, 40); U32(bytes, side); U32(bytes, side * 2U);
    U16(bytes, 1); U16(bytes, 32);
    U32(bytes, 0); U32(bytes, 0); // BI_RGB does not require biSizeImage.
    U32(bytes, 0); U32(bytes, 0); U32(bytes, 0); U32(bytes, 0);
    for (unsigned row = side; row > 0; --row) {
        const auto begin = pixels.bgra.begin() + static_cast<ptrdiff_t>((row - 1U) * side * 4U);
        bytes.insert(bytes.end(), begin, begin + static_cast<ptrdiff_t>(side * 4U));
    }
    for (unsigned row = side; row > 0; --row) {
        std::vector<BYTE> mask(maskStride, 0);
        for (unsigned column = 0; column < side; ++column) {
            if (pixels.bgra[((row - 1U) * side + column) * 4U + 3U] == 0) {
                mask[column / 8U] |= static_cast<BYTE>(0x80U >> (column % 8U));
            }
        }
        bytes.insert(bytes.end(), mask.begin(), mask.end());
    }
    return bytes;
}

std::vector<BYTE> Png(IconPixels& pixels)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr));
    Check(frame->Initialize(nullptr));
    Check(frame->SetSize(static_cast<UINT>(pixels.size), static_cast<UINT>(pixels.size)));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format));
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        throw std::runtime_error("The Windows PNG encoder does not accept BGRA.");
    }
    Check(frame->WritePixels(static_cast<UINT>(pixels.size), static_cast<UINT>(pixels.size * 4),
        static_cast<UINT>(pixels.bgra.size()), pixels.bgra.data()));
    Check(frame->Commit());
    Check(encoder->Commit());
    STATSTG status{};
    Check(stream->Stat(&status, STATFLAG_NONAME));
    if (status.cbSize.QuadPart > 16U * 1024U * 1024U) {
        throw std::runtime_error("Encoded icon is unexpectedly large.");
    }
    std::vector<BYTE> bytes(static_cast<size_t>(status.cbSize.QuadPart));
    LARGE_INTEGER start{};
    Check(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    ULONG read = 0;
    Check(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
    if (read != bytes.size()) {
        throw std::runtime_error("Cannot read the complete encoded icon.");
    }
    return bytes;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fputs("Usage: XTray.IconBuilder output.ico\n", stderr);
        return 1;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        return 1;
    }
    struct ComLifetime { ~ComLifetime() { CoUninitialize(); } } lifetime;
    try {
        SvgIconRenderer renderer;
        constexpr std::array<unsigned, 9> sizes{16, 20, 24, 32, 40, 48, 64, 128, 256};
        std::vector<std::vector<BYTE>> images;
        for (const auto side : sizes) {
            auto pixels = StraightAlpha(renderer.Render(SvgIcon::Application,
                static_cast<int>(side), kApplicationIconColor));
            images.push_back(side == 256 ? Png(pixels) : Bitmap(pixels));
        }
        std::vector<BYTE> bytes;
        U16(bytes, 0); U16(bytes, 1); U16(bytes, static_cast<unsigned>(sizes.size()));
        unsigned offset = 6U + static_cast<unsigned>(sizes.size()) * 16U;
        for (size_t index = 0; index < sizes.size(); ++index) {
            bytes.push_back(static_cast<BYTE>(sizes[index] == 256 ? 0 : sizes[index]));
            bytes.push_back(static_cast<BYTE>(sizes[index] == 256 ? 0 : sizes[index]));
            bytes.push_back(0); bytes.push_back(0);
            U16(bytes, 1); U16(bytes, 32);
            U32(bytes, static_cast<unsigned>(images[index].size()));
            U32(bytes, offset);
            offset += static_cast<unsigned>(images[index].size());
        }
        for (const auto& image : images) {
            bytes.insert(bytes.end(), image.begin(), image.end());
        }
        std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Cannot save the generated ICO.");
        }
        std::puts("Generated yellow XTray.ico from the embedded SVG at 9 sizes.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Icon generation failed: %s\n", error.what());
        return 1;
    }
}
