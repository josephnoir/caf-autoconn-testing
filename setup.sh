#!/bin/bash

ROOT_DIR=$(pwd)

cores=4
if [ "$(uname)" == "Darwin" ]; then
  cores=$(sysctl -n hw.ncpu)
elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
  cores=$(nproc --all)
fi

echo "Using $cores cores for compilation."

# This only works with newer git versions:
# git -C actor-framework pull || git clone https://github.com/actor-framework/actor-framework.git
if cd actor-framework
then
  git pull
else 
  git clone https://server/repo repo
fi
cd $ROOT_DIR/actor-framework
git checkout topic/improve-autoconnect
./configure --build-type=release --no-opencl --no-tools --no-examples
make -j$cores
cd $ROOT_DIR
