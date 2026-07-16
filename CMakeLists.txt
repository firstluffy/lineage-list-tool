cmake_minimum_required(VERSION 3.15)
project(LineageListTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lineage_list_tool WIN32
  src/main.cpp
  src/app.rc
)
set_target_properties(lineage_list_tool PROPERTIES OUTPUT_NAME "변신리스트수정 툴")

target_compile_definitions(lineage_list_tool PRIVATE UNICODE _UNICODE)

if(MSVC)
  target_compile_options(lineage_list_tool PRIVATE /W3 /utf-8)
  target_compile_definitions(lineage_list_tool PRIVATE
    WINVER=0x0601
    _WIN32_WINNT=0x0601
  )
endif()

option(LINEAGE_LIST_STATIC_RUNTIME
  "Visual C++ 정적 런타임(/MT) — 배포 시 VC++ 재배포 패키지 없이 단일 exe"
  OFF)
if(MSVC AND LINEAGE_LIST_STATIC_RUNTIME)
  set_property(TARGET lineage_list_tool PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

target_link_libraries(lineage_list_tool PRIVATE comdlg32 comctl32 user32 gdi32 kernel32)
