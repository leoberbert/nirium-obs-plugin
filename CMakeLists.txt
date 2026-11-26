cmake_minimum_required(VERSION 3.20)

project(niriun-obs VERSION 0.1.4 LANGUAGES C CXX)

option(NIRIUN_BUILD_PLUGIN "Build the OBS Studio plugin (needs libobs and FFmpeg)" ON)
option(NIRIUN_BUILD_TESTS "Build the protocol unit tests" ON)
set(NIRIUN_DEPS_DIR "" CACHE PATH
    "Windows: folder created by scripts/fetch-windows-deps.ps1 (OBS headers, FFmpeg headers, import libraries)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NIRIUN_BUILD_TESTS)
  enable_testing()
  add_executable(niriun_protocol_test tests/protocol_test.cc)
  target_include_directories(niriun_protocol_test PRIVATE src)
  if(MSVC)
    target_compile_options(niriun_protocol_test PRIVATE /W4 /utf-8)
  else()
    target_compile_options(niriun_protocol_test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME protocol COMMAND niriun_protocol_test)
endif()

if(NOT NIRIUN_BUILD_PLUGIN)
  return()
endif()

# ---- dependencies ---------------------------------------------------------------------------------------

add_library(niriun-deps INTERFACE)

if(NIRIUN_DEPS_DIR)
  # Layout produced by scripts/fetch-windows-deps.ps1
  target_include_directories(niriun-deps INTERFACE
    "${NIRIUN_DEPS_DIR}/obs-studio/libobs"
    "${NIRIUN_DEPS_DIR}/obs-studio/frontend/api"
    "${NIRIUN_DEPS_DIR}/ffmpeg"
    "${NIRIUN_DEPS_DIR}/generated")
  foreach(lib obs obs-frontend-api avcodec avutil)
    find_library(NIRIUN_LIB_${lib} NAMES ${lib} PATHS "${NIRIUN_DEPS_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(niriun-deps INTERFACE "${NIRIUN_LIB_${lib}}")
  endforeach()
else()
  find_package(libobs CONFIG)
  if(TARGET OBS::libobs)
    find_package(obs-frontend-api CONFIG REQUIRED)
    target_link_libraries(niriun-deps INTERFACE OBS::libobs OBS::obs-frontend-api)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS REQUIRED libobs)
    pkg_check_modules(OBS_FRONTEND REQUIRED obs-frontend-api)
    target_include_directories(niriun-deps INTERFACE ${LIBOBS_INCLUDE_DIRS} ${OBS_FRONTEND_INCLUDE_DIRS})
    target_link_directories(niriun-deps INTERFACE ${LIBOBS_LIBRARY_DIRS} ${OBS_FRONTEND_LIBRARY_DIRS})
    target_link_libraries(niriun-deps INTERFACE ${LIBOBS_LIBRARIES} ${OBS_FRONTEND_LIBRARIES})
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil)
  target_include_directories(niriun-deps INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(niriun-deps INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(niriun-deps INTERFACE ${FFMPEG_LIBRARIES})
endif()

# ---- plugin ---------------------------------------------------------------------------------------------

set(NIRIUN_SOURCES
  src/adb.cc
  src/discovery.cc
  src/ffmpeg_decode.cc
  src/net.cc
  src/plugin.cc
  src/source.cc)

if(WIN32)
  list(APPEND NIRIUN_SOURCES src/sys/win/cmd.cc src/sys/win/ifaces.cc src/niriun.rc)
else()
  list(APPEND NIRIUN_SOURCES src/sys/unix/cmd.cc src/sys/unix/ifaces.cc)
endif()

add_library(niriun-obs MODULE ${NIRIUN_SOURCES})
target_include_directories(niriun-obs PRIVATE src)
target_link_libraries(niriun-obs PRIVATE niriun-deps)
target_compile_definitions(niriun-obs PRIVATE NIRIUN_VERSION_STR="${PROJECT_VERSION}")
set_target_properties(niriun-obs PROPERTIES PREFIX "")

if(WIN32)
  target_link_libraries(niriun-obs PRIVATE ws2_32 iphlpapi)
  target_compile_definitions(niriun-obs PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
  target_compile_options(niriun-obs PRIVATE /W3 /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(niriun-obs PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(APPLE)
    set_target_properties(niriun-obs PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "plugin" SUFFIX "")
    target_link_options(niriun-obs PRIVATE -undefined dynamic_lookup)
  endif()
endif()

# ---- install --------------------------------------------------------------------------------------------
# `cmake --install build --prefix <dir>` produces the standard OBS plugin zip layout:
#   Windows: <dir>/obs-plugins/64bit/niriun-obs.dll   <dir>/data/obs-plugins/niriun-obs/locale/*.ini
#   Linux:   <dir>/lib/obs-plugins/niriun-obs.so      <dir>/share/obs/obs-plugins/niriun-obs/locale/*.ini
if(WIN32)
  install(TARGETS niriun-obs RUNTIME DESTINATION obs-plugins/64bit LIBRARY DESTINATION obs-plugins/64bit)
  install(DIRECTORY data/ DESTINATION data/obs-plugins/niriun-obs)
elseif(APPLE)
  install(TARGETS niriun-obs LIBRARY DESTINATION lib/obs-plugins BUNDLE DESTINATION lib/obs-plugins)
  install(DIRECTORY data/ DESTINATION share/obs/obs-plugins/niriun-obs)
else()
  install(TARGETS niriun-obs LIBRARY DESTINATION lib/obs-plugins)
  install(DIRECTORY data/ DESTINATION share/obs/obs-plugins/niriun-obs)
endif()
