//////////////////////////////////////////////////////////////////
//
// config.h configuration header
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitly grant the right to link this code
// with the OpenH323 library.
//
//////////////////////////////////////////////////////////////////


#ifndef CONFIG_H
#define CONFIG_H "@(#) $Id: config.h,v 1.4 2009/02/09 13:25:59 willamowius Exp $"

#include "pwlib_compat.h"

#ifdef _WIN32
#include "gnugkbuildopts.h"
#endif

#if HAS_MYSQL || HAS_PGSQL || HAS_FIREBIRD || HAS_ODBC || HAS_SQLITE
#define		HAS_DATABASE 1
#endif

#endif // CONFIG_H

