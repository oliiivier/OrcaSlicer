#include "ScreenCapture.hpp"
#include "McpToolUtils.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <GL/glew.h>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/dcscreen.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/timer.h>

#include <vector>

namespace Slic3r { namespace AIAssistant { namespace ScreenCapture {

using GUI::wxGetApp;

namespace {

// The canvas renders on demand: without a frame by then, the capture fails.
constexpr int FRAME_TIMEOUT_MS = 2000;
// Lets the window manager remove the hidden chat window from the screen.
constexpr int HIDE_DELAY_MS = 300;

struct Pending
{
    GUI::GLCanvas3D* canvas { nullptr };
    Callback         callback;
    unsigned         serial { 0 };
};

Pending& pending()
{
    static Pending p;
    return p;
}

void finish(const wxImage& image)
{
    Pending& p = pending();
    if (!p.callback)
        return;
    Callback callback = std::move(p.callback);
    p.callback        = nullptr;
    p.canvas          = nullptr;
    callback(image);
}

class OneShotTimer : public wxTimer
{
public:
    explicit OneShotTimer(std::function<void()> fn) : m_fn(std::move(fn)) {}
    void Notify() override
    {
        std::function<void()> fn = std::move(m_fn);
        delete this;
        fn();
    }

private:
    std::function<void()> m_fn;
};

void call_later(int ms, std::function<void()> fn) { (new OneShotTimer(std::move(fn)))->StartOnce(ms); }

GUI::GLCanvas3D* current_canvas()
{
    GUI::Plater* plater = wxGetApp().plater();
    GUI::GLCanvas3D* canvas = plater ? plater->get_current_canvas3D() : nullptr;
    if (canvas == nullptr || canvas->get_wxglcanvas() == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
        return nullptr;
    return canvas;
}

} // namespace

void capture_view(Callback callback)
{
    GUI::GLCanvas3D* canvas = current_canvas();
    if (canvas == nullptr) {
        callback(wxImage());
        return;
    }
    Pending& p = pending();
    finish(wxImage()); // a previous capture that never got its frame
    p.canvas             = canvas;
    p.callback           = std::move(callback);
    const unsigned serial = ++p.serial;

    canvas->set_as_dirty();
    canvas->get_wxglcanvas()->Refresh();
    call_later(FRAME_TIMEOUT_MS, [serial]() {
        if (pending().serial == serial)
            finish(wxImage());
    });
}

void on_canvas_frame(GUI::GLCanvas3D& canvas)
{
    Pending& p = pending();
    if (!p.callback || p.canvas != &canvas)
        return;

    const Size size = canvas.get_canvas_size();
    const int  w    = size.get_width();
    const int  h    = size.get_height();
    if (w <= 0 || h <= 0)
        return;

    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadBuffer(GL_BACK);
    ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // OpenGL rows go bottom-up.
    wxImage        image(w, h, false);
    unsigned char* dst = image.GetData();
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgba.data() + size_t(h - 1 - y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x, src += 4, dst += 3) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    }
    // Not from inside the rendering.
    wxGetApp().CallAfter([image]() { finish(image); });
}

void capture_main_window(Callback callback, std::function<void()> before, std::function<void()> after)
{
    before();
    call_later(HIDE_DELAY_MS, [callback, after]() {
        // The screen copy of an OpenGL window may be black or stale: the 3D view is read
        // back from OpenGL and drawn over it.
        capture_view([callback, after](const wxImage& view) {
            wxWindow* frame = wxGetApp().mainframe;
            if (frame == nullptr) {
                after();
                callback(wxImage());
                return;
            }
            const wxRect r = frame->GetScreenRect();
            wxBitmap     bitmap(r.GetSize());
            {
                wxScreenDC screen;
                wxMemoryDC mem(bitmap);
                mem.Blit(0, 0, r.width, r.height, &screen, r.x, r.y);
                if (view.IsOk()) {
                    if (GUI::GLCanvas3D* canvas = current_canvas()) {
                        const wxRect c      = canvas->get_wxglcanvas()->GetScreenRect();
                        wxImage      scaled = view;
                        if (scaled.GetWidth() != c.width || scaled.GetHeight() != c.height)
                            scaled.Rescale(c.width, c.height, wxIMAGE_QUALITY_HIGH);
                        mem.DrawBitmap(wxBitmap(scaled), c.x - r.x, c.y - r.y);
                    }
                }
            }
            after();
            callback(bitmap.ConvertToImage());
        });
    });
}

std::string to_png_base64(const wxImage& image, int max_edge)
{
    if (!image.IsOk())
        return {};
    if (wxImage::FindHandler(wxBITMAP_TYPE_PNG) == nullptr)
        wxImage::AddHandler(new wxPNGHandler);

    wxImage   scaled  = image;
    const int longest = std::max(scaled.GetWidth(), scaled.GetHeight());
    if (longest > max_edge)
        scaled.Rescale(scaled.GetWidth() * max_edge / longest, scaled.GetHeight() * max_edge / longest, wxIMAGE_QUALITY_HIGH);

    wxMemoryOutputStream out;
    if (!scaled.SaveFile(out, wxBITMAP_TYPE_PNG))
        return {};
    std::vector<char> png(out.GetSize());
    out.CopyTo(png.data(), png.size());
    return base64(png.data(), png.size());
}

}}} // namespace Slic3r::AIAssistant::ScreenCapture
