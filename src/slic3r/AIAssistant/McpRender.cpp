#include "McpRender.hpp"
#include "McpToolUtils.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/beast/core/detail/base64.hpp>

#include <algorithm>
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

std::string base64(const void* data, size_t size)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(size), '\0');
    out.resize(b64::encode(out.data(), data, size));
    return out;
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

}} // namespace Slic3r::AIAssistant
