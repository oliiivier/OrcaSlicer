# Fork-only module: AI assistant.
# Included from src/slic3r/CMakeLists.txt with a single OPTIONAL include so that
# upstream changes to that file rarely conflict with this fork.

option(SNAPMAKER_AI_ASSISTANT "Build the AI assistant (fork feature)" ON)

if (SNAPMAKER_AI_ASSISTANT)
    set(_ai_dir ${CMAKE_CURRENT_LIST_DIR})
    target_sources(libslic3r_gui PRIVATE
        ${_ai_dir}/AIAssistant.cpp
        ${_ai_dir}/AIAssistant.hpp
        ${_ai_dir}/AssistantDialog.cpp
        ${_ai_dir}/AssistantDialog.hpp
        ${_ai_dir}/AssistantSettings.cpp
        ${_ai_dir}/AssistantSettings.hpp
        ${_ai_dir}/LlmProvider.hpp
    )
    target_compile_definitions(libslic3r_gui PRIVATE SNAPMAKER_AI_ASSISTANT)
    message(STATUS "AI assistant: enabled")
endif ()
