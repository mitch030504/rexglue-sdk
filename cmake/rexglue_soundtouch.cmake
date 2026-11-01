# Replaceable shared DSP dependency; the guest runtime never uses its host-paced state.
include(FetchContent)
FetchContent_Declare(rex_soundtouch_source
    URL https://codeberg.org/soundtouch/soundtouch/archive/2.4.1.tar.gz
    URL_HASH SHA256=35d404e6e8c2ebd12fb4000da6fadd75c99e37eed2126a04721828c11c0377ec
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/soundtouch-download"
    DOWNLOAD_NAME soundtouch_source.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _rexglue_unused
)
FetchContent_MakeAvailable(rex_soundtouch_source)
file(GLOB soundtouch_sources CONFIGURE_DEPENDS
    "${rex_soundtouch_source_SOURCE_DIR}/source/SoundTouch/*.cpp")
add_library(rex_soundtouch SHARED ${soundtouch_sources})
set_target_properties(rex_soundtouch PROPERTIES
    OUTPUT_NAME SoundTouch WINDOWS_EXPORT_ALL_SYMBOLS ON EXPORT_NAME soundtouch)
target_compile_definitions(rex_soundtouch PUBLIC SOUNDTOUCH_FLOAT_SAMPLES)
target_include_directories(rex_soundtouch PUBLIC
    $<BUILD_INTERFACE:${rex_soundtouch_source_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/soundtouch>)
configure_file("${rex_soundtouch_source_SOURCE_DIR}/COPYING.TXT"
    "${CMAKE_BINARY_DIR}/soundtouch_license.txt" COPYONLY)
configure_file("${CMAKE_BINARY_DIR}/_deps/soundtouch-download/soundtouch_source.tar.gz"
    "${CMAKE_BINARY_DIR}/soundtouch_source.tar.gz" COPYONLY)
install(DIRECTORY "${rex_soundtouch_source_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/soundtouch" FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_BINARY_DIR}/soundtouch_license.txt"
    "${CMAKE_BINARY_DIR}/soundtouch_source.tar.gz"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/rexglue/licenses")
