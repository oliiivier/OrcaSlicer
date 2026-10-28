#pragma once

#include <functional>
#include <string>

class wxImage;

namespace Slic3r {
namespace GUI { class GLCanvas3D; }

namespace AIAssistant { namespace ScreenCapture {

// Called with the captured image, or an invalid image when the capture failed.
using Callback = std::function<void(const wxImage& image)>;

// The 3D view (prepare or preview) as currently drawn, with its legend and sliders.
// The image is read back from the next frame the canvas renders.
void capture_view(Callback callback);

// The whole main window. `before` runs first (e.g. to hide the chat window), `after`
// once the capture is done, whatever the result.
void capture_main_window(Callback callback, std::function<void()> before, std::function<void()> after);

// Called by GLCanvas3D::render() before it swaps the buffers.
void on_canvas_frame(GUI::GLCanvas3D& canvas);

// PNG in base64, scaled down so that its longest side is at most `max_edge` pixels.
std::string to_png_base64(const wxImage& image, int max_edge);

}} // namespace AIAssistant::ScreenCapture
} // namespace Slic3r
