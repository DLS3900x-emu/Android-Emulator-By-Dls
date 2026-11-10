// GDI+ helpers for anti-aliased shapes (rounded rectangles, circles). Call
// EnsureGdiplus() once from the main thread before any other thread draws.
#pragma once
#include <windows.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace Aetheron {

inline void EnsureGdiplus() {
    static ULONG_PTR token = 0;
    if (token) return;
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
}

inline Gdiplus::Color GdipColor(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline void FillRound(HDC dc, const RECT& r, int radius, COLORREF c, BYTE alpha = 255) {
    const float x = float(r.left), y = float(r.top), w = float(r.right - r.left), h = float(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(GdipColor(c, alpha));
    const float d = std::min(float(radius) * 2, std::min(w, h));
    if (d <= 0) {
        g.FillRectangle(&brush, x, y, w, h);
        return;
    }
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

inline void FillCircle(HDC dc, float cx, float cy, float radius, COLORREF c, BYTE alpha = 255) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(GdipColor(c, alpha));
    g.FillEllipse(&brush, cx - radius, cy - radius, radius * 2, radius * 2);
}

}  // namespace Aetheron
