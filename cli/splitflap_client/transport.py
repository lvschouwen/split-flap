"""HTTP transport for one board. No retries on mutations; verbatim error bodies."""
from __future__ import annotations

from typing import Any

import httpx


class SplitflapError(Exception):
    pass


class Unreachable(SplitflapError):
    def __init__(self, url: str, cause: Exception):
        super().__init__(f"unreachable: {url} ({cause})")
        self.url = url
        self.cause = cause


class HttpError(SplitflapError):
    def __init__(self, status: int, body: str, url: str):
        super().__init__(f"HTTP {status} from {url}: {body}")
        self.status = status
        self.body = body
        self.url = url


class ParseError(SplitflapError):
    pass


DEFAULT_TIMEOUT = httpx.Timeout(connect=2.0, read=5.0, write=5.0, pool=2.0)


class BoardClient:
    def __init__(self, base_url: str, *,
                 timeout: httpx.Timeout = DEFAULT_TIMEOUT,
                 transport: httpx.BaseTransport | None = None):
        self.base_url = base_url.rstrip("/")
        self._http = httpx.Client(base_url=self.base_url, timeout=timeout,
                                  transport=transport)

    def close(self) -> None:
        self._http.close()

    def __enter__(self) -> "BoardClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _request(self, method: str, path: str, **kw) -> httpx.Response:
        try:
            resp = self._http.request(method, path, **kw)
        except httpx.TransportError as exc:
            raise Unreachable(f"{self.base_url}{path}", exc) from exc
        if resp.status_code >= 400:
            raise HttpError(resp.status_code, resp.text, str(resp.url))
        return resp

    def get_json(self, path: str, params: dict | None = None) -> Any:
        resp = self._request("GET", path, params=params)
        try:
            return resp.json()
        except ValueError as exc:
            raise ParseError(f"invalid JSON from {resp.url}") from exc

    def get_text(self, path: str, params: dict | None = None) -> str:
        return self._request("GET", path, params=params).text

    def get(self, path: str, params: dict | None = None) -> httpx.Response:
        """Raw GET (mirrors post()) for routes whose 2xx status is itself the
        answer: GET /cluster/discover returns a text/plain 202 while the scan
        runs and JSON only on 200, so get_json would ParseError on the 202."""
        return self._request("GET", path, params=params)

    def post(self, path: str, *, params: dict | None = None,
             data: dict | None = None) -> httpx.Response:
        return self._request("POST", path, params=params, data=data)

    def stream(self, path: str):
        """Context manager for a long-lived GET (SSE): read timeout disabled.
        Accept header is load-bearing: the vendored ESPAsyncWebServer only
        routes /events to the SSE handler when the request declares
        text/event-stream (AsyncEventSource canHandle -> isSSE()); without it
        the request 404s and the body must not be parsed as an empty stream."""
        timeout = httpx.Timeout(connect=2.0, read=None, write=5.0, pool=2.0)
        return self._http.stream("GET", path, timeout=timeout,
                                 headers={"Accept": "text/event-stream"})
