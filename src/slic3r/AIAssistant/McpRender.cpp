#include "McpRender.hpp"
#include "McpToolUtils.hpp"
#include "ScreenCapture.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"


#include <wx/image.h>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace Slic3r { namespace AIAssistant {

using nlohmann::json;
using GUI::ThumbnailView;

namespace {

const std::pair<const char*, ThumbnailView> VIEWS[] = {
    { "iso", ThumbnailView::Iso },     { "top_front", ThumbnailView::TopFront }, { "left", ThumbnailView::Left },
    { "right", ThumbnailView::Right }, { "top", ThumbnailView::Top },            { "bottom", ThumbnailView::Bottom },
    { "front", ThumbnailView::Front }, { "rear", ThumbnailView::Rear },
};

ThumbnailView parse_view(const std::string& name)
{
    for (const auto& [view_name, view] : VIEWS)
        if (name == view_name)
            return view;
    throw ToolError("unknown view '" + name + "'");
}

const char* const PREVIEW_VIEWS[] = { "iso", "top", "bottom", "front", "rear", "left", "right" };

// Captures waiting for their frame, by request number.
std::map<int, std::optional<wxImage>>& captures()
{
    static std::map<int, std::optional<wxImage>> c;
    return c;
}

// Layer index (0-based in the slider) of op[name], a 1-based layer.
int slider_layer(const json& args, const char* name, int fallback, int max)
{
    if (!args.contains(name))
        return fallback;
    if (!args[name].is_number_integer() || args[name].get<int>() < 1 || args[name].get<int>() > max + 1)
        throw ToolError(std::string("'") + name + "' must be a layer between 1 and " + std::to_string(max + 1));
    return args[name].get<int>() - 1;
}

} // namespace

json render_view_content(const json& args)
{
    GUI::Plater&       plater = require_plater();
    GUI::GLCanvas3D*   canvas = plater.get_view3D_canvas3D();
    if (canvas == nullptr)
        throw ToolError("The 3D view is not available");

    const std::string view_name = args.value("view", std::string("iso"));
    const ThumbnailView view    = parse_view(view_name);
    const int           size    = std::clamp(args.value("size", 768), 256, 1024);
    const int           plate   = plater.get_partplate_list().get_curr_plate_index();

    // Physical filament colors first, then mixed filaments: indexed by filament ID - 1.
    const std::vector<std::string> hex_colors = plater.get_extruder_colors_from_plater_config();
    std::vector<ColorRGBA>         colors;
    for (const std::string& hex : hex_colors) {
        ColorRGBA color = ColorRGBA::GRAY();
        decode_color(hex, color);
        colors.push_back(color);
    }

    // printable_only = false, parts_only = true, show_bed = true, transparent_background = false
    const ThumbnailsParams params = { {}, false, true, true, false, plate };
    ThumbnailData          thumbnail;
    canvas->render_thumbnail(thumbnail, unsigned(size), unsigned(size), params, canvas->get_volumes(), colors,
                             GUI::Camera::EType::Perspective, view, false, false);
    if (!thumbnail.is_valid())
        throw ToolError("The view could not be rendered (OpenGL offscreen rendering unavailable?)");

    const auto png = GCodeThumbnails::compress_thumbnail(thumbnail, GCodeThumbnailsFormat::PNG);
    if (!png || png->data == nullptr || png->size == 0)
        throw ToolError("The view could not be encoded");

    std::string legend = "View '" + view_name + "' of plate " + std::to_string(plate + 1) +
                         ", objects only (supports and toolpaths are not shown). Colors by filament ID:";
    for (size_t i = 0; i < hex_colors.size(); ++i)
        legend += " " + std::to_string(i + 1) + "=" + hex_colors[i];

    return json::array({ { { "type", "image" }, { "data", base64(png->data, png->size) }, { "mimeType", "image/png" } },
                         { { "type", "text" }, { "text", legend } } });
}

json preview_content(const json& args, json& state)
{
    GUI::Plater& plater = require_plater();
    GUI::PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (!plate->is_slice_result_valid())
        throw ToolError("The current plate is not sliced: call slice first");
    const std::string view = args.value("view", std::string("iso"));
    if (std::find_if(std::begin(PREVIEW_VIEWS), std::end(PREVIEW_VIEWS), [&view](const char* v) { return view == v; }) ==
        std::end(PREVIEW_VIEWS))
        throw ToolError("'view' must be iso, top, bottom, front, rear, left or right for the preview");
    const int size = std::clamp(args.value("size", 1024), 256, 1600);

    if (!state.contains("phase")) {
        // As the Preview tab, then the camera buttons.
        plater.select_view_3D("Preview");
        plater.select_view(view);
        state = { { "phase", "loading" }, { "polls", 0 } };
        throw ToolRetry();
    }

    GUI::GLCanvas3D* canvas = plater.get_preview_canvas3D();
    GUI::IMSlider*   slider = canvas != nullptr ? canvas->get_gcode_viewer().get_layers_slider() : nullptr;
    const std::string phase = state["phase"].get<std::string>();
    if (phase == "loading") {
        state["polls"] = state["polls"].get<int>() + 1;
        if (!plater.is_preview_shown() || !plater.is_preview_loaded() || slider == nullptr || slider->GetMaxValue() <= 0) {
            if (state["polls"].get<int>() > 40)
                throw ToolError("The preview did not load");
            throw ToolRetry();
        }
        // As dragging the two thumbs of the layer slider.
        const int max   = slider->GetMaxValue();
        const int lower = slider_layer(args, "from_layer", 0, max);
        const int upper = slider_layer(args, "to_layer", max, max);
        if (lower > upper)
            throw ToolError("'from_layer' must not be above 'to_layer'");
        slider->SetSelectionSpan(lower, upper);
        slider->set_as_dirty();
        canvas->set_as_dirty();
        canvas->request_extra_frame();
        state["phase"] = "settling";
        throw ToolRetry();
    }
    if (phase == "settling") {
        // One more frame applies the layer range before the capture.
        static int last_request = 0;
        const int  request      = ++last_request;
        captures()[request];
        ScreenCapture::capture_view([request](const wxImage& image) { captures()[request] = image; });
        state["phase"]   = "capturing";
        state["request"] = request;
        throw ToolRetry();
    }

    const int  request = state["request"].get<int>();
    const auto it      = captures().find(request);
    if (it == captures().end() || !it->second)
        throw ToolRetry();
    const wxImage image = *it->second;
    captures().erase(it);
    if (!image.IsOk())
        throw ToolError("The preview could not be captured (the 3D view must be visible on the screen)");

    const int lower = slider->GetLowerValue() + 1;
    const int upper = slider->GetHigherValue() + 1;
    std::string legend = "Sliced preview of plate " + std::to_string(plate->get_index() + 1) + ", view '" + view + "', layers " +
                         std::to_string(lower) + " to " + std::to_string(upper) + " of " + std::to_string(slider->GetMaxValue() + 1) +
                         " (" + float_to_string_decimal_point(slider->GetLowerValueD(), 2) + " - " +
                         float_to_string_decimal_point(slider->GetHigherValueD(), 2) +
                         " mm), as the preview of the slicer shows it: toolpaths colored by the legend at the left, supports, "
                         "layer slider at the right.";
    return json::array({ { { "type", "image" }, { "data", ScreenCapture::to_png_base64(image, size) }, { "mimeType", "image/png" } },
                         { { "type", "text" }, { "text", legend } } });
}

}} // namespace Slic3r::AIAssistant
