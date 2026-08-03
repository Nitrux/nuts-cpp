#pragma once

#include <QtCore/qglobal.h>
#include <QString>

inline QString operator+(const char* lhs, const QString& rhs) {
    return QString::fromUtf8(lhs) + rhs;
}


#if defined(NUTS_LIBRARY)
#  define NUTS_EXPORT Q_DECL_EXPORT
#else
#  define NUTS_EXPORT Q_DECL_IMPORT
#endif
