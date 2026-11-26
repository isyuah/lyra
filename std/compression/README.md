# `std.compression`

Incremental gzip encoding and decoding, using the static zlib in the SDK:

```lyra
import std.compression.{GzipEncoder, GzipError}

fn encodeChunk(input: Bytes): Result<List<Bytes>, GzipError> {
    val encoder = GzipEncoder.open()?
    defer { val _ = encoder.close() }
    val first = encoder.push(input, flush: true)?
    val tail = encoder.finish()?
    Ok([first, tail])
}
```

`GzipEncoder.push` consumes a whole bounded input chunk and returns whatever compressed
bytes are available. An empty result is normal when zlib buffers input. `flush: true`
makes the input available to a streaming decoder without ending the member. `finish`
returns the remaining bytes, including the gzip trailer, and closes the encoder.
Write everything returned by both methods, in order. `close` abandons the member
without writing a trailer.

`pushRange(content, start, end, flush: true)` encodes a half-open byte range of
an existing `Bytes` without first copying the slice. The selected range has the
same input limit as `push`.

`GzipDecoder.push` accepts one immutable input chunk. Call `read` until `Ok(None)`
before pushing the next chunk. `Ok(Some(bytes))` gives a bounded decoded chunk;
`Ok(None)` means more input is needed, not that the entire gzip stream is complete.
When the source reports EOF, call `finish` to require a complete gzip member and
close the decoder. Concatenated gzip members are supported and share one output
budget. The decoder rejects a bad CRC, a bad uncompressed size, non-gzip data and
incomplete members. Bytes returned by `read` precede trailer validation; defer any
irreversible use that requires validated input until `finish` succeeds.

- Input and output chunk sizes are configurable from one byte through 16 MiB. The
  default input limit is 64 KiB. The encoder defaults to at most 256 KiB of output
  per call; the decoder returns at most 16 KiB per read, with an 8 MiB total
  decoded output limit by default. A zero total limit accepts only empty output.
- Invalid options and oversize input do not consume input. `Busy` means a native
  call is active, or the decoder must drain its accepted input before another
  `push` or `finish`. Format, truncation and exceeded output budget errors close
  the stream; future operations then report `Closed`.
- `close` is idempotent and releases codec state without validating or producing a
  trailer. GC releases abandoned state as a fallback; use explicit `close` for
  deterministic cleanup. Expensive calls with large chunk limits can run in
  `blocking`, but a stream's chunks must still be processed in order.
