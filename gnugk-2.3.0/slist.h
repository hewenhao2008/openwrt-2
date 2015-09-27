//////////////////////////////////////////////////////////////////
//
// slist.h
//
// A Single Linked List Template   *** LEGACY - use STL for new code ***
//
// Copyright (c) Citron Network Inc. 2003
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitly grant the right to link this code
// with the OpenH323 library.
//
// initial author: Chih-Wei Huang <cwhuang@linux.org.tw>
// initial version: 07/16/2003
//
//////////////////////////////////////////////////////////////////

#ifndef SLIST_H
#define SLIST_H "@(#) $Id: slist.h,v 1.5 2009/02/09 13:25:59 willamowius Exp $"

#include "factory.h"

template<class T>
class SList {
public:
	typedef T Base;	// for SimpleCreator template 

	SList() : m_next(0) {}
	virtual ~SList() = 0;

	static T *Create(const PStringArray &);
	
protected:
	T *m_next;
};

template<class T>
SList<T>::~SList()
{
	delete m_next;  // delete whole list recursively
}

template<class T>
T *SList<T>::Create(const PStringArray & rules)
{
	T *next = 0;
	for (int i = rules.GetSize(); --i >= 0; )
		if (T *current = Factory<T>::Create(rules[i])) {
			current->m_next = next;
			next = current;
		}
	return next;
}

#endif // SLIST_H
