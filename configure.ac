AC_INIT(libtorrent, 0.12.6, jaris@ifi.uio.no)

dnl Find a better way to do this
AC_DEFINE(PEER_NAME, "-lt0C60-", Identifier that is part of the default peer id)
AC_DEFINE(PEER_VERSION, "lt\x0C\x60", 4 byte client and version identifier for DHT)

LIBTORRENT_CURRENT=11
LIBTORRENT_REVISION=6
LIBTORRENT_AGE=0

LIBTORRENT_INTERFACE_VERSION_INFO=$LIBTORRENT_CURRENT:$LIBTORRENT_REVISION:$LIBTORRENT_AGE
LIBTORRENT_INTERFACE_VERSION_NO=$LIBTORRENT_CURRENT.$LIBTORRENT_AGE.$LIBTORRENT_REVISION

AC_SUBST(LIBTORRENT_CURRENT)
AC_SUBST(LIBTORRENT_INTERFACE_VERSION_INFO)
AC_SUBST(LIBTORRENT_INTERFACE_VERSION_NO)

AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(config.h)
AC_DISABLE_STATIC

AM_DISABLE_STATIC
AM_PROG_LIBTOOL
AC_PROG_CXX

AC_C_BIGENDIAN(
    AC_DEFINE(IS_BIG_ENDIAN, 1, Big endian),
    AC_DEFINE(IS_LITTLE_ENDIAN, 1, Little endian),
    AC_MSG_ERROR([Could not determine endianness])
)

TORRENT_ENABLE_ALIGNED
TORRENT_CHECK_CXXFLAGS
TORRENT_ENABLE_DEBUG
TORRENT_ENABLE_EXTRA_DEBUG
TORRENT_ENABLE_WERROR
TORRENT_ENABLE_TR1

TORRENT_DISABLE_IPV6

AC_SYS_LARGEFILE

TORRENT_ENABLE_ARCH
TORRENT_WITH_SYSROOT

dnl TORRENT_WITH_XFS
TORRENT_WITH_KQUEUE
TORRENT_WITHOUT_EPOLL
TORRENT_WITH_POSIX_FALLOCATE
TORRENT_WITH_ADDRESS_SPACE

TORRENT_WITHOUT_STATVFS
TORRENT_WITHOUT_STATFS

CC_ATTRIBUTE_VISIBILITY

AC_ARG_ENABLE(openssl,
  [  --disable-openssl       Don't use OpenSSL's SHA1 implementation.],
  [
    if test "$enableval" = "yes"; then
      PKG_CHECK_MODULES(OPENSSL, openssl,
        CXXFLAGS="$CXXFLAGS `pkg-config --cflags openssl`";
        LIBS="$LIBS -lcrypto `pkg-config --libs-only-L --libs-only-other openssl`")

      AC_DEFINE(USE_OPENSSL, 1, Using OpenSSL.)
      AC_DEFINE(USE_OPENSSL_SHA, 1, Using OpenSSL's SHA1 implementation.)
    else
      AC_DEFINE(USE_NSS_SHA, 1, Using Mozilla's SHA1 implementation.)
    fi
  ],[
    PKG_CHECK_MODULES(OPENSSL, openssl,
      CXXFLAGS="$CXXFLAGS `pkg-config --cflags openssl`";
      LIBS="$LIBS -lcrypto `pkg-config --libs-only-L --libs-only-other openssl`")

    AC_DEFINE(USE_OPENSSL, 1, Using OpenSSL.)
    AC_DEFINE(USE_OPENSSL_SHA, 1, Using OpenSSL's SHA1 implementation.)
  ]
)

PKG_CHECK_MODULES(STUFF, sigc++-2.0,
	          CXXFLAGS="$CXXFLAGS $STUFF_CFLAGS";
		  LIBS="$LIBS $STUFF_LIBS")

TORRENT_CHECK_MADVISE()
TORRENT_MINCORE()
TORRENT_OTFD()

LIBTORRENT_LIBS="-ltorrent"
AC_SUBST(LIBTORRENT_LIBS)

LIBTORRENT_CFLAGS=""
AC_SUBST(LIBTORRENT_CFLAGS)

AC_DEFINE(HAVE_CONFIG_H, 1, true if config.h was included)

CC_ATTRIBUTE_UNUSED(
	AC_DEFINE([__UNUSED], [__attribute__((unused))], [Wrapper around unused attribute]),
	AC_DEFINE([__UNUSED], [], [Null-wrapper if unused attribute is unsupported])
)

AC_OUTPUT([
	libtorrent.pc
	Makefile
	src/Makefile
	src/torrent/Makefile
	src/torrent/peer/Makefile
	src/torrent/data/Makefile
	src/data/Makefile
	src/dht/Makefile
	src/download/Makefile
	src/net/Makefile
	src/protocol/Makefile
	src/tracker/Makefile
	src/utils/Makefile
])
