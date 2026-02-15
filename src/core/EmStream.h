/* -*- mode: C++; tab-width: 4 -*- */
// ===========================================================================
//	EmStream.h				   �1993-1998 Metrowerks Inc. All rights reserved.
// ===========================================================================
//
//	Abstract class for reading/writing an ordered sequence of bytes

#ifndef EmStream_h
#define EmStream_h

#include "EmTypes.h"			// ErrCode

#include <deque>
#include <list>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------

enum StreamFromType
{
	kStreamFromStart = 1,
	kStreamFromEnd,
	kStreamFromMarker
};

// ---------------------------------------------------------------------------

class EmStream
{
	public:
								EmStream		(void);
		virtual					~EmStream		(void);

		virtual void			SetMarker		(int32			inOffset,
												 StreamFromType	inFromWhere);
		virtual int32			GetMarker		(void) const;

		virtual void			SetLength		(int32			inLength);
		virtual int32			GetLength		(void) const;

		Bool					AtEnd			(void) const
								{
									return GetMarker () >= GetLength ();
								}

						// Write Operations

		virtual ErrCode			PutBytes		(const void*	inBuffer,
												 int32			ioByteCount);

		EmStream&				operator <<		(const char* inString);
		EmStream&				operator <<		(const std::string& inString);
		EmStream&				operator <<		(int8 inNum);
		EmStream&				operator <<		(uint8 inNum);
		EmStream&				operator <<		(char inChar);
		EmStream&				operator <<		(int16 inNum);
		EmStream&				operator <<		(uint16 inNum);
		EmStream&				operator <<		(int32 inNum);
		EmStream&				operator <<		(uint32 inNum);
		EmStream&				operator <<		(int64 inNum);
		EmStream&				operator <<		(uint64 inNum);
		EmStream&				operator <<		(bool inBool);

						// Read Operations

		virtual ErrCode			GetBytes		(void*	outBuffer,
												 int32	ioByteCount);
		int32					PeekData		(void*	outButter,
												 int32	inByteCount);

		EmStream&				operator >>		(char* outString);
		EmStream&				operator >>		(std::string& outString);
		EmStream&				operator >>		(int8 &outNum);
		EmStream&				operator >>		(uint8 &outNum);
		EmStream&				operator >>		(char &outChar);
		EmStream&				operator >>		(int16 &outNum);
		EmStream&				operator >>		(uint16 &outNum);
		EmStream&				operator >>		(int32 &outNum);
		EmStream&				operator >>		(uint32 &outNum);
		EmStream&				operator >>		(int64 &outNum);
		EmStream&				operator >>		(uint64 &outNum);
		EmStream&				operator >>		(bool &outBool);

		template <class T>
		EmStream&				operator >>		(std::deque<T>& container)
								{
									Int32	numElements;
									*this >> numElements;

									container.resize (numElements);

									typename std::deque<T>::iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this >> *iter;
										++iter;
									}

									return *this;
								}

		template <class T>
		EmStream&				operator >>		(std::list<T>& container)
								{
									Int32	numElements;
									*this >> numElements;

									container.resize (numElements);

									typename std::list<T>::iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this >> *iter;
										++iter;
									}

									return *this;
								}

		template <class T>
		EmStream&				operator >>		(std::vector<T>& container)
								{
									Int32	numElements;
									*this >> numElements;

									container.resize (numElements);

									typename std::vector<T>::iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this >> *iter;
										++iter;
									}

									return *this;
								}

		template <class T>
		EmStream&				operator <<		(const std::deque<T>& container)
								{
									Int32	numElements = container.size ();

									*this << numElements;

									typename std::deque<T>::const_iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this << *iter;
										++iter;
									}

									return *this;
								}

		template <class T>
		EmStream&				operator <<		(const std::list<T>& container)
								{
									Int32	numElements = container.size ();

									*this << numElements;

									typename std::list<T>::const_iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this << *iter;
										++iter;
									}

									return *this;
								}

		template <class T>
		EmStream&				operator <<		(const std::vector<T>& container)
								{
									Int32	numElements = container.size ();

									*this << numElements;

									typename std::vector<T>::const_iterator	iter = container.begin ();
									while (iter != container.end ())
									{
										*this << *iter;
										++iter;
									}

									return *this;
								}


		// Data-specific read/write functions
		//   There is an equivalent Shift operator for each one
		//	 except WritePtr/ReadPtr (since Ptr is really a char*,
		//	 which is the same as a C string).

		int32					WriteCString	(const char*	inString);
		int32					ReadCString		(char*			outString);

		int32					WriteString		(const std::string&	inString);
		int32					ReadString		(std::string&		outString);

		int						PrintF			(const char* fmt, ...);
		int						ScanF			(const char* fmt, ...);

		int						PutC			(int);
		int						GetC			(void);

		int						PutS			(const char*);
		char*					GetS			(char*, int n);

	protected:
		int32					mMarker;
		int32					mLength;
};




inline EmStream& operator << (EmStream& s, const MemPtr& p)
{
	// 64-BIT FIX: Casting pointer to 32-bit emuptr loses high bits!
	// This is conceptually broken - MemPtr should be emulated addresses, not host pointers
	// TODO: Fix serialization logic to properly handle emulated vs. host addresses
	s << (emuptr) reinterpret_cast<uintptr_t>(p);

	return s;
}

inline EmStream& operator >> (EmStream& s, MemPtr& p)
{
	emuptr	t;
	s >> t;

	// 64-BIT FIX: This is unsafe on 64-bit! Upper bits are zero.
	p = reinterpret_cast<MemPtr>(static_cast<uintptr_t>(t));

	return s;
}

inline EmStream& operator << (EmStream& s, const MemHandle& h)
{
	// 64-BIT FIX: Same issue as MemPtr above
	s << (emuptr) reinterpret_cast<uintptr_t>(h);

	return s;
}

inline EmStream& operator >> (EmStream& s, MemHandle& h)
{
	emuptr	t;
	s >> t;

	// 64-BIT FIX: Same issue as MemPtr above
	h = reinterpret_cast<MemHandle>(static_cast<uintptr_t>(t));

	return s;
}


#if 0
	// Although the MSL headers indicate that it can support member template
	// functions (see _MSL_MUST_INLINE_MEMBER_TEMPLATE in mslconfig), the
	// following out-of-line definitions result in a compilation error.  So,
	// for now, the definitions are in-line.

template <class T>
EmStream& EmStream::operator>> (std::deque<T>& container)
{
	Int32	numElements;
	*this >> numElements;

	container.clear ();

	for (Int32 ii = 0; ii < numElements; ++ii)
	{
		T	object;
		*this >> object;
		container.push_back (object);
	}

	return *this;
}


template <class T>
EmStream& EmStream::operator<T> >> (std::list<T>& container)
{
	Int32	numElements;
	*this >> numElements;

	container.clear ();

	for (Int32 ii = 0; ii < numElements; ++ii)
	{
		T	object;
		*this >> object;
		container.push_back (object);
	}

	return *this;
}


template <class T>
EmStream& EmStream::operator<T> >> (std::vector<T>& container)
{
	Int32	numElements;
	*this >> numElements;

	container.clear ();

	for (Int32 ii = 0; ii < numElements; ++ii)
	{
		T	object;
		*this >> object;
		container.push_back (object);
	}

	return *this;
}


template <class T>
EmStream& EmStream::operator<T> << (const std::deque<T>& container)
{
	Int32	numElements = container.size ();

	*this << numElements;

	const typename std::deque<T>::const_iterator	iter = container.begin ();
	while (iter != container.end ())
	{
		*this << *iter;
	}

	return *this;
}


template <class T>
EmStream& EmStream::operator<T> << (const std::list<T>& container)
{
	Int32	numElements = container.size ();

	*this << numElements;

	const typename std::list<T>::const_iterator	iter = container.begin ();
	while (iter != container.end ())
	{
		*this << *iter;
	}

	return *this;
}


template <class T>
EmStream& EmStream::operator<T> << (const std::vector<T>& container)
{
	Int32	numElements = container.size ();

	*this << numElements;

	const typename std::vector<T>::const_iterator	iter = container.begin ();
	while (iter != container.end ())
	{
		*this << *iter;
	}

	return *this;
}
#endif	/* if 0 */

class EmStreamBlock : public EmStream
{
	public:
								EmStreamBlock	(void*, int32);
		virtual					~EmStreamBlock	(void);

		virtual void			SetLength		(int32			inLength);

		virtual ErrCode			PutBytes		(const void*	inBuffer,
												 int32			ioByteCount);
		virtual ErrCode			GetBytes		(void*			outBuffer,
												 int32			ioByteCount);

	private:
		void*					fData;
};



#endif	/* EmStream_h */
