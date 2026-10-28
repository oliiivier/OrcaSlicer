#pragma once

namespace Slic3r { namespace AIAssistant {

// At startup, connects the slicer to the printer when exactly one printer of the
// account is online and none is connected yet, like selecting it in the Device
// tab. Tries for a short time after launch only, once: a printer disconnected by
// the user later is left alone. Enabled by McpSettings::auto_connect_printer.
// GUI thread only.
void start_printer_auto_connect();

}} // namespace Slic3r::AIAssistant
