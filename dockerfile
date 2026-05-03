FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libsdl2-dev \
    libgl1-mesa-dev \
    libglew-dev \
    libzmq3-dev \
    libcurl4-openssl-dev \
    libpq-dev \
    git \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir -p /app/third_party/stb && \
    wget -O /app/third_party/stb/stb_image.h \
    https://raw.githubusercontent.com/nothings/stb/master/stb_image.h

RUN rm -rf build && mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc)

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsdl2-2.0-0 \
    libglew2.2 \
    libzmq5 \
    libcurl4 \
    libpq5 \
    libgl1-mesa-glx \
    libglx0 \
    libopengl0 \
    libglx-mesa0 \
    libegl1 \
    libdrm2 \
    libx11-6 \
    libxcb1 \
    libxau6 \
    libxdmcp6 \
    libxext6 \
    libxfixes3 \
    libxrender1 \
    libxrandr2 \
    libxcursor1 \
    libxinerama1 \
    libxi6 \
    libxxf86vm1 \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/main .

ENV DB_HOST=db
ENV DB_PORT=5432
ENV DB_NAME=android_backend
ENV DB_USER=postgres
ENV DB_PASSWORD=somepassword
ENV SDL_VIDEODRIVER=x11

ENTRYPOINT ["./main"]