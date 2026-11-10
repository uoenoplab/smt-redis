# Redis over Homa and SMT

This tree is Redis 8.10.1 with two more transports beside TCP:

- **Homa** ([PlatformLab/HomaModule](https://github.com/PlatformLab/HomaModule)): a message-based
  datacenter transport. All clients share one server socket, and each request and its reply are one
  Homa RPC.
- **SMT** (Secure Message Transport, [uoenoplab/smt](https://github.com/uoenoplab/smt)): Homa with
  TLS records inside the transport, from our S&P'26 paper "Designing Transport-Level Encryption for
  Datacenter Networks".

The server, the bundled hiredis, `redis-cli` and `redis-benchmark` all speak both transports. Redis's
own README is [README-redis.md](README-redis.md).

## Build

```
make -j
```

Tested on Linux 6.17.8 with Homa `main` @ `1c59d7b6` loaded (`homa.ko`).

## Run

Server, listening on TCP 6379 and Homa port 2000:

```
src/redis-server --port 6379 --homa-port 2000 --bind 0.0.0.0 --io-threads 1
```

`--smt-port` opens an SMT listener in the same way. Homa binds a port, never an address, so `bind` must
include `*` or `0.0.0.0`.

Clients, with the server's IPv4 address:

```
src/redis-cli -h 10.0.1.1 -p 2000 --homa ping
src/redis-benchmark -h 10.0.1.1 -p 2000 --homa -t get,set -c 16
```

`--smt` in place of `--homa` connects over SMT. A C program uses `redisConnectHoma()` or
`redisConnectSmt()` from the bundled hiredis (`deps/hiredis`) in place of `redisConnect()`.

## How it works

- **Server** (`src/homa.c`): one Homa socket per listener. When it is readable, the event loop
  drains the ready RPCs and gives each to the Redis client of its peer (ip:port). When Redis has
  executed all commands of an RPC, their replies leave as that RPC's response, in one `sendmsg`,
  from Redis's output buffers.
- **Client** (`deps/hiredis/net.c`): one RPC in flight per connection. A response goes to the reply
  parser straight from Homa's receive buffer. Closing a connection sends `QUIT`, since Homa has no
  FIN to tell the server that the client is gone.
- **Idle peers**: the server checks a slice of its peer table every 10 ms, as Redis's `clientsCron`
  does. It drops a peer that is idle for longer than `timeout`. When `timeout` is 0, the limit is 60 s.

## Limits

- `io-threads 1` only.
- The server refuses `SUBSCRIBE`, `PSUBSCRIBE`, `SSUBSCRIBE` and `MONITOR` over Homa: Homa cannot
  push a message to a client that did not ask for it.
- A request or a reply must fit in one Homa message (1 MB). A larger reply gets an error.
- The hiredis async API does not support Homa.
- IPv4 only.

## Results

Two pairs of CloudLab xl170 (25 Gb/s), Homa against TCP. Each artifact branch holds the runbook,
scripts, raw results and figures.

| Branch | What | Main result |
|---|---|---|
| [`artifact/microbench`](https://github.com/uoenoplab/smt-redis/tree/artifact/microbench) | one mechanism at a time: the transport alone, one request over one connection, a 512 KB reply, batching, 16 to 25,000 connections, head-of-line blocking, YCSB-C | Homa's server CPU per request stays flat from 16 to 25,000 connections (20.5-21.7 us). Stock TCP's grows from 29.3 to 39.3 us |
| [`artifact/fleet`](https://github.com/uoenoplab/smt-redis/tree/artifact/fleet) | 4 to 24,576 independent clients against one Redis shard, request mixes from Twitter's cache trace ([memtier_benchmark with Homa](https://github.com/uoenoplab/memtier_benchmark)) | Small values: Homa's GET p50 is 9-34% lower. TCP uses up to 1.7x Homa's server CPU per request. Mixed sizes up to 35 KB: TCP's capacity is 6-25% higher. |

## Branches

| Branch | What |
|---|---|
| `main` | this tree |
| `artifact/microbench`, `artifact/fleet` | the measurements above |
| `archive/sp2026` | the Redis port of the S&P'26 paper (Redis 7.2.4, the first SMT) |
| `archive/sp2026-ktls` | the paper's TCP + kTLS baseline |

The YCSB-C client with the same transports is [uoenoplab/smt-YCSB-C](https://github.com/uoenoplab/smt-YCSB-C).
