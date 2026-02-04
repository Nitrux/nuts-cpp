// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

#pragma once

#include <QtCore/qglobal.h>

#if defined(NUTS_LIBRARY)
#  define NUTS_EXPORT Q_DECL_EXPORT
#else
#  define NUTS_EXPORT Q_DECL_IMPORT
#endif
