/*
 *   Copyright � 2008 dragchan <zgchan317@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef FBIO_H
#define FBIO_H

#include "io.h"
#include "hash.h"

class FbIoDispatcher : public IoDispatcher {
public:
	void poll();

private:
	friend class IoDispatcher;
	FbIoDispatcher();
	~FbIoDispatcher();

	virtual void addIoSource(IoPipe *src, bool isread);
	virtual void removeIoSource(IoPipe *src, bool isread);

	s32 mEpollFd;
	HashTable<IoPipe*> *mpIoSources;
};

#endif
