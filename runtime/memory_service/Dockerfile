FROM python:3.12-slim-bookworm
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 PYTHONPATH=/opt/upstream
WORKDIR /opt/kim-memory
COPY requirements.lock .
RUN pip install --no-cache-dir --disable-pip-version-check -r requirements.lock
COPY upstream/mimirs-memory-hub/vividmimir /opt/upstream/vividmimir
COPY upstream/mimirs-memory-hub/LICENSE /opt/upstream/LICENSE
COPY app.py .
RUN groupadd --gid 11081 kim && useradd --uid 11081 --gid 11081 --home-dir /nonexistent --no-create-home kim \
    && mkdir /data && chown kim:kim /data
USER 11081:11081
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=4s --start-period=20s CMD python -c "import urllib.request,os; t=open('/run/secrets/kim_mimir_token').read().strip(); urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8080/health',headers={'Authorization':'Bearer '+t}),timeout=2).read()" || exit 1
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080", "--workers", "1", "--no-access-log"]
