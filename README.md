# Nitrux Update Tool System (NUTS) | [![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

Modern C++ rewrite of NUTS with MauiKit and PolicyKit integration.

![NUTS](https://nxos.org/wp-content/uploads/2026/02/screenshot-20260204-141553.png)
> A simple utility to update Nitrux.

## Introduction

We designed the Nitrux Update Tool System utility to update [Nitrux OS](https://nxos.org/) and to provide a rollback backup option.

> [!WARNING]
> We intended the Nitrux Update Tool System to work exclusively in Nitrux OS; using this utility in other distributions will break them or cause it to stop working. Please do not open issues regarding this use case; they will be closed.

## Features

- It creates a backup of the XFS partition using xfsdump and stores it locally.
- Then, it downloads an OTA-style archive and updates the system atomically.
- Rollbacks are handled offline and integrated into the Nitrux ecosystem.

### Configuration:

The Nitrux Update Tool System reads the `/etc/nuts.conf` file to load certain settings.

# Licensing

The license for this repository and its contents is **BSD-3-Clause**.

# Issues

If you find problems with the contents of this repository, please create an issue and use the **🐞 Bug report** template.

## Submitting a bug report

Before submitting a bug, you should look at the [existing bug reports](https://github.com/Nitrux/nuts/issues) to verify that no one has reported the bug already.

©2026 Nitrux Latinoamericana S.C.
