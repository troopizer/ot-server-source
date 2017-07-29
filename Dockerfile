FROM ot-server-build-base

RUN git clone --recursive https://github.com/otland/forgottenserver.git \
    && cd forgottenserver && mkdir build && cd build \
    && cmake .. \
    && make \
    && mv tfs ../

