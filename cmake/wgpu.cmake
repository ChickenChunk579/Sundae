include(ExternalProject)

set(WGPU_NATIVE_GIT_REPOSITORY
    "https://github.com/gfx-rs/wgpu-native.git"
    CACHE STRING "wgpu-native git repository")

set(WGPU_NATIVE_GIT_TAG
    "v27.0.4.1"
    CACHE STRING "wgpu-native git tag/commit")

set(WGPU_HEADERS_GIT_REPOSITORY
    "https://github.com/webgpu-native/webgpu-headers.git"
    CACHE STRING "WebGPU headers git repository")

set(WGPU_HEADERS_GIT_TAG
    "main"
    CACHE STRING "WebGPU headers git tag/commit")

set(WGPU_NATIVE_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/_deps/wgpu-native-src")

set(WGPU_HEADERS_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/_deps/webgpu-headers-src")

# ---------------------------------------------------------------------------
# Cargo
# ---------------------------------------------------------------------------

find_program(WGPU_CARGO_EXECUTABLE
    NAMES cargo
    HINTS
        "$ENV{HOME}/.cargo/bin"
        "$ENV{USERPROFILE}/.cargo/bin"
)

if(NOT WGPU_CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "wgpu-native requires Cargo/Rust, but cargo was not found.")
endif()

# ---------------------------------------------------------------------------
# Rust target
# ---------------------------------------------------------------------------

set(WGPU_CARGO_TARGET_ARGS)

if(CMAKE_CROSSCOMPILING)

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(WGPU_RUST_TARGET "aarch64-apple-darwin")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            set(WGPU_RUST_TARGET "x86_64-apple-darwin")
        endif()

    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(WGPU_RUST_TARGET "aarch64-unknown-linux-gnu")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            set(WGPU_RUST_TARGET "x86_64-unknown-linux-gnu")
        endif()

    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            set(WGPU_RUST_TARGET "aarch64-pc-windows-msvc")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            set(WGPU_RUST_TARGET "x86_64-pc-windows-msvc")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86)$")
            set(WGPU_RUST_TARGET "i686-pc-windows-msvc")
        endif()

    endif()

    if(NOT WGPU_RUST_TARGET)
        message(FATAL_ERROR
            "Unable to determine Rust target for "
            "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(WGPU_CARGO_TARGET_ARGS
        --target "${WGPU_RUST_TARGET}"
    )

endif()

# ---------------------------------------------------------------------------
# Cargo output
# ---------------------------------------------------------------------------

if(WGPU_RUST_TARGET)
    set(WGPU_CARGO_TARGET_DIR
        "${WGPU_NATIVE_SOURCE_DIR}/target/${WGPU_RUST_TARGET}")
else()
    set(WGPU_CARGO_TARGET_DIR
        "${WGPU_NATIVE_SOURCE_DIR}/target")
endif()

if(WIN32)
    set(WGPU_NATIVE_LIBRARY
        "${WGPU_CARGO_TARGET_DIR}/release/wgpu_native.lib")
else()
    set(WGPU_NATIVE_LIBRARY
        "${WGPU_CARGO_TARGET_DIR}/release/libwgpu_native.a")
endif()

# ---------------------------------------------------------------------------
# WebGPU headers
# ---------------------------------------------------------------------------

ExternalProject_Add(_webgpu_headers
    GIT_REPOSITORY
        "${WGPU_HEADERS_GIT_REPOSITORY}"

    GIT_TAG
        "${WGPU_HEADERS_GIT_TAG}"

    GIT_SHALLOW
        TRUE

    SOURCE_DIR
        "${WGPU_HEADERS_SOURCE_DIR}"

    CONFIGURE_COMMAND ""

    BUILD_COMMAND ""

    INSTALL_COMMAND ""

    UPDATE_DISCONNECTED
        TRUE

    USES_TERMINAL_DOWNLOAD
        TRUE
)

# ---------------------------------------------------------------------------
# wgpu-native
# ---------------------------------------------------------------------------

ExternalProject_Add(_wgpu_native_build
    GIT_REPOSITORY
        "${WGPU_NATIVE_GIT_REPOSITORY}"

    GIT_TAG
        "${WGPU_NATIVE_GIT_TAG}"

    GIT_SHALLOW
        TRUE

    SOURCE_DIR
        "${WGPU_NATIVE_SOURCE_DIR}"

    CONFIGURE_COMMAND ""

    BUILD_COMMAND
        "${WGPU_CARGO_EXECUTABLE}"
        build
        --release
        ${WGPU_CARGO_TARGET_ARGS}

    BUILD_IN_SOURCE
        TRUE

    INSTALL_COMMAND ""

    BUILD_BYPRODUCTS
        "${WGPU_NATIVE_LIBRARY}"

    UPDATE_DISCONNECTED
        TRUE

    USES_TERMINAL_DOWNLOAD
        TRUE

    USES_TERMINAL_BUILD
        TRUE
)

# ---------------------------------------------------------------------------
# Imported library target
# ---------------------------------------------------------------------------

if(NOT TARGET wgpu::wgpu)

    add_library(wgpu STATIC IMPORTED GLOBAL)

    set_target_properties(wgpu PROPERTIES
        IMPORTED_LOCATION
            "${WGPU_NATIVE_LIBRARY}"

        INTERFACE_INCLUDE_DIRECTORIES
            "${WGPU_HEADERS_SOURCE_DIR}"
    )

    add_dependencies(wgpu
        _wgpu_native_build
        _webgpu_headers
    )

    add_library(wgpu::wgpu ALIAS wgpu)

endif()

# ---------------------------------------------------------------------------
# Platform libraries
# ---------------------------------------------------------------------------

if(APPLE)

    target_link_libraries(wgpu INTERFACE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework IOKit"
    )

elseif(WIN32)

    target_link_libraries(wgpu INTERFACE
        d3d12
        dxgi
        userenv
        bcrypt
        ws2_32
        opengl32
    )

elseif(UNIX)

    find_package(Threads REQUIRED)

    target_link_libraries(wgpu INTERFACE
        Threads::Threads
        dl
        m
    )

endif()

message(STATUS "wgpu-native: ${WGPU_NATIVE_GIT_TAG}")
message(STATUS "wgpu-native Cargo: ${WGPU_CARGO_EXECUTABLE}")
message(STATUS "wgpu-native library: ${WGPU_NATIVE_LIBRARY}")
message(STATUS "WebGPU headers: ${WGPU_HEADERS_SOURCE_DIR}")