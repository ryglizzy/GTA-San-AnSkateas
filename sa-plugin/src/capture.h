// Photographs for the automated test (-sktest): copies the centre of the
// game's frame into one cell of a contact sheet, several views side by side,
// and saves the sheet as a PNG with a caption on each cell.
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
namespace Gdiplus {
using std::max; // gdiplus.h uses min and max, which NOMINMAX removes
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include <cstdint>
#include <string>
#include <vector>

class ContactSheet {
public:
    void Begin(int columns, int rows, int cellWidth, int cellHeight) {
        columns_ = columns;
        rows_ = rows;
        cellW_ = cellWidth;
        cellH_ = cellHeight;
        stride_ = (columns_ * cellW_ * 3 + 3) & ~3;
        pixels_.assign(static_cast<size_t>(stride_) * rows_ * cellH_, 0x20);
        captions_.assign(static_cast<size_t>(columns_) * rows_, L"");
    }

    // Copies the middle of the current render target (full height, the
    // cell's aspect) into cell `index`, scaled down.
    bool Grab(IDirect3DDevice9* device, int index, const std::wstring& caption, std::string& error) {
        if (index < 0 || index >= columns_ * rows_) return Fail(error, "cell out of range");
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target) return Fail(error, "GetRenderTarget failed");
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        IDirect3DSurface9* source = target;
        IDirect3DSurface9* resolved = nullptr;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                  &resolved, nullptr)) ||
                FAILED(device->StretchRect(target, nullptr, resolved, nullptr, D3DTEXF_NONE))) {
                Release(resolved);
                Release(target);
                return Fail(error, "could not resolve the multisampled frame");
            }
            source = resolved;
        }
        if (!system_ || systemW_ != desc.Width || systemH_ != desc.Height || systemFormat_ != desc.Format) {
            Release(system_);
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                           &system_, nullptr))) {
                system_ = nullptr;
            }
            systemW_ = desc.Width;
            systemH_ = desc.Height;
            systemFormat_ = desc.Format;
        }
        bool copied = system_ && SUCCEEDED(device->GetRenderTargetData(source, system_));
        Release(resolved);
        Release(target);
        if (!copied) return Fail(error, "GetRenderTargetData failed (format " + std::to_string(desc.Format) + ")");
        D3DLOCKED_RECT locked{};
        if (FAILED(system_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return Fail(error, "LockRect failed");
        bool ok = Copy(static_cast<const uint8_t*>(locked.pBits), locked.Pitch, desc, index, error);
        system_->UnlockRect();
        if (ok) captions_[index] = caption;
        return ok;
    }

    bool Save(const std::wstring& path, std::string& error) {
        if (!StartGdiplus()) return Fail(error, "GDI+ did not start");
        int width = columns_ * cellW_, height = rows_ * cellH_;
        Gdiplus::Bitmap bitmap(width, height, stride_, PixelFormat24bppRGB, pixels_.data());
        {
            Gdiplus::Graphics g(&bitmap);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Gdiplus::Font font(L"Segoe UI", static_cast<Gdiplus::REAL>(std::max(10, cellH_ / 28)), Gdiplus::FontStyleBold,
                               Gdiplus::UnitPixel);
            Gdiplus::SolidBrush shadow(Gdiplus::Color(255, 0, 0, 0)), text(Gdiplus::Color(255, 255, 255, 120));
            Gdiplus::Pen line(Gdiplus::Color(255, 0, 0, 0), 2.f);
            for (int i = 0; i < columns_ * rows_; i++) {
                auto x = static_cast<Gdiplus::REAL>((i % columns_) * cellW_), y = static_cast<Gdiplus::REAL>((i / columns_) * cellH_);
                g.DrawRectangle(&line, x, y, static_cast<Gdiplus::REAL>(cellW_), static_cast<Gdiplus::REAL>(cellH_));
                if (captions_[i].empty()) continue;
                g.DrawString(captions_[i].c_str(), -1, &font, Gdiplus::PointF(x + 5.f, y + 4.f), &shadow);
                g.DrawString(captions_[i].c_str(), -1, &font, Gdiplus::PointF(x + 4.f, y + 3.f), &text);
            }
        }
        // PNG encoder {557CF406-1A04-11D3-9A73-0000F81EF32E}
        const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        if (bitmap.Save(path.c_str(), &png, nullptr) != Gdiplus::Ok) return Fail(error, "could not write the PNG");
        return true;
    }

    ~ContactSheet() { Release(system_); }

private:
    static bool Fail(std::string& error, const std::string& why) {
        error = why;
        return false;
    }

    template <class T>
    static void Release(T*& p) {
        if (p) p->Release();
        p = nullptr;
    }

    static bool StartGdiplus() {
        static ULONG_PTR token = 0;
        static bool started = false;
        if (!started) {
            Gdiplus::GdiplusStartupInput input;
            started = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
        }
        return started;
    }

    // One source pixel as 8-bit blue, green, red.
    static bool Pixel(const uint8_t* row, int x, D3DFORMAT format, uint8_t out[3]) {
        switch (format) {
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8R8G8B8:
            out[0] = row[x * 4 + 0];
            out[1] = row[x * 4 + 1];
            out[2] = row[x * 4 + 2];
            return true;
        case D3DFMT_A2R10G10B10: {
            uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
            out[0] = static_cast<uint8_t>((v & 0x3ff) >> 2);
            out[1] = static_cast<uint8_t>(((v >> 10) & 0x3ff) >> 2);
            out[2] = static_cast<uint8_t>(((v >> 20) & 0x3ff) >> 2);
            return true;
        }
        default:
            return false;
        }
    }

    bool Copy(const uint8_t* bits, int pitch, const D3DSURFACE_DESC& desc, int index, std::string& error) {
        uint8_t probe[3];
        if (!Pixel(bits, 0, desc.Format, probe)) return Fail(error, "unsupported frame format " + std::to_string(desc.Format));
        // The largest centred region with the cell's shape.
        int srcH = static_cast<int>(desc.Height);
        int srcW = srcH * cellW_ / cellH_;
        if (srcW > static_cast<int>(desc.Width)) {
            srcW = static_cast<int>(desc.Width);
            srcH = srcW * cellH_ / cellW_;
        }
        int left = (static_cast<int>(desc.Width) - srcW) / 2, top = (static_cast<int>(desc.Height) - srcH) / 2;
        int cellX = (index % columns_) * cellW_, cellY = (index / columns_) * cellH_;
        for (int y = 0; y < cellH_; y++) {
            int y0 = top + y * srcH / cellH_, y1 = std::max(y0 + 1, top + (y + 1) * srcH / cellH_);
            uint8_t* dst = pixels_.data() + static_cast<size_t>(cellY + y) * stride_ + static_cast<size_t>(cellX) * 3;
            for (int x = 0; x < cellW_; x++) {
                int x0 = left + x * srcW / cellW_, x1 = std::max(x0 + 1, left + (x + 1) * srcW / cellW_);
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const uint8_t* row = bits + static_cast<size_t>(sy) * pitch;
                    for (int sx = x0; sx < x1; sx++) {
                        uint8_t p[3];
                        Pixel(row, sx, desc.Format, p);
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                        n++;
                    }
                }
                for (int c = 0; c < 3; c++) dst[x * 3 + c] = static_cast<uint8_t>(sum[c] / n);
            }
        }
        return true;
    }

    int columns_ = 0, rows_ = 0, cellW_ = 0, cellH_ = 0, stride_ = 0;
    std::vector<uint8_t> pixels_; // 24-bit BGR rows, as GDI+ expects
    std::vector<std::wstring> captions_;
    IDirect3DSurface9* system_ = nullptr;
    UINT systemW_ = 0, systemH_ = 0;
    D3DFORMAT systemFormat_ = D3DFMT_UNKNOWN;
};
