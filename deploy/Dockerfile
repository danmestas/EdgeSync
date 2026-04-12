FROM golang:1.26-alpine AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY go-libfossil/ go-libfossil/
COPY leaf/ leaf/
RUN cd leaf && GOWORK=off go build -buildvcs=false -o /leaf ./cmd/leaf/

FROM alpine:3.20
RUN apk add --no-cache ca-certificates curl
COPY --from=build /leaf /usr/local/bin/leaf
ENTRYPOINT ["leaf"]
