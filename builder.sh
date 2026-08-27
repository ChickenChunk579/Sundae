#!/usr/bin/env bash

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
    echo "please install cmake.
    exit 1
fi


NINJA=ninja

# test for ninja
if command -v "$NINJA" >/dev/null 2>&1; then
    echo found ninja
else
    echo "please install ninja.
    exit 1
fi

CMAKE_FLAGS="-B build/$1 -S . -GNinja "
if [[ $1 == "mac" ]]; then
    CMAKE_FLAGS+="-DPLATFORM=cli -DBACKEND=appkit" 
else
    echo unknown platform: $1
    exit 1
fi

echo invoking cmake
$CMAKE $CMAKE_FLAGS

if [[ $2 == "build" || $2 == "run" ]]; then
    $CMAKE --build build/$1
    if [[ $2 == "run" ]]; then
        ./build/$1/sundae ${@:3}
    else
        echo done
    fi
else
    echo done
fi