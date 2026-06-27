# googbase

A custom database system built in C from scratch. No SQL — designed for performance over readability.

## Build

```sh
make
```

## Run

```sh
./googbase
```

## File format

| Offset | Size | Field |
|--------|------|-------|
| 0      | 4    | Magic `GOOG` |
| 4      | 4    | Page count (uint32 LE) |
| 8      | 4096 | Page data (repeated) |

## Status

Early development. Currently supports page-based file storage with allocate, read, and write operations.
