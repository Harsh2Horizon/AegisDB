# AegisDB HTTP API v1

HTTP/1.1 over TCP; default `127.0.0.1:8080`. There is no authentication or TLS. Bind only within a trusted environment. Admin operations have the same trust boundary. Each connection serves one request and closes. Send `Content-Type: application/json` on POSTs. JSON object bodies and canonical, padded Base64 are required; whitespace and noncanonical padding in Base64 are rejected.

## Operations

| Method and path | Request body | Successful response |
|---|---|---|
| POST `/v1/kv/put` | `key_base64`, `value_base64`, optional boolean `sync` (default true) | 200 `{"success":true}` |
| POST `/v1/kv/get` | `key_base64`, optional `snapshot_id` | 200 `{"found":true,"value_base64":"..."}` or `{"found":false}` |
| POST `/v1/kv/delete` | `key_base64`, optional `sync` | 200 `{"success":true}`; deletion is idempotent in logical effect |
| POST `/v1/kv/batch` | `operations` array, optional `sync` | 200 `{"success":true}`; entire batch has atomic visibility |
| POST `/v1/kv/scan` | optional `start_key_base64`, `end_key_base64`, `after_key_base64`, `limit`, `snapshot_id` | 200 `{"entries":[...],"next_after_key_base64":null or "..."}` |
| POST `/v1/snapshots` | `{}` | 201 `{"snapshot_id":"...","sequence":42,"ttl_seconds":300}` |
| DELETE `/v1/snapshots/{id}` | none | 200 `{"success":true}` |
| GET `/v1/health` | none | 200 `{"status":"ok"}` |
| GET `/v1/stats` | none | 200 measured counters and `levels` array |
| POST `/v1/admin/flush` | `{}` | 202 `{"success":true,"operation_id":"..."}` |
| POST `/v1/admin/compact` | `{}` | 202 `{"success":true,"operation_id":"..."}` |
| GET `/v1/admin/operations/{id}` | none | 200 `{"state":"queued/running/done/failed","code":"OK/..."}` |

Batch example:

```json
{"sync":true,"operations":[
  {"type":"put","key_base64":"YQ==","value_base64":"QQ=="},
  {"type":"delete","key_base64":"Yg=="},
  {"type":"put","key_base64":"Yw==","value_base64":"Qw=="}
]}
```

Operations receive consecutive sequence numbers. If a key appears more than once, the last operation wins. All validation precedes WAL modification. A failed or canceled request can have committed a complete batch; there are no idempotency tokens. The server never returns a partial committed batch.

## Scans and snapshots

Scans use `[start,end)`, bytewise ordering and one value per key. Missing start means the empty key; missing end means no upper bound. `after_key_base64` resumes strictly after that user key. Limit defaults to the smaller of 1,000 and the configured engine result limit, which defaults to 10,000. Decoded page payload is additionally capped at 32 MiB; the JSON/Base64 response is larger. An individual record exceeding the scan payload cap produces RESOURCE_EXHAUSTED. A non-null continuation can lead to an empty last page if remaining records are invisible or deleted.

For repeatable pagination, create one snapshot and use its `snapshot_id` in every page. Changing or omitting the snapshot produces independently consistent pages with potentially different logical state. Expired/unknown snapshot handles return 404, rather than silently falling back to current state. Snapshot IDs include a random 128-bit server generation and a counter. They are not authentication credentials. TTL is measured from creation; using a snapshot does not extend it. Restart invalidates all handles. Embedded snapshots have RAII ownership; server snapshots must be deleted or allowed to expire.

```sh
curl -s -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8080/v1/snapshots
# Copy the returned snapshot_id into SNAPSHOT_ID.
SNAPSHOT_ID='returned-generation-and-counter'
curl -s -H 'Content-Type: application/json' http://127.0.0.1:8080/v1/kv/scan \
  -d "{\"start_key_base64\":\"YQ==\",\"end_key_base64\":\"eg==\",\"limit\":100,\"snapshot_id\":\"$SNAPSHOT_ID\"}"
# Copy next_after_key_base64 into the next request's after_key_base64; retain SNAPSHOT_ID.
curl -s -X DELETE "http://127.0.0.1:8080/v1/snapshots/$SNAPSHOT_ID"
```

## Maintenance and shutdown

The engine flush call waits until the entry-time committed prefix is installed in SSTables. Compact flushes that prefix and performs a deterministic leveled sweep, or rewrites the bottom level when only bottom-level files remain. With automatic compaction disabled, it first relieves existing L0 pressure. Concurrent later mutations may remain in memory.

HTTP maintenance is queued on one separate worker. The default registry/queue limit is 64 operations. Terminal operation entries can be evicted when a new operation is admitted; a missing status is 404. Operation IDs are process-local. Poll them rather than assuming a 202 means completion.

```sh
curl -s -H 'Content-Type: application/json' -d '{}' http://127.0.0.1:8080/v1/admin/flush
curl -s http://127.0.0.1:8080/v1/admin/operations/RETURNED_OPERATION_ID
```

SIGINT/SIGTERM closes admission and network sessions, then waits for admitted database handlers and maintenance operations. The filesystem synchronization or compaction already in progress is allowed to finish; there is no hard shutdown deadline or cancellation of a disk syscall. Network responses can be canceled even when their write committed. Request receive/send deadlines apply to socket operations, not execution time of admitted database work. The serialized writer has its own admission/backpressure timeout. At maximum connection admission, newly accepted sockets are closed before parsing a request.

## Errors and limits

Errors use `{"success":false,"error":{"code":"INVALID_ARGUMENT","message":"INVALID_ARGUMENT"}}`. HTTP messages are sanitized; contextual infrastructure errors belong in operator logs. Missing keys use 200 `found:false`, while an invalid snapshot or endpoint uses 404.

| HTTP | Meaning |
|---|---|
| 400 | Invalid JSON, fields, bounds, Base64, or operation |
| 404 | Unknown route, snapshot, or maintenance operation |
| 408 | TIMEOUT if represented as an API error; a receive/send deadline ordinarily closes the socket |
| 413 | Beast body-size rejection (default request cap 32 MiB) |
| 429 | Engine/registry resource limit |
| 503 | BUSY/backpressure or SHUTTING_DOWN |
| 500 | IO_ERROR, CORRUPTION, INTERNAL_ERROR |

See `config/example.json`, `ServerOptions` and `Options` for limits. Key/value bounds apply to decoded bytes. JSON header parsing is capped at 16 KiB. Server snapshots are bounded and swept every second. Request tasks are bounded by admitted connections; only one request runs per connection. Increasing limits increases possible aggregate memory use.

Socket and write-admission timeouts must be positive and at most five minutes; snapshot TTL must be positive and at most 24 hours. These configuration limits prevent overflow when constructing steady-clock deadlines. Statistics counters are process-local and sampled atomically; hierarchy/frontier information is captured under the state lock. They are operational observations, rather than one transactional snapshot across every counter.
