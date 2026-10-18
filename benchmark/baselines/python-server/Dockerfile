# The upstream benchmark's Python server, ported to the official Python MCP SDK 2.x (FastMCP was
# renamed MCPServer and its transport options moved to streamable_http_app()). The workload is
# unchanged. Dependencies and the base image are pinned so the build cannot drift to a release
# the server was not written for.
FROM python@sha256:e88e9763f943ec1834f992a4b51e0f24500486803e8bc534e5767af9ea65f6ce

WORKDIR /app

COPY requirements.txt constraints.txt ./
RUN pip install --no-cache-dir -r requirements.txt -c constraints.txt

COPY main.py .

EXPOSE 8082

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8082", "--workers", "4", "--loop", "uvloop"]
