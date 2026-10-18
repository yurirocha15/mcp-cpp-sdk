FROM python@sha256:57cd7c3a7a273101a6485ba99423ee568157882804b1124b4dd04266317710de

RUN pip install --disable-pip-version-check --no-cache-dir redis==5.2.1

ENTRYPOINT ["python3"]
