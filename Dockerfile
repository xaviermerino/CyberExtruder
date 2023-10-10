FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive

COPY scripts/udevadm /bin/udevadm
COPY cxxopts/include/cxxopts.hpp /usr/include/cxxopts.hpp

RUN apt update && apt upgrade && apt install -y build-essential	libx11-dev \
    libxcb-shape0-dev libxcb-xfixes0-dev libbz2-dev sm libcairo2-dev \ 
    libxxf86vm-dev libpango1.0-dev libatk1.0-0 libgdk-pixbuf2.0-0 \ 
    libgl1-mesa-dev libghc-glut-dev libglu1-mesa libgtkgl2.0-1 \
    libgtkmm-2.4-dev software-properties-common dumb-init

RUN add-apt-repository ppa:ubuntu-toolchain-r/test && \
    apt update && apt install -y gcc-11 g++-11 && \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-11 11 && \
    update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-11 11

RUN chmod +x /bin/udevadm

COPY Aureus /Aureus

WORKDIR /Aureus
RUN cd /Aureus/Aureus_Tracking && ./build.sh
ENTRYPOINT ["/usr/bin/dumb-init", "--", "./Aureus_Tracking_exe"]