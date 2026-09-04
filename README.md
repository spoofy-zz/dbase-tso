# DBASE/TSO

DBASE/TSO is a small dBASE-style command processor for MVS 3.8j / MVTK5. It is
written in C for the `cc370` toolchain and packaged/deployed with MBT.

The goal is not to emulate every dBASE III/IV feature. This is a practical,
retro-friendly table editor/query shell for TSO that lets you define simple
tables, append records, list data, search text, replace fields, mark records as
deleted, and pack the active records back together.

## What It Does

DBASE/TSO gives a TSO user a dot-prompt command shell:

```text
DBASE/TSO 0.1 for MVS 3.8j - type HELP
.
```

It supports a compact subset of dBASE-like commands:

- `CREATE` defines a table and its fields.
- `USE` selects an existing table.
- `APPEND` inserts a record with `FIELD=value` assignments.
- `LIST` displays active records.
- `LIST ALL` also displays records marked as deleted.
- `DISPLAY STRUCTURE` shows the current table definition.
- `FIND` searches all fields for text.
- `REPLACE` changes fields in one record.
- `DELETE` marks one record as deleted.
- `PACK` removes deleted records from the active record stream.
- `COUNT` reports active and physical record counts.
- `HELP`, `QUIT`, and `EXIT` do what their names imply.

## Storage Backend

The current backend is a fixed-block sequential MVS dataset allocated to DD name
`DBASED`.

Default dataset:

```text
RVEZ001.DBASE.TEXT
```

Default JCL allocation:

```jcl
//DBASED   DD DSN=RVEZ001.DBASE.TEXT,DISP=(NEW,CATLG,DELETE),
//            UNIT=SYSDA,VOL=SER=TSO003,SPACE=(TRK,(5,2)),
//            DCB=(RECFM=FB,LRECL=384,BLKSIZE=6144)
```

The file is used as a small line-oriented key/value store. Table definitions and
records are serialized as text lines. On startup the program reads the store
into memory. On each change it rewrites the store through `fopen("DBASED", "w")`.

This backend was chosen because it is simple, visible from normal MVS tools, and
works reliably under the `cc370` C runtime on TK5. An earlier VSAM KSDS backend
was tested, but `clibvsam` writes returned `PUT RC=8` on this host, so the
current implementation deliberately uses ordinary dataset I/O.

Practical limits in this version:

- up to 8 fields per table
- up to 32 characters per field value
- up to 240 records per table
- up to 256 physical key/value entries in the store

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

`.env` contains the Hercules/zOSMF settings for `RVEZ001`. It is ignored by
Git because it contains the password.

```sh
make deploy-mvs
```

The deploy target builds `DBASE`, allocates `RVEZ001.DBASE.TEXT`, deploys
`RVEZ001.DBASE.LOAD`, uploads source members to `RVEZ001.DBASE`, and installs
`SYS2.CMDPROC(DBASE)`.

Important deploy outputs:

- `RVEZ001.DBASE.LOAD(DBASE)` - interactive TSO module
- `RVEZ001.DBASE.LOAD(DBBATCH)` - batch module
- `RVEZ001.DBASE.TEXT` - table/record store
- `RVEZ001.DBASE` - uploaded source/JCL PDS
- `SYS2.CMDPROC(DBASE)` - TSO CLIST launcher

Warning: `make deploy-mvs` runs `jcl/ALLOCVS.jcl`, which resets
`RVEZ001.DBASE.TEXT`. Any test data in that dataset is deleted during deploy.

## TSO Use

From TSO:

```text
DBASE
```

Example session:

```text
CREATE PEOPLE ID 8 NAME 24 AGE 3
APPEND ID=1 NAME=ANA AGE=42
APPEND ID=2 NAME=MARKO AGE=35
LIST
DISPLAY STRUCTURE
FIND ANA
REPLACE 2 NAME=IVAN
DELETE 1
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
Table PEOPLE created with 3 fields
Record 1 added
Record 2 added
RECNO ID       NAME                     AGE
    1 1        ANA                      42
    2 2        MARKO                    35
2 active records (2 physical)
```
