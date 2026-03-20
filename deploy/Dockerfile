FROM golang:1.25-alpine AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY go.work go.work.sum ./
COPY go-libfossil/ go-libfossil/
COPY leaf/ leaf/
RUN cd leaf && go build -buildvcs=false -o /leaf ./cmd/leaf/

FROM alpine:3.20
RUN apk add --no-cache ca-certificates curl
COPY --from=build /leaf /usr/local/bin/leaf
ENTRYPOINT ["leaf"]
