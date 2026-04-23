FROM golang:1.26-alpine AS build
RUN apk add --no-cache gcc musl-dev git
WORKDIR /src
COPY libfossil/ libfossil/
COPY leaf/ leaf/
RUN cd leaf && \
    go mod edit -replace github.com/danmestas/libfossil=/src/libfossil \
                -replace github.com/danmestas/libfossil/db/driver/modernc=/src/libfossil/db/driver/modernc \
                -replace github.com/danmestas/libfossil/db/driver/ncruces=/src/libfossil/db/driver/ncruces && \
    GOWORK=off go build -buildvcs=false -o /leaf ./cmd/leaf/

FROM alpine:3.20
RUN apk add --no-cache ca-certificates curl
COPY --from=build /leaf /usr/local/bin/leaf
ENTRYPOINT ["leaf"]
