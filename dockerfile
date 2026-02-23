# Dockerfile for OS Capstone: Multi-Process Trading System
FROM ubuntu:24.04

# Avoid interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install development tools and dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    gdb \
    valgrind \
    linux-tools-generic \
    clang-14 \
    clang-tools-14 \
    libgtest-dev \
    libgmock-dev \
    wget \
    curl \
    vim \
    htop \
    && rm -rf /var/lib/apt/lists/*

# Set up alternatives for clang
RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-14 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-14 100

# Build Google Test (Ubuntu 24.04 requires building from source)
RUN cd /usr/src/gtest && \
    cmake . && \
    make && \
    cp lib/*.a /usr/lib/

# Create working directory
WORKDIR /workspace

# Copy project files (when running, mount your code as volume)
# COPY . /workspace

# Set up huge pages (runtime configuration)
# Note: This requires --privileged flag or specific capabilities
RUN echo "vm.nr_hugepages=128" > /etc/sysctl.d/99-hugepages.conf

# Create mount point for shared memory
RUN mkdir -p /dev/shm && chmod 1777 /dev/shm

# Set environment variables
ENV CC=clang
ENV CXX=clang++
ENV ASAN_OPTIONS=detect_leaks=1:symbolize=1
ENV TSAN_OPTIONS=history_size=7:force_seq_cst_atomics=1

# Add helpful aliases
RUN echo 'alias ll="ls -lah"' >> /root/.bashrc && \
    echo 'alias build-debug="cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j\$(nproc)"' >> /root/.bashrc && \
    echo 'alias build-release="cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"' >> /root/.bashrc && \
    echo 'alias build-asan="cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build -j\$(nproc)"' >> /root/.bashrc && \
    echo 'alias build-tsan="cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON && cmake --build build -j\$(nproc)"' >> /root/.bashrc

# Expose any ports if needed (for monitoring/GUI)
# EXPOSE 8080

# Default command
CMD ["/bin/bash"]