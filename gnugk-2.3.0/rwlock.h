//////////////////////////////////////////////////////////////////
//
// rwlock.h
//
// Utilities for PReadWriteMutex usage
//
// Copyright (c) Citron Network Inc. 2002
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitly grant the right to link this code
// with the OpenH323 library.
//
// initial author: Chin-Wei Huang <cwhuang@linux.org.tw>
// initial version: 6/7/2002
//
//////////////////////////////////////////////////////////////////

#ifndef RWLOCK_H
#define RWLOCK_H "@(#) $Id: rwlock.h,v 1.6 2009/02/09 13:25:59 willamowius Exp $"

class ReadLock {
	PReadWriteMutex & mutex;
  public:
	ReadLock(PReadWriteMutex & m) : mutex(m) { mutex.StartRead(); }
	~ReadLock() { mutex.EndRead(); }
};

class WriteLock {
	PReadWriteMutex & mutex;
  public:
	WriteLock(PReadWriteMutex & m) : mutex(m) { mutex.StartWrite(); }
	~WriteLock() { mutex.EndWrite(); }
};

class ReadUnlock {
	PReadWriteMutex & mutex;
  public:
	ReadUnlock(PReadWriteMutex & m) : mutex(m) { mutex.EndRead(); }
	~ReadUnlock() { mutex.StartRead(); }
};

class WriteUnlock {
	PReadWriteMutex & mutex;
  public:
	WriteUnlock(PReadWriteMutex & m) : mutex(m) { mutex.EndWrite(); }
	~WriteUnlock() { mutex.StartWrite(); }
};

extern PReadWriteMutex ConfigReloadMutex;

#endif // RWLOCK_H
