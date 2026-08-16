# Sextant, built and served from one image.
#
# THREE STAGES, ON PURPOSE
#
# The C++ toolchain is ~1.5 GB and Node's node_modules is another few hundred
# megabytes. Neither is needed to RUN anything, so both stay in build stages and
# the final image copies out one binary and a directory of static files.
#
# NOT VERIFIED IN THIS REPOSITORY'S CI
#
# Stated plainly because the execution plan says to test this on a clean machine
# and the environment it was written in has no Docker daemon. The commands are
# the same ones `make demo` runs and CI runs those on every push, so the risk is
# in the packaging rather than in the steps. Treat a first `docker compose up`
# as something to watch rather than something proven.

# --- stage 1: the C++ -------------------------------------------------------
FROM debian:bookworm-slim AS cpp-build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates \
      libpq-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Dependencies are fetched by CMake at configure time. Copying the CMake files
# first means a source-only change does not re-fetch googletest and yaml-cpp,
# which is most of the configure cost.
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSEXTANT_BUILD_TESTS=OFF -DSEXTANT_BUILD_BENCH=OFF || true

COPY include/ ./include/
COPY src/ ./src/
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSEXTANT_BUILD_TESTS=OFF -DSEXTANT_BUILD_BENCH=OFF \
 && cmake --build build -j "$(nproc)" --target sextant

# --- stage 2: the frontend --------------------------------------------------
FROM node:22-slim AS web-build

WORKDIR /web
# package.json and the lockfile first, so npm ci is cached independently of the
# application source.
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

# --- stage 3: the runtime ---------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libpq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --create-home --shell /bin/false sextant

WORKDIR /app

COPY --from=cpp-build /src/build/src/cli/sextant /usr/local/bin/sextant
COPY --from=web-build /web/dist ./web/dist

# The schema, the committed data samples and the golden sets. These are what
# make the image self-contained: it can ingest and resolve with no network.
COPY schema/ ./schema/
COPY data/ ./data/
COPY eval/ ./eval/
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh && chown -R sextant:sextant /app

USER sextant
EXPOSE 8080

# The database lives on a volume so a restart does not mean a re-resolve.
VOLUME ["/app/sextant-db"]

HEALTHCHECK --interval=10s --timeout=3s --start-period=90s --retries=5 \
  CMD ["/usr/local/bin/sextant", "schema", "--schema", "/app/schema"]

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
