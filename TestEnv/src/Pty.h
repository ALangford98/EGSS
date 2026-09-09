#pragma once

// A POSIX pseudo-terminal wrapper. Knows nothing about terminal emulation
// or rendering -- just an OS-level byte pipe to a spawned shell, non-
// blocking so it never stalls Application::Run()'s single-threaded frame
// loop (a blocking read() on a shell producing no output would freeze
// rendering entirely).

#include <GS.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

class Pty
{
public:
	Pty() = default;
	Pty(const Pty&) = delete;
	Pty& operator=(const Pty&) = delete;

	~Pty()
	{
		Close();
	}

	// `command`, when non-null, runs via `/bin/sh -c command` instead of the
	// interactive `$SHELL` -- exists for this task's own deterministic self-
	// test (a real interactive shell's prompt/output depends on the user's
	// rc files, which a test can't predict). TerminalPanel (Task 4) always
	// calls this with no `command`, for a real interactive session.
	bool Open(int cols, int rows, const char* command = nullptr)
	{
		// A restart (TerminalPanel calling Open() again after a shell
		// exited) must not leak the previous session's fd/pid -- Open()
		// establishes a brand new session, so any prior one is torn down
		// first, the same way the destructor would.
		Close();

		struct winsize ws{};
		ws.ws_col = (unsigned short)cols;
		ws.ws_row = (unsigned short)rows;

		m_ChildPid = forkpty(&m_MasterFd, nullptr, nullptr, &ws);
		if (m_ChildPid < 0)
		{
			GS_ERROR("Pty::Open: forkpty failed: {0}", strerror(errno));
			return false;
		}

		if (m_ChildPid == 0)
		{
			// Child: replace this process image. Never returns on success.
			if (command)
			{
				execl("/bin/sh", "/bin/sh", "-c", command, (char*)nullptr);
			}
			else
			{
				const char* shell = getenv("SHELL");
				if (!shell || shell[0] == '\0')
					shell = "/bin/sh";
				execl(shell, shell, (char*)nullptr);
			}
			_exit(127); // execl only returns on failure
		}

		// Parent.
		int flags = fcntl(m_MasterFd, F_GETFL, 0);
		fcntl(m_MasterFd, F_SETFL, flags | O_NONBLOCK);
		return true;
	}

	// Non-blocking. Returns bytes read (>= 0, possibly 0 if nothing is
	// available this frame), or -1 once the child has exited and there is
	// nothing left to read.
	int Read(char* buf, int max)
	{
		if (m_MasterFd < 0)
			return -1;

		ssize_t n = read(m_MasterFd, buf, (size_t)max);
		if (n > 0)
			return (int)n;
		if (n == 0)
			return -1; // EOF: the child closed its end.
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0; // Nothing available this frame.
		return -1; // A real error (commonly EIO, once the child has exited).
	}

	// Best-effort: a full kernel pipe drops input rather than blocking the
	// frame loop. A human typing faster than a shell can drain its input
	// buffer is not a scenario that happens at interactive typing speed.
	void Write(const char* data, int len)
	{
		if (m_MasterFd < 0)
			return;
		(void)write(m_MasterFd, data, (size_t)len);
	}

	void Resize(int cols, int rows)
	{
		if (m_MasterFd < 0)
			return;
		struct winsize ws{};
		ws.ws_col = (unsigned short)cols;
		ws.ws_row = (unsigned short)rows;
		ioctl(m_MasterFd, TIOCSWINSZ, &ws); // delivers SIGWINCH to the child
	}

	bool IsAlive()
	{
		if (m_ChildPid <= 0)
			return false;

		int status = 0;
		pid_t result = waitpid(m_ChildPid, &status, WNOHANG);
		if (result == 0)
			return true;

		m_ChildPid = -1; // reaped -- don't waitpid an already-reaped pid again
		return false;
	}

private:
	void Close()
	{
		if (m_MasterFd >= 0)
		{
			close(m_MasterFd);
			m_MasterFd = -1;
		}

		if (m_ChildPid > 0)
		{
			kill(m_ChildPid, SIGHUP);

			int status = 0;
			bool reaped = false;
			for (int i = 0; i < 20 && !reaped; i++) // ~200ms grace, 10ms per check
			{
				if (waitpid(m_ChildPid, &status, WNOHANG) != 0)
					reaped = true;
				else
					usleep(10000);
			}
			if (!reaped)
			{
				kill(m_ChildPid, SIGKILL);
				waitpid(m_ChildPid, &status, 0);
			}
			m_ChildPid = -1; // must reset here too -- Close() can now run
			                 // from Open() while the object is still alive,
			                 // not only from the destructor
		}
	}

	int m_MasterFd = -1;
	pid_t m_ChildPid = -1;
};
