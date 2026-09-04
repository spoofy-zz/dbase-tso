# DBASE/TSO

DBASE/TSO is a small dBASE-style command processor for MVS 3.8j / MVTK5. It is
written in C for the `cc370` toolchain and packaged/deployed with MBT.

The goal is not to emulate every dBASE III/IV feature. This is a practical,
retro-friendly table editor/query shell for TSO that lets you define simple
tables, append records, list data, search text, replace fields, mark records as
deleted, recall them, navigate with a record pointer, import/export pipe-delimited
data, and pack the active records back together.

## What It Does

DBASE/TSO gives a TSO user a dot-prompt command shell:

```text
DBASE/TSO 0.1 for MVS 3.8j - type HELP
.
```

It supports a compact subset of dBASE-like commands:

- `CREATE` defines a table and its fields.
- `CREATE` supports `C`, `N`, `D`, and `L` field types.
- `TABLES` lists defined tables in the VSAM store.
- `USE` selects an existing table.
- `APPEND` inserts a record with `FIELD=value` assignments.
- `APPEND FROM ddname` imports pipe-delimited records from a DD.
- `LIST` displays active records.
- `LIST ALL` also displays records marked as deleted.
- `LIST FOR field=value` filters records by one field.
- `DISPLAY STRUCTURE` shows the current table definition.
- `FIND` searches all fields for text.
- `REPLACE` changes fields in one record.
- `DELETE` marks one record as deleted.
- `RECALL` unmarks a deleted record.
- `PACK` removes deleted records from the active record stream.
- `GO`, `GOTO`, and `SKIP` move the current record pointer.
- `COPY TO ddname` exports pipe-delimited records to a DD.
- `COUNT` reports active and physical record counts.
- `HELP`, `QUIT`, and `EXIT` do what their names imply.

## Storage Backend

The current backend is a VSAM KSDS allocated to DD name `DBASEV`.

Default dataset:

```text
IBMUSER.DBASE.KV
```

Default IDCAMS allocation:

```jcl
  DEFINE CLUSTER (NAME(IBMUSER.DBASE.KV) -
         INDEXED -
         KEYS(64 0) -
         RECORDSIZE(1024 1024) -
         RECORDS(16000 4000) -
         SHAREOPTIONS(2 3) -
         UNIQUE -
         SPEED VOLUMES(TSO003)) -
    DATA (NAME(IBMUSER.DBASE.KV.DATA)) -
    INDEX (NAME(IBMUSER.DBASE.KV.INDEX))
```

The KSDS is used as a fixed-record key/value store. The first 64 bytes are the
key and the remaining 960 bytes hold serialized table metadata or row data.

Key families:

- `A|table` stores the table definition.
- `R|table|000001` and up store table records.

The VSAM access layer uses the same `clibvsam` pattern as the MiniSQL/TSO
project: `__vsopen`, `__vsread`, `__vswrit`, and `__vsdel`. The physical row
count is derived by probing consecutive row keys, so there is no mutable count
metadata record.

Practical limits in this version:

- up to 12 fields per table
- up to 32 characters per field value
- up to 999 records per table

These limits keep virtual storage usage modest for MVS 3.8j batch and TSO
execution.

## Architecture

The load library contains two entry points:

- `DBASE` is the interactive TSO command processor.
- `DBBATCH` is the batch version used by `jcl/DBASE.jcl` smoke tests.

The TSO module is built from `src/dbasetso.c`, which enables TSO terminal I/O and
includes the shared core from `src/dbase.c`. The small assembler helpers
`asm/dbtget.asm` and `asm/dbtput.asm` wrap TSO `TGET` and `TPUT`.

The batch module is built directly from `src/dbase.c` and reads commands from
`SYSIN`, which makes it useful for repeatable tests.

## Build

```sh
make
make package
```

## Deploy

`.env` contains the Hercules/zOSMF settings. It is ignored by
Git because it contains the password.

```sh
make deploy-mvs
```

The deploy target builds `DBASE`, allocates `IBMUSER.DBASE.KV`, deploys
`IBMUSER.DBASE.LOAD`, uploads source members to `IBMUSER.DBASE`, and installs
`SYS2.CMDPROC(DBASE)`.

Important deploy outputs:

- `IBMUSER.DBASE.LOAD(DBASE)` - interactive TSO module
- `IBMUSER.DBASE.LOAD(DBBATCH)` - batch module
- `IBMUSER.DBASE.KV` - VSAM table/record store
- `IBMUSER.DBASE` - uploaded source/JCL PDS
- `SYS2.CMDPROC(DBASE)` - TSO CLIST launcher

Warning: `make deploy-mvs` runs `jcl/ALLOCVS.jcl`, which resets
`IBMUSER.DBASE.KV`. Any test data in that VSAM cluster is deleted during deploy.

## TSO Use

From TSO:

```text
DBASE
```

Example session:

```text
CREATE PEOPLE ID N 8 NAME C 24 AGE N 3 CITY C 16 ACTIVE L 1
TABLES
APPEND ID=1 NAME=ANA AGE=42 CITY=ZAGREB ACTIVE=Y
APPEND ID=2 NAME=MARKO AGE=35 CITY=SPLIT ACTIVE=Y
LIST
LIST FOR CITY=SPLIT
DISPLAY STRUCTURE
FIND ANA
REPLACE 2 NAME=IVAN
GO TOP
SKIP 1
DELETE 1
RECALL 1
PACK
QUIT
```

## Batch Smoke Test

The batch smoke test runs `DBBATCH` with commands from `SYSIN`:

```sh
zowe zos-jobs submit local-file jcl/DBASE.jcl --wait-for-output \
  --host hercules --port 1080 --user RVEZ001 --password '...' \
  --protocol http --reject-unauthorized false
```

Expected output includes:

```text
Table PEOPLE created with 5 fields
Tables:
  PEOPLE            5 fields
Record 1 added
Record 2 added
RECNO ID       NAME                     AGE CITY             ACTIVE
    1 1        ANA                      42                   .
    2 2        MARKO                    35  SPLIT            Y
3 active records (3 physical)
```
