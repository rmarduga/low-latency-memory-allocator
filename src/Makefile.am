# Build the object pool implementation library
lib_LTLIBRARIES = libobjectpool.la

libobjectpool_la_SOURCES = object_pool_impl.cpp
libobjectpool_la_CPPFLAGS = -I$(top_srcdir)/include -std=c++1y
libobjectpool_la_LDFLAGS = -lpthread

# Install the header files
include_HEADERS = $(top_srcdir)/include/object_pool_impl.hpp $(top_srcdir)/include/object_pool.hpp $(top_srcdir)/include/spinlock_mutex.hpp
