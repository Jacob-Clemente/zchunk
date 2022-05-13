# Build Stage
FROM --platform=linux/amd64 ubuntu:20.04 as builder

## Install build dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y cmake

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y meson

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y ninja-build

## Add source code to the build stage.
ADD . /zchunk
WORKDIR /zchunk

## TODO: ADD YOUR BUILD INSTRUCTIONS HERE.
RUN meson build && cd build && ninja

#Package Stage
FROM --platform=linux/amd64 ubuntu:20.04

## TODO: Change <Path in Builder Stage>
COPY --from=builder /zchunk/build/src/zck /
COPY --from=builder /zchunk/build/src/unzck /

COPY --from=builder /zchunk/build/src/lib/libzck.so.1 /usr/lib/x86_64-linux-gnu/libzck.so.1
