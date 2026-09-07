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
- `SELECT` changes the active work area by number or alias.
- `USE name ALIAS alias` selects an existing table in the current work area.
- `APPEND` inserts a record with `FIELD=value` assignments.
- `APPEND BLANK` creates an empty record.
- `APPEND FROM ddname` imports pipe-delimited records from a DD.
- `LIST field-list` displays selected fields from active records.
- `LIST ALL` also displays records marked as deleted.
- `LIST FOR field op value` filters records with `=`, `<>`, `!=`, `<`, `>`,
  `<=`, `>=`, `AND`, `OR`, `NOT`, and parentheses.
- `DISPLAY field-list` shows selected fields from the current record.
- `DISPLAY STRUCTURE` shows the current table definition.
- `FIND` searches all fields for text.
- `REPLACE` changes fields in one record.
- `REPLACE field WITH value FOR field op value` changes matching records.
- `DELETE` marks one record, all records, or matching records as deleted.
- `RECALL` unmarks one deleted record, all deleted records, or matching records.
- `PACK` removes deleted records from the active record stream.
- `GO`, `GOTO`, and `SKIP` move the current record pointer.
- `LOCATE FOR` and `CONTINUE` search records with a saved condition.
- `SUM` and `AVERAGE` aggregate numeric fields.
- `ZAP` removes all records from the selected table but keeps its structure.
- `INDEX ON field TO name` builds a simple VSAM-backed index.
- `INDEXES`, `REINDEX`, `SET INDEX TO name`, and `SEEK value` use indexes.
- `SET RELATION TO field INTO table ON field` links parent and child tables.
- `SET FILTER TO expression` restricts commands in the current work area.
- `SET DELETED ON/OFF` hides or shows deleted records in the current work area.
- `STORE value TO var` and `STORE var=value` define in-memory variables.
- `&var` expands a memory variable inside later commands.
- `? expression` prints a value or simple numeric expression.
- `DISPLAY MEMORY` and `LIST MEMORY` show currently defined variables.
- `DO ddname` or `DO dataset(member)` runs a command file.
- Command files support `IF`, `ELSE`, `ENDIF`, `DO WHILE`, `ENDDO`,
  `AND`, `OR`, `NOT`, and parentheses in conditions.
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
- `I|table|index` stores index metadata.
- `K|table|index|value|recno` stores index entries.
- `R|table|000001` and up store table records.

Index entries are maintained after data changes such as `APPEND`, `REPLACE`,
`DELETE`, `RECALL`, `PACK`, and `ZAP`. `REINDEX` rebuilds all indexes defined
for the current table from the active rows.

Memory variables are process-local only. They are kept in the running TSO or
batch address space, are useful for command files and repeated substitutions,
and are not written to the VSAM cluster. Data tables, rows, index definitions,
and index entries are the persistent VSAM-backed objects.

Work areas are also runtime state. Each work area keeps its selected table,
alias, record pointer, active index, filter expression, and deleted-record
visibility. The persistent table and index contents remain in the VSAM store.

The VSAM access layer uses the same `clibvsam` pattern as the MiniSQL/TSO
project: `__vsopen`, `__vsread`, `__vswrit`, and `__vsdel`. The physical row
count is derived by probing consecutive row keys, so there is no mutable count
metadata record.

Practical limits in this version:

- up to 12 fields per table
- up to 32 characters per field value
- up to 32 memory variables per session
- up to 200 lines per `DO` command file
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
- `IBMUSER.DBASE(SCRIPT)` - sample `DO` command file
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
USE PEOPLE ALIAS P
TABLES
APPEND ID=1 NAME=ANA AGE=42 CITY=ZAGREB ACTIVE=Y
APPEND ID=2 NAME=MARKO AGE=35 CITY=SPLIT ACTIVE=Y
LIST
LIST FOR CITY=SPLIT
DISPLAY STRUCTURE
FIND ANA
REPLACE 2 NAME=IVAN
STORE IVAN TO NEWNAME
STORE 0 TO LOOP
DO IBMUSER.DBASE(SCRIPT)
DISPLAY MEMORY
REPLACE CITY WITH ZAGREB FOR NAME=ANA
LOCATE FOR AGE>30 AND NOT CITY=PULA
CONTINUE
SUM AGE
AVERAGE AGE
INDEX ON ID TO PID
INDEXES
SET INDEX TO PID
SEEK 2
REPLACE 2 ID=22
SEEK 22
REPLACE 2 ID=2
REINDEX
SEEK 2
SELECT 2
CREATE ORDERS CUSTID N 8 ITEM C 16
USE ORDERS ALIAS O
APPEND CUSTID=2 ITEM=BOOK
APPEND CUSTID=1 ITEM=PEN
SELECT P
DISPLAY STATUS
SET FILTER TO ACTIVE=Y AND NOT CITY=PULA
LIST P->NAME CITY
DISPLAY ALL P->ID P->NAME
SET FILTER OFF
SET RELATION TO ID INTO ORDERS ON CUSTID
GO 2
GO TOP
SKIP 1
DELETE 1
DELETE FOR AGE<30
SET DELETED OFF
LIST NAME
SET DELETED ON
RECALL ALL
PACK
QUIT
```

Example command file, supplied as `IBMUSER.DBASE(SCRIPT)` by the deploy step:

```text
IF &NEWNAME=IVAN AND NOT &LOOP=1
REPLACE 2 NAME=&NEWNAME
ELSE
REPLACE 2 NAME=BAD
ENDIF
DO WHILE &LOOP<2 AND &NEWNAME=IVAN
STORE &LOOP+1 TO LOOP
ENDDO
? &LOOP
```

In batch JCL, control-flow commands can also be written directly in `SYSIN`:

```jcl
//SYSIN    DD *
STORE ANA TO NEWNAME
IF &NEWNAME=ANA
REPLACE NAME WITH &NEWNAME FOR ID=1
ENDIF
/*
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
Using PEOPLE in work area 1 alias P
Tables:
  PEOPLE            5 fields
Record 1 added
Record 2 added
Record 2 replaced
NEWNAME = IVAN
LOOP = 0
Record 2 replaced
LOOP = 1
LOOP = 2
2
Memory variables:
1 record(s) replaced
Sum AGE = 104.00
Average AGE = 34.67
Index PID on ID built with 3 entries
Index PID active on ID
Record 2 replaced
Record 2 replaced
Reindexed 1 index(es), 3 entry(s)
Selected work area 2
Using ORDERS in work area 2 alias O
Selected work area 1 PEOPLE alias P
Work areas:
Filter set to ACTIVE=Y AND NOT CITY=PULA
RECNO NAME                     CITY
RECNO ID       NAME
Filter off
Relation PEOPLE.ID -> ORDERS.CUSTID active
RECNO ID       NAME                     AGE CITY             ACTIVE
    1 1        ANA                      42  ZAGREB           .
    2 2        IVAN                     35  SPLIT            Y
Related ORDERS:
RECNO CUSTID   ITEM
    1 2        BOOK
Deleted off
RECNO NAME
Deleted on
4 active records (4 physical)
```
