FROM debian:bookworm AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    python3 \
    python3-pip \
    libpq-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build-docker -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build-docker \
    && cmake --install build-docker --prefix /opt/openems/install


FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    curl \
    python3 \
    python3-pip \
    python3-venv \
    postgresql-client \
    libpq5 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/openems/install

COPY --from=builder /opt/openems/install /opt/openems/install
COPY docker/start_openems.sh /usr/local/bin/start_openems.sh

ENV VIRTUAL_ENV=/opt/openems/venv
ENV PATH="/opt/openems/venv/bin:${PATH}"

RUN chmod +x /usr/local/bin/start_openems.sh \
    && python3 -m venv "$VIRTUAL_ENV" \
    && "$VIRTUAL_ENV/bin/pip" install --no-cache-dir -r /opt/openems/install/web/requirements.txt

ENV OPENEMS_DB_URL=postgresql://postgres:postgres@postgres:5432/openems_admin
ENV OPENEMS_ADMIN_USERNAME=admin
ENV OPENEMS_WEB_PORT=8080
ENV OPENEMS_SYNC_CONFIG_ON_START=1
ENV OPENEMS_ENABLE_MODBUS=1
ENV OPENEMS_ENABLE_IEC104=1
ENV OPENEMS_ENABLE_HISTORY=1
ENV OPENEMS_ENABLE_ALARM=1

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=5 \
  CMD sh -c 'curl -fsS "http://127.0.0.1:${OPENEMS_WEB_PORT:-8080}/login" > /dev/null || exit 1'

ENTRYPOINT ["/usr/local/bin/start_openems.sh"]
