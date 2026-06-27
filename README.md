# googdb

A custom database system built in C from scratch. No SQL — designed for performance over readability.

## Build

```sh
make
```

## Run

```sh
./googdb test.gdb
```

## Features

- **Page-based storage** — 4096-byte pages with linked-list data pages per table
- **Tables with schema** — INT32, UINT32, FLOAT, STRING column types
- **Key-value mode** — PUT/GET/DEL commands, auto-creates buckets
- **Encryption** — XTEA-128 in CTR mode, encrypt/decrypt all pages on demand
- **Interactive CLI** — CREATE TABLE, INSERT, SELECT, DELETE, WHERE clauses

## Examples

```
googdb> CREATE TABLE users (id INT32, name STRING, age INT32)
OK
googdb> INSERT INTO users VALUES (1, 'Alice', 30)
OK
googdb> INSERT INTO users VALUES (2, 'Bob', 25)
OK
googdb> SELECT * FROM users
[0] id=1 name='Alice' age=30
[1] id=2 name='Bob' age=25
googdb> SELECT * FROM users WHERE id = 1
[0] id=1 name='Alice' age=30
googdb> PUT mybucket mykey myvalue
OK
googdb> GET mybucket mykey
myvalue
googdb> ENCRYPT mypassword
Encryption enabled
googdb> .tables
users
mybucket
googdb> .exit
```

## Commands

| Command | Description |
|---------|-------------|
| `CREATE TABLE name (col TYPE ...)` | Create a table |
| `INSERT INTO name VALUES (...)` | Insert a row |
| `SELECT * FROM name [WHERE c = v]` | Query rows |
| `DELETE FROM name WHERE c = v` | Delete rows |
| `PUT bucket key value` | Key-value set |
| `GET bucket key` | Key-value get |
| `DEL bucket key` | Key-value delete |
| `ENCRYPT password` | Enable/rekey encryption |
| `ENCRYPT OFF` | Disable encryption |
| `.tables` | List tables |
| `.schema name` | Show table schema |
| `.help` | Show help |
| `.exit` | Exit |

## File format

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Magic `GOOG` |
| 4 | 4 | Page count (uint32 LE) |
| 8 | 4 | Flags (bit 0 = encrypted) |
| 12 | 4 | Reserved |

Page 0 is the table directory. Each table has a schema page. Data pages form linked lists.
