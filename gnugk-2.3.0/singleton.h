//////////////////////////////////////////////////////////////////
//
// singleton.h
//
// All singleton objects are put into a list
// so that it would be delete when program exits.
//
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitly grant the right to link this code
// with the OpenH323 library.
//
//////////////////////////////////////////////////////////////////

#ifndef SINGLETON_H
#define SINGLETON_H "@(#) $Id: singleton.h,v 1.20 2009/02/09 13:25:59 willamowius Exp $"

// STL
#include <list>
#include <algorithm>
//#include <stdexcept>

//
// a list of pointers that would delete all objects
// referred by the pointers in the list on destruction
//
template<class T> class listptr : public std::list<void *> {
  public:
	listptr() : clear_list(false) {}
	~listptr();
	bool clear_list;

  private:
	static void delete_obj(void *t) { delete static_cast<T *>(t); }
};

template<class T> listptr<T>::~listptr()
{
	clear_list=true;
	std::for_each(begin(), end(), delete_obj);
}


// Base class for all singletons
class SingletonBase {
  public:
	SingletonBase(const char *);
	virtual ~SingletonBase();

  private:
	const char *m_name;
	// Note the SingletonBase instance is not singleton itself :p
	// However, list of singletons *are* singleton
	// But we can't put the singleton into the list :(
	static listptr<SingletonBase> _instance_list;
};

//
// A singleton class should be derived from this template.
// class Ts : public Singleton<Ts> {
//     ...
// };
//
// If the class is instantiated more than once,
// a runtime error would be thrown
//
// To access the singleton use T::Instance()
//
template<class T> class Singleton : public SingletonBase {
  public:
	static T *Instance();
	static bool InstanceExists();

  protected:
	Singleton(const char *);
	virtual ~Singleton();

  public:
	static T *m_Instance;
	static PMutex m_CreationLock;
};

template<class T> Singleton<T>::Singleton(const char *n) : SingletonBase(n)
{
//	if (m_Instance != 0)
//		throw std::runtime_error("Duplicate instances");
}

template<class T> Singleton<T>::~Singleton()
{
	PWaitAndSignal lock(m_CreationLock);
	m_Instance = 0;
}

// Function to access the singleton
template<class T> T *Singleton<T>::Instance()
{
	if (m_Instance == 0) {
		PWaitAndSignal lock(m_CreationLock);
		// We have to check it again after we got the lock
		if (m_Instance == 0)
			m_Instance = new T;
	}
	return m_Instance;
}

// Function to check for existance of singleton
template<class T> bool Singleton<T>::InstanceExists()
{
	return (m_Instance != 0);
}

// static members
template<class T> T *Singleton<T>::m_Instance = 0;
template<class T> PMutex Singleton<T>::m_CreationLock;


#endif // SINGLETON_H
