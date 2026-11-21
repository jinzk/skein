#ifndef SKEIN_MYSQL_HEADERS_H
#define SKEIN_MYSQL_HEADERS_H

/* Keep the MySQL client include portable between CMake builds (which pass an
 * explicit include directory) and source-native module builds (which only see
 * the module source tree). */
#if defined(__has_include)
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#define SKEIN_MYSQL_HEADERS_FOUND 1
#elif __has_include(<mariadb/mysql.h>)
#include <mariadb/mysql.h>
#define SKEIN_MYSQL_HEADERS_FOUND 1
#elif __has_include(<mysql.h>)
#include <mysql.h>
#define SKEIN_MYSQL_HEADERS_FOUND 1
#endif
#else
#include <mysql/mysql.h>
#define SKEIN_MYSQL_HEADERS_FOUND 1
#endif

#ifndef SKEIN_MYSQL_HEADERS_FOUND
#error "MySQL/MariaDB client headers not found; install libmysqlclient-dev or libmariadb-dev"
#endif

#endif
