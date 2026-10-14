FROM python:3.11-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 HF_HUB_OFFLINE=1 HF_HOME=/home/kim/.cache/huggingface
WORKDIR /app
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt && groupadd -g 1000 kim && useradd -u 1000 -g 1000 -m kim
COPY --chown=1000:1000 character.py engine.py memory_control.py voice.py response_tags.py transport.py /app/
COPY --chown=1000:1000 tmp-nltk /opt/nltk_data/tokenizers/punkt_tab
ENV NLTK_DATA=/opt/nltk_data
USER 1000:1000
CMD ["python", "-m", "transport"]
