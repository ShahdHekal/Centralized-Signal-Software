#!/bin/bash
set -e   
FLAGS="-std=c++20 -O2 -static -D_WIN32_WINNT=0x0A00 -DASIO_STANDALONE -Ithird_party/asio"

g++ $FLAGS TCP_server.cpp -o TCP_server.exe -lws2_32 -lmswsock

g++ $FLAGS -Ithird_party/unicorn eeg_client.cpp -o eeg_client.exe \
    -Lthird_party/unicorn -lUnicorn -lws2_32 -lmswsock
cp third_party/unicorn/Unicorn.dll .

echo "built TCP_server.exe and eeg_client.exe"
