FROM alpine:3.20
RUN apk add --no-cache build-base cmake bash
WORKDIR /app
COPY . .
RUN chmod +x create_test_folders.sh && ./create_test_folders.sh
RUN chmod +x build.sh && ./build.sh
ENV RUN_FOREGROUND=1 \
    CONFIG_PATH=./run/config.txt \
    TARGET=lab1
STOPSIGNAL SIGTERM
WORKDIR /app
RUN ls -R
RUN pwd
ENTRYPOINT ["/bin/sh","-c","exec /app/run/${TARGET}"]