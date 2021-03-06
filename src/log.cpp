/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "log.h"

#include "threading/mutex_auto_lock.h"
#include "debug.h"
#include "gettime.h"
#include "porting.h"
#include "config.h"
#include "exceptions.h"
#include "util/numeric.h"
#include "log.h"

#include <sstream>
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <cstring>

class StringBuffer : public std::streambuf {
public:
	StringBuffer() {}

	int overflow(int c);
	virtual void flush(const std::string &buf) = 0;
	std::streamsize xsputn(const char *s, std::streamsize n);
	void push_back(char c);

private:
	std::string buffer;
};


class LogBuffer : public StringBuffer {
public:
	LogBuffer(Logger &logger, LogLevel lev) :
		logger(logger),
		level(lev)
	{}

	void flush(const std::string &buffer);

private:
	Logger &logger;
	LogLevel level;
};


class RawLogBuffer : public StringBuffer {
public:
	void flush(const std::string &buffer);
};


#ifdef __ANDROID__
static unsigned int level_to_android[] = {
	ANDROID_LOG_INFO,     // LL_NONE
	//ANDROID_LOG_FATAL,
	ANDROID_LOG_ERROR,    // LL_ERROR
	ANDROID_LOG_WARN,     // LL_WARNING
	ANDROID_LOG_WARN,     // LL_ACTION
	//ANDROID_LOG_INFO,
	ANDROID_LOG_DEBUG,    // LL_INFO
	ANDROID_LOG_VERBOSE,  // LL_VERBOSE

};
#endif

////
//// Globals
////

Logger g_logger;

StreamLogOutput stdout_output(std::cout);
StreamLogOutput stderr_output(std::cerr);
std::ostream null_stream(NULL);

RawLogBuffer raw_buf;

LogBuffer none_buf(g_logger, LL_NONE);
LogBuffer error_buf(g_logger, LL_ERROR);
LogBuffer warning_buf(g_logger, LL_WARNING);
LogBuffer action_buf(g_logger, LL_ACTION);
LogBuffer info_buf(g_logger, LL_INFO);
LogBuffer verbose_buf(g_logger, LL_VERBOSE);

// Connection
std::ostream *dout_con_ptr = &null_stream;
std::ostream *derr_con_ptr = &verbosestream;

// Server
std::ostream *dout_server_ptr = &infostream;
std::ostream *derr_server_ptr = &errorstream;

#ifndef SERVER
// Client
std::ostream *dout_client_ptr = &infostream;
std::ostream *derr_client_ptr = &errorstream;
#endif

std::ostream rawstream(&raw_buf);
std::ostream dstream(&none_buf);
std::ostream errorstream(&error_buf);
std::ostream warningstream(&warning_buf);
std::ostream actionstream(&action_buf);
std::ostream infostream(&info_buf);
std::ostream verbosestream(&verbose_buf);


///////////////////////////////////////////////////////////////////////////////


////
//// Logger
////

LogLevel Logger::stringToLevel(const std::string &name)
{
	if (name == "none")
		return LL_NONE;
	else if (name == "error")
		return LL_ERROR;
	else if (name == "warning")
		return LL_WARNING;
	else if (name == "action")
		return LL_ACTION;
	else if (name == "info")
		return LL_INFO;
	else if (name == "verbose")
		return LL_VERBOSE;
	else
		return LL_MAX;
}

void Logger::addOutput(ILogOutput *out)
{
	addOutputMaxLevel(out, LL_MAX);
}

void Logger::addOutput(ILogOutput *out, LogLevel lev)
{
	m_outputs[lev].push_back(out);
}

void Logger::addOutputMaxLevel(ILogOutput *out, LogLevel lev)
{
	for (size_t i = 0; i <= lev; i++)
		m_outputs[i].push_back(out);
}

void Logger::removeOutput(ILogOutput *out)
{
	for (size_t i = 0; i < LL_MAX; i++) {
		std::vector<ILogOutput *>::iterator it;

		it = std::find(m_outputs[i].begin(), m_outputs[i].end(), out);
		if (it != m_outputs[i].end())
			m_outputs[i].erase(it);
	}
}

void Logger::setLevelSilenced(LogLevel lev, bool silenced)
{
	m_silenced_levels[lev] = silenced;
}

void Logger::registerThread(const std::string &name)
{
	threadid_t id = thr_get_current_thread_id();
	MutexAutoLock lock(m_mutex);
	m_thread_names[id] = name;
}

void Logger::deregisterThread()
{
	threadid_t id = thr_get_current_thread_id();
	MutexAutoLock lock(m_mutex);
	m_thread_names.erase(id);
}

const std::string Logger::getLevelLabel(LogLevel lev)
{
	static const std::string names[] = {
		"",
		"ERROR",
		"WARNING",
		"ACTION",
		"INFO",
		"VERBOSE",
	};
	assert(lev < LL_MAX && lev >= 0);
	assert(ARRLEN(names) == LL_MAX);
	return names[lev];
}

const std::string Logger::getThreadName()
{
	std::map<threadid_t, std::string>::const_iterator it;

	threadid_t id = thr_get_current_thread_id();
	it = m_thread_names.find(id);
	if (it != m_thread_names.end())
		return it->second;

	std::ostringstream os;
	os << "#0x" << std::hex << id;
	return os.str();
}

void Logger::log(LogLevel lev, const std::string &text)
{
	if (m_silenced_levels[lev])
		return;

	const std::string thread_name = getThreadName();
	const std::string label = getLevelLabel(lev);
	std::ostringstream os(std::ios_base::binary);
	os << getTimestamp() << ": " << label << "[" << thread_name << "]: " << text;

	logToSystem(lev, text);
	logToOutputs(lev, os.str());
}

void Logger::logRaw(LogLevel lev, const std::string &text)
{
	if (m_silenced_levels[lev])
		return;

	logToSystem(lev, text);
	logToOutputs(lev, text);
}

void Logger::logToSystem(LogLevel lev, const std::string &text)
{
#ifdef __ANDROID__
	assert(ARRLEN(level_to_android) == LL_MAX);
	__android_log_print(level_to_android[lev],
		PROJECT_NAME_C, "%s", text.c_str());
#endif
}

void Logger::logToOutputs(LogLevel lev, const std::string &text)
{
	MutexAutoLock lock(m_mutex);
	for (size_t i = 0; i != m_outputs[lev].size(); i++)
		m_outputs[lev][i]->log(text);
}


////
//// *LogOutput methods
////

void FileLogOutput::open(const std::string &filename)
{
	stream.open(filename.c_str(), std::ios::app | std::ios::ate);
	if (!stream.good())
		throw FileNotGoodException("Failed to open log file " +
			filename + ": " + strerror(errno));
	stream << "\n\n"
		   "-------------" << std::endl
		<< "  Separator" << std::endl
		<< "-------------\n" << std::endl;
}



////
//// *Buffer methods
////

int StringBuffer::overflow(int c)
{
	push_back(c);
	return c;
}


std::streamsize StringBuffer::xsputn(const char *s, std::streamsize n)
{
	for (int i = 0; i < n; ++i)
		push_back(s[i]);
	return n;
}

void StringBuffer::push_back(char c)
{
	if (c == '\n' || c == '\r') {
		if (!buffer.empty())
			flush(buffer);
		buffer.clear();
	} else {
		buffer.push_back(c);
	}
}




void LogBuffer::flush(const std::string &buffer)
{
	logger.log(level, buffer);
}

void RawLogBuffer::flush(const std::string &buffer)
{
	g_logger.logRaw(LL_NONE, buffer);
}
