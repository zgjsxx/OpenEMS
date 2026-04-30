FROM debian:bookworm-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir --break-system-packages pymodbus==2.5.3

WORKDIR /app
COPY docker/e2e/simulator/simulator.py /app/simulator.py

EXPOSE 1502 18080

CMD ["python3", "/app/simulator.py"]
