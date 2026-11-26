# `std.httpCompression`

Explicit gzip response transformation and bounded request decompression.

```lyra
import std.http.{Request, Response}
import std.httpCompression.{gzipResponse}

async fn handle(request: Request): Response {
    match gzipResponse(request, Response.text(200, "large public response")) {
        Ok(response) => response,
        Err(error) => Response.text(500, error.message()),
    }
}
```

`gzipResponse` leaves application-sensitive compression decisions with the caller.
It combines repeated `Accept-Encoding` fields, recognizes case-insensitive `gzip`,
`identity`, wildcard and valid three-digit quality values, and keeps the lowest
weight when a coding is repeated. An explicit `gzip` weight overrides `*`; an
explicit higher `identity` weight keeps the identity representation. Missing,
empty or malformed negotiation fields keep the response uncompressed. Explicit
rejection of both supported representations produces 406.

Negotiable responses vary on `Accept-Encoding`, `Range` and `Cache-Control`, so
an identity response selected because of a request directive or range cannot be
served to a request that would otherwise get gzip. Existing `Vary` fields are
retained, including `*`. Responses with a content encoding, content range,
status 204/205/206/304, or `Cache-Control: no-transform` already set on the
response pass through unchanged. Gzip output drops the original representation's
ETag, digest and byte-range advertisements. HEAD uses the same selected headers
and never runs the body producer.

Buffered and streaming bodies use the same backpressured streaming writer.
Large writes are encoded in 16 KiB slices, each downstream write is awaited,
and each producer write ends with a sync flush. Cancellation, producer failure
or invalid writes close the encoder. A writer retained after its producer
returns reports `Closed`.

`GzipRequestReader.fromRequest(request, maxOutputBytes: ...)` requires a streaming
request and a single `Content-Encoding: gzip`. Use one reader per request and do
not mix its reads with raw `request.readBody()` calls. `read` returns bounded decoded
chunks, accepts concatenated gzip members and returns `Ok(None)` only after the
HTTP body ends and gzip trailer validation succeeds. The compressed-byte budget
comes from `HttpServerOptions`; the decoded-byte budget is independent. Bad
data/checksums or truncation report `Malformed`, exceeded output reports
`TooLarge`. These failures close the decoder, not the request: handlers can
return 400/413, and the HTTP transport prevents reuse when responding before
the raw body ends. `close` abandons just the decoder; the request retains its
normal lifetime. Bytes delivered before EOF still need the final checksum
validation before irreversible use.
