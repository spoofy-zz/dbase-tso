# DBASE/TSO

Small dBASE-style command processor for MVS 3.8j / MVTK5, built with cc370 and
deployed with MBT.

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

The program stores table definitions and up to 240 records per table in a fixed-block line dataset
allocated as DD `DBASED`.
