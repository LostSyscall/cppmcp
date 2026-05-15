# Stage 1: Build with vcpkg dependencies
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# Use Tsinghua mirror for apt (HTTP — ca-certificates not yet installed)
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-11 \
    cmake \
    make \
    pkg-config \
    git \
    ca-certificates \
    curl \
    zip \
    unzip \
    tar \
    && rm -rf /var/lib/apt/lists/*

ENV CXX=g++-11
ENV CC=gcc-11
# Proxy for vcpkg's curl (git uses host network directly with --network=host)
ENV HTTP_PROXY=http://host.docker.internal:10808
ENV HTTPS_PROXY=http://host.docker.internal:10808

# Fix network instability: force HTTP/1.1 and increase buffer
RUN git config --global http.version HTTP/1.1 \
    && git config --global http.postBuffer 524288000

# Bootstrap vcpkg (shallow clone)
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /src/cppmcp

# Copy vcpkg manifest only (not vcpkg-configuration.json — use local baseline to avoid git fetch)
COPY vcpkg.json ./
RUN bash -c 'for i in 1 2 3; do /opt/vcpkg/vcpkg install --triplet x64-linux && break || sleep 10; done'

# Copy project source
COPY CMakeLists.txt ./
COPY src/ src/
COPY include/ include/
COPY tests/ tests/
COPY examples/ examples/

# Configure and build
RUN cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCPPMCP_BUILD_TESTS=ON \
    -DCPPMCP_BUILD_EXAMPLES=ON \
    && cmake --build build

# Run tests
RUN cd build && ctest --output-on-failure -C Debug

# Stage 2: Dev environment (pre-built deps, ready for incremental rebuild)
FROM ubuntu:22.04 AS dev

ENV DEBIAN_FRONTEND=noninteractive

# Use Tsinghua mirror for apt (HTTP)
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-11 \
    cmake \
    make \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CXX=g++-11
ENV CC=gcc-11
ENV VCPKG_ROOT=/opt/vcpkg

COPY --from=build /opt/vcpkg /opt/vcpkg

WORKDIR /src/cppmcp