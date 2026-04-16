# Python Destination Container
FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir uv

WORKDIR /app

COPY pyproject.toml .
COPY src/ src/

# Install package and its dependencies
RUN uv pip install --system .

ENV PYTHONUNBUFFERED=1

CMD ["shmem-reader"]
