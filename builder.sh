#!/usr/bin/env bash
set -e

if [ "$#" -lt 2 ]; then
    echo "please pass a target, an action, and any optional arguments"
    echo "usage: $0 {mac} {conf/build} [extra_args...]"
    exit 1
fi


CMAKE=cmake

# test for cmake
if command -v "$CMAKE" >/dev/null 2>&1; then
    echo found cmake
else
    echo "please install cmake."
    exit 1
fi


NINJA=ninja

# test for ninja
if command -v "$NINJA" >/dev/null 2>&1; then
    echo found ninja
else
    echo "please install ninja."
    exit 1
fi

CMAKE_FLAGS="-B build/$1 -S . -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=1 "
if [[ $1 == "mac-appkit" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=appkit -DVIDEO_BACKEND=ffmpeg" 
elif [[ $1 == "mac-glfw3" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=glfw3 -DVIDEO_BACKEND=ffmpeg" 
elif [[ $1 == "mac-sdl1" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl1 -DVIDEO_BACKEND=ffmpeg" 
elif [[ $1 == "mac-sdl2" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl2 -DVIDEO_BACKEND=ffmpeg" 
elif [[ $1 == "mac-sdl3" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl3 -DVIDEO_BACKEND=ffmpeg" 
elif [[ $1 == "linux-sdl1" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl1 -DVIDEO_BACKEND=ffmpeg"
elif [[ $1 == "linux-sdl2" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl2 -DVIDEO_BACKEND=ffmpeg"
elif [[ $1 == "linux-sdl3" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=sdl3 -DVIDEO_BACKEND=ffmpeg"
else
    echo "unknown platform: $1"
    exit 1
fi

echo invoking cmake
$CMAKE $CMAKE_FLAGS

if [[ $2 == "clean" ]]; then
    $CMAKE --build "build/$1" --target clean
    echo "done cleaning"
elif [[ $2 == "build" || $2 == "run" ]]; then
    $CMAKE --build "build/$1"
    if [[ $2 == "run" ]]; then
        "./build/$1/sundae" "${@:3}"
    else
        echo "done building"
    fi
elif [[ $2 == "conf" ]]; then
    echo "done configuring"
else
    echo "unknown action: $2"
    exit 1
fi

