/*
 *   Copyright � 2008 dragchan <zgchan317@gmail.com>
 *   This file is part of FbTerm.
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

#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include "config.h"
#include "fbterm.h"
#include "fbshell.h"
#include "fbshellman.h"
#include "fbconfig.h"
#include "fbio.h"
#include "screen.h"
#include "input.h"
#include "input_key.h"
#include "mouse.h"
#include "improxy.h"

#ifdef HAVE_SIGNALFD
// <sys/signalfd.h> offered by some systems has bug with g++
#include "signalfd.h"

sigset_t oldSigmask;

class SignalIo : public IoPipe {
public:
	SignalIo(sigset_t &sigmask);

private:
	virtual void readyRead(s8 *buf, u32 len);
};

SignalIo::SignalIo(sigset_t &sigmask)
{
	int fd = signalfd(-1, &sigmask, 0);
	setFd(fd);
}

void SignalIo::readyRead(s8 *buf, u32 len)
{
	signalfd_siginfo *si = (signalfd_siginfo*)buf;
	for (len /= sizeof(*si); len--; si++) {
		FbTerm::instance()->processSignal(si->ssi_signo);
	}
}
#else

static volatile sig_atomic_t pendsigs = 0;

static void signalHandler(s32 signo)
{
	pendsigs |= 1 << signo;
}

static void pollSignal()
{
	if (!pendsigs) return;
	
	sig_atomic_t sigs = pendsigs;
	pendsigs = 0;

	for (u32 i = 0; i < sizeof(sigs); i++) {
		u8 sig8 = (sigs >> (i * 8)) & 0xff;
		if (!sig8) continue;

		for (u32 j = 0; j < 8; j++) {
			if (sig8 & (1 << j)) {
				FbTerm::instance()->processSignal(i * 8 + j);
			}
		}
	}
}
#endif

DEFINE_INSTANCE_DEFAULT(FbTerm)

FbTerm::FbTerm()
{
	mInit = false;
	init();
}

FbTerm::~FbTerm()
{
	IoDispatcher::uninstance();
	FbShellManager::uninstance();
	ImProxy::uninstance();
	Screen::uninstance();
}

void FbTerm::init()
{
	if (!TtyInput::instance() || !Screen::instance()) return;

	struct vt_mode vtm;
	vtm.mode = VT_PROCESS;
	vtm.waitv = 0;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	vtm.frsig = 0;
	ioctl(STDIN_FILENO, VT_SETMODE, &vtm);

#ifdef HAVE_SIGNALFD
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGUSR1);
	sigaddset(&sigmask, SIGUSR2);
	sigaddset(&sigmask, SIGALRM);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGHUP);

	sigprocmask(SIG_BLOCK, &sigmask, &oldSigmask);
	new SignalIo(sigmask);
#else
	sighandler_t sh = signalHandler;

	signal(SIGUSR1, sh);
	signal(SIGUSR2, sh);
	signal(SIGALRM, sh);
	signal(SIGTERM, sh);
	signal(SIGHUP, sh);
#endif
	signal(SIGPIPE, SIG_IGN);

	Mouse::instance();
	mInit = true;
}

void FbTerm::run()
{
	if (!mInit) return;
	FbShellManager::instance()->createShell();

	mRun = true;
	FbIoDispatcher *io = (FbIoDispatcher*)IoDispatcher::instance();
	while (mRun) {
		io->poll();
#ifndef HAVE_SIGNALFD
		pollSignal();
#endif
	}
}

void FbTerm::processSignal(u32 signo)
{
	switch (signo) {
	case SIGTERM:
	case SIGHUP:
		exit();
		break;

	case SIGALRM:
		FbShellManager::instance()->drawCursor();
		break;

	case SIGUSR1:
		FbShellManager::instance()->switchVc(false);
		Screen::instance()->switchVc(false);
		TtyInput::instance()->switchVc(false);
		ioctl(STDIN_FILENO, VT_RELDISP, 1);
		break;

	case SIGUSR2:
		TtyInput::instance()->switchVc(true);
		Screen::instance()->switchVc(true);
		FbShellManager::instance()->switchVc(true);
		break;

	default:
		break;
	}
}

void FbTerm::processSysKey(u32 key)
{
	FbShellManager *manager = FbShellManager::instance();

	switch (key) {
	case CTRL_ALT_E:
		exit();
		break;

	case SHIFT_PAGEDOWN:
	case SHIFT_PAGEUP:
		manager->historyScroll(key == SHIFT_PAGEDOWN);
		break;

	case CTRL_ALT_C:
		manager->createShell();
		break;

	case CTRL_ALT_D:
		manager->deleteShell();
		break;

	case CTRL_ALT_1 ... CTRL_ALT_0:
		manager->switchShell(key - CTRL_ALT_1);
		break;

	case SHIFT_LEFT:
		manager->prevShell();
		break;

	case SHIFT_RIGHT:
		manager->nextShell();
		break;

	case CTRL_ALT_F1 ... CTRL_ALT_F6:
		if (manager->activeShell()) {
			manager->activeShell()->switchCodec(key - CTRL_ALT_F1);
		}
		break;

	case CTRL_SPACE:
		manager->toggleIm();
		break;

	default:
		break;
	}
}

void FbTerm::initChildProcess()
{
#ifndef HAVE_FS_CAPABILITY
    setuid(getuid());
#endif

#ifdef HAVE_SIGNALFD
	sigprocmask(SIG_SETMASK, &oldSigmask, 0);
#endif

	signal(SIGPIPE, SIG_DFL);
}

#ifndef HAVE_FS_CAPABILITY
u32 effective_uid;
#endif

int main(int argc, char **argv)
{
#ifndef HAVE_FS_CAPABILITY
	effective_uid = geteuid();
	seteuid(getuid());
#endif

	if (Config::instance()->parseArgs(argc, argv)) {
		FbTerm::instance()->run();
		FbTerm::uninstance();
	}
	
	Config::uninstance();
	return 0;
}
