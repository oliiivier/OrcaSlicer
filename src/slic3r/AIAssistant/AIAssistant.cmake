# Fork-only module: AI assistant.
# Included from src/slic3r/CMakeLists.txt with a single OPTIONAL include so that
# upstream changes to that file rarely conflict with this fork.

option(SNAPMAKER_AI_ASSISTANT "Build the AI assistant MCP server (fork feature)" ON)

if (SNAPMAKER_AI_ASSISTANT)
    set(_ai_dir ${CMAKE_CURRENT_LIST_DIR})
    target_sources(libslic3r_gui PRIVATE
        ${_ai_dir}/AIAssistant.cpp
        ${_ai_dir}/AIAssistant.hpp
        ${_ai_dir}/McpChanges.cpp
        ${_ai_dir}/McpChanges.hpp
        ${_ai_dir}/McpDialog.cpp
        ${_ai_dir}/McpDialog.hpp
        ${_ai_dir}/McpDocs.cpp
        ${_ai_dir}/McpDocs.hpp
        ${_ai_dir}/McpExperiences.cpp
        ${_ai_dir}/McpExperiences.hpp
        ${_ai_dir}/McpIssues.cpp
        ${_ai_dir}/McpIssues.hpp
        ${_ai_dir}/McpMaterials.cpp
        ${_ai_dir}/McpMaterials.hpp
        ${_ai_dir}/McpPrinter.cpp
        ${_ai_dir}/McpPrinter.hpp
        ${_ai_dir}/McpPrinterControl.cpp
        ${_ai_dir}/McpPrinterControl.hpp
        ${_ai_dir}/McpPrinterLive.cpp
        ${_ai_dir}/McpPrinterLive.hpp
        ${_ai_dir}/McpRender.cpp
        ${_ai_dir}/McpRender.hpp
        ${_ai_dir}/McpServer.cpp
        ${_ai_dir}/McpServer.hpp
        ${_ai_dir}/McpService.cpp
        ${_ai_dir}/McpService.hpp
        ${_ai_dir}/McpSettings.cpp
        ${_ai_dir}/McpSettings.hpp
        ${_ai_dir}/McpSlicing.cpp
        ${_ai_dir}/McpSlicing.hpp
        ${_ai_dir}/McpTools.cpp
        ${_ai_dir}/McpTools.hpp
        ${_ai_dir}/McpToolUtils.cpp
        ${_ai_dir}/McpToolUtils.hpp
        ${_ai_dir}/PrinterAutoConnect.cpp
        ${_ai_dir}/PrinterAutoConnect.hpp
        ${_ai_dir}/ProjectAdapt.cpp
        ${_ai_dir}/ProjectAdapt.hpp
    )
    target_compile_definitions(libslic3r_gui PRIVATE SNAPMAKER_AI_ASSISTANT)
    message(STATUS "AI assistant: enabled")
endif ()
