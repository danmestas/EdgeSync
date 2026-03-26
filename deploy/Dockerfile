FROM golang:1.25-alpine AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY go-libfossil/ go-libfossil/
COPY leaf/ leaf/
RUN cd leaf && GOWORK=off go build -buildvcs=false -o /leaf ./cmd/leaf/

# --- Iroh sidecar (Rust) ---
FROM rust:1-bookworm AS iroh-builder
WORKDIR /src
COPY iroh-sidecar/ ./iroh-sidecar/
RUN cargo build --release --manifest-path iroh-sidecar/Cargo.toml

FROM alpine:3.20
RUN apk add --no-cache ca-certificates curl
COPY --from=build /leaf /usr/local/bin/leaf
COPY --from=iroh-builder /src/iroh-sidecar/target/release/iroh-sidecar /usr/local/bin/
ENTRYPOINT ["leaf"]
