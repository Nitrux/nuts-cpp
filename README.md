# Nitrux Update Tool System (NUTS) | [![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

C++ and MauiKit rewrite of the Nitrux Update tool System.

## Introduction

We designed the Nitrux Update Tool System utility to update [Nitrux OS](https://nxos.org/) and to provide a rollback backup option.

> [!WARNING]
> We intended the Nitrux Update Tool System to work exclusively in Nitrux OS; using this utility in other distributions will break them or cause it to stop working. Please do not open issues regarding this use case; they will be closed.

## Features

- It creates a backup of the XFS partition using xfsdump and stores it locally.
- Then, it downloads an OTA-style archive and updates the system atomically.
- Rollbacks are handled offline and integrated into the Nitrux ecosystem through the Live session using xfsrestore.

## Configuration

Configuration file: `/etc/nuts.conf`

## License

BSD-3-Clause

## Issues

If you encounter any issues with this repository, please open an issue.

©2026 Nitrux Latinoamericana S.C.
