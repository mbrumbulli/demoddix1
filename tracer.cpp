/* Copyright 2013 Humboldt University of Berlin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 * 
 * Author: Mihal Brumbulli <mbrumbulli@gmail.com>
 */

#include <cstring>
#include <climits>
#include <algorithm>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include "demoddix.hpp"

std::vector<Tracer> Tracer::tracerList;						// list of tracers
std::vector<std::thread> Tracer::threadList;				// list of threads for tracers

std::thread Tracer::pollThread;								// thread for continously checking tracers
std::string Tracer::tracerCommand 		= "msc-tracer -p ";	// command for opening tracer
std::string Tracer::pollCommand 		= "resume|\n";		// command for checking tracers (resumes trace)
unsigned int Tracer::pollInterval 		= 200; 				// checking frequency in ms
std::atomic_bool Tracer::doPoll(true);						// guard for checking (if false terminate checking thread)

// format for reading values from trace files
static const char* taskCreatedF 		= "<taskCreated nId=\"n%lu\" time=\"%llu\" creatorId=\"%lx\" pName=\"p%u\" creatorName=\"p%u\" pId=\"%lx\" />";
static const char* taskDeletedF 		= "<taskDeleted nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" pId=\"%lx\" />";
static const char* messageSentF 		= "<messageSent nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" mId=\"%lx\" pId=\"%lx\" sigNum=\"%u\" msgName=\"m%u\" />";
static const char* messageReceivedF 	= "<messageReceived nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" mId=\"%lx\" pId=\"%lx\" sigNum=\"%u\" msgName=\"m%u\" />";
static const char* messageSavedF 		= "<messageSaved nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" mId=\"%lx\" pId=\"%lx\" sigNum=\"%u\" msgName=\"m%u\" />";
static const char* semaphoreCreatedF 	= "<semaphoreCreated nId=\"n%lu\" time=\"%llu\" semName=\"x%u\" stillAvailable=\"%d\" pId=\"%lx\" />";
static const char* takeAttemptF 		= "<takeAttempt nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" semName=\"x%u\" timeout=\"%d\" pId=\"%lx\" semId=\"%lx\" />";
static const char* takeSuccededF 		= "<takeSucceeded nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" semName=\"x%u\" stillAvailable=\"%d\" pId=\"%lx\" semId=\"%lx\" />";
static const char* takeTimedOutF 		= "<takeTimedOut nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" semName=\"x%u\" pId=\"%lx\" semId=\"%lx\" />";
static const char* giveSemF 			= "<giveSem nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" semName=\"x%u\" pId=\"%lx\" semId=\"%lx\" />";
static const char* timerStartedF 		= "<timerStarted nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" timerName=\"m%u\" pId=\"%lx\" tId=\"%lx\" timeLeft=\"%d\" />";
static const char* timerCancelledF 		= "<timerCancelled nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" timerName=\"m%u\" pId=\"%lx\" tId=\"%lx\" />";
static const char* timerTimedOutF 		= "<timerTimedOut nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" timerName=\"m%u\" pId=\"%lx\" tId=\"%lx\" />";
static const char* taskChangedStateF 	= "<taskChangedState nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" pId=\"%lx\" stateName=\"s%u\" />";
static const char* informationF 		= "<information nId=\"n%lu\" time=\"%llu\" pName=\"p%u\" pId=\"%lx\" message=\"%[^\"]\" />";

// initialize tracers
void Tracer::Open() 
{	
	Tracer::tracerList.resize(Demoddix::nodeList.size());
	Tracer::threadList.resize(Demoddix::nodeList.size());
	Tracer::pollThread = std::thread(Tracer::Poll);
}

// terminate tracers
void Tracer::Close() 
{	
	Tracer::doPoll.store(false);
	Tracer::pollThread.join();
	
	// terminate leftover tracers
	for (auto& t: Tracer::threadList) {
		if (t.joinable()) {
			t.join();
		}
	}
}

// launch tracer
void Tracer::Launch(unsigned long id) 
{
	// tracer is still show
	if (Tracer::tracerList[id].status() != Tracer::IDLE) {
		std::cerr << "Warning: tracer on node " << id << " is still running." << std::endl;
		return;
	}
	
	// used ports from opened tracers
	std::vector<unsigned short> used;
	for (auto& t: Tracer::tracerList) {
		if (t.port() > 0) {
			used.push_back(t.port());
		}
	}
	
	// find a free port for tracer to listen
	unsigned short p = USHRT_MAX;
	for (; p > 0; --p) {
		if (std::find(used.begin(), used.end(), p) != used.end()) {
			continue;
		}
		addrinfo hints;
		addrinfo* result = NULL;
		addrinfo* rp = NULL;
		memset(&hints, 0, sizeof(addrinfo));
		hints.ai_family = AF_UNSPEC;     // return IPv4 and IPv6 choices
		hints.ai_socktype = SOCK_STREAM; // TCP socket
		hints.ai_flags = AI_PASSIVE;     // all interfaces
		if (getaddrinfo(NULL, std::to_string(p).c_str(), &hints, &result) == 0) {
			for (rp = result; rp != NULL; rp = rp->ai_next) {
				int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
				if (sock < 0) {
					continue;
				}
				int flags = fcntl(sock, F_GETFL, 0);
				if (flags == -1) {
					close(sock);
					continue;
				}
				flags |= O_NONBLOCK;
				if (fcntl(sock, F_SETFL, flags) == -1) {
					close(sock);
					continue;
				}
				int on = 1;
				if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void*) &on, sizeof(on)) == 0 && bind(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
					close(sock);
					break;
				}	
			}
			if (rp != NULL) {
				freeaddrinfo(result);
				break;
			}
			freeaddrinfo(result);
		}
	}
	
	if (p == 0) {
		std::cerr << "Warning: no free port for tracer on node " << id << "." << std::endl;
		return;
	}
	
	// create thread for tracer
	Tracer::tracerList[id].port(p);
	Tracer::threadList[id] = std::thread([=] {
		Tracer::tracerList[id].status(Tracer::OPENED);
		std::system((Tracer::tracerCommand + std::to_string(Tracer::tracerList[id].port())).c_str());
		Tracer::tracerList[id].status(Tracer::CLOSED);
	});
}

// continously check tracer status
void Tracer::Poll()
{
	while (Tracer::doPoll.load()) {
		for (unsigned long id = 0; id < Tracer::tracerList.size(); ++id) {
			// if tracer is idle, then do nothing 
			if (Tracer::tracerList[id].status() == Tracer::IDLE) {
				continue;
			}
			// if tracer is opened, then try to connect to it
			else if (Tracer::tracerList[id].status() == Tracer::OPENED) {
				addrinfo hints;
				addrinfo* result = NULL;
				addrinfo* rp = NULL;
				memset(&hints, 0, sizeof(addrinfo));
				hints.ai_family = AF_UNSPEC;     // return IPv4 and IPv6 choices
				hints.ai_socktype = SOCK_STREAM; // TCP socket
				hints.ai_flags = AI_PASSIVE;     // all interfaces
				if (getaddrinfo(NULL, std::to_string(Tracer::tracerList[id].port()).c_str(), &hints, &result) == 0) {
					for (rp = result; rp != NULL; rp = rp->ai_next) {
						if (Tracer::tracerList[id].sock() < 0) {
							Tracer::tracerList[id].sock(socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol));
							if (Tracer::tracerList[id].sock() < 0) {
								continue;
							}
							int flags = fcntl(Tracer::tracerList[id].sock(), F_GETFL, 0);
							if (flags == -1) {
								close(Tracer::tracerList[id].sock());
								Tracer::tracerList[id].sock(-1);
								continue;
							}
							flags |= O_NONBLOCK;
							if (fcntl(Tracer::tracerList[id].sock(), F_SETFL, flags) == -1) {
								close(Tracer::tracerList[id].sock());
								Tracer::tracerList[id].sock(-1);
								continue;
							}
						}
						if (connect(Tracer::tracerList[id].sock(), rp->ai_addr, rp->ai_addrlen) == 0) {
							Tracer::tracerList[id].status(Tracer::CONNECTED);
						}
						break;	
					}
					freeaddrinfo(result);
				}
			}
			// if tracer is connected, then check connection by sending a resume command
			else if (Tracer::tracerList[id].status() == Tracer::CONNECTED) {
				// if send fails, then close socket
				if (write(Tracer::tracerList[id].sock(), Tracer::pollCommand.c_str(), Tracer::pollCommand.size()) < 0) {
					close(Tracer::tracerList[id].sock());
					Tracer::tracerList[id].sock(-1);
					Tracer::tracerList[id].status(Tracer::OPENED);
				}
			}
			// if tracer is closed, then terminate thread
			else if (Tracer::tracerList[id].status() == Tracer::CLOSED) {
				close(Tracer::tracerList[id].sock());
				Tracer::tracerList[id].port(0);
				Tracer::tracerList[id].sock(-1);
				Tracer::tracerList[id].status(Tracer::IDLE);
				Tracer::threadList[id].join();
			}
		}
		// sleep for some time
		std::this_thread::sleep_for(std::chrono::milliseconds(Tracer::pollInterval));
	}
}

// format trace and send it to the tracer
void Tracer::Send(const char* buffer)
{
	char command[Demoddix::bufferSize];
	unsigned long long time;
	unsigned long nId, creatorId, pId, mId, semId, tId;
	unsigned int pName, creatorName, sigNum, msgName, semName, timerName, stateName;
	int stillAvailable, timeout, timeLeft;
	char info[256];
	if (sscanf(buffer, taskCreatedF, &nId, &time, &creatorId, &pName, &creatorName, &pId) == 6) {
		sprintf(command, "taskCreated| -t%llu| -c%lu| -n%s| -N%s| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, creatorId, Demoddix::processList[pName].name.c_str(), Demoddix::processList[creatorName].name.c_str(), pId);
	}
	else if (sscanf(buffer, taskDeletedF, &nId, &time, &pName, &pId) == 4) {
		sprintf(command, "taskDeleted| -t%llu| -n%s| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), pId);
	}
	else if (sscanf(buffer, messageSentF, &nId, &time, &pName, &mId, &pId, &sigNum, &msgName) == 7) {
		sprintf(command, "messageSent| -t%llu| -n%s| -i%lu| %lu| %d| %s|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), mId, pId, sigNum, Demoddix::messageList[msgName].name.c_str());
	}
	else if (sscanf(buffer, messageReceivedF, &nId, &time, &pName, &mId, &pId, &sigNum, &msgName) == 7) {
		sprintf(command, "messageReceived| -t%llu| -n%s| -i%lu| %lu| %d| %s|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), mId, pId, sigNum, Demoddix::messageList[msgName].name.c_str());
	}
	else if (sscanf(buffer, messageSavedF, &nId, &time, &pName, &mId, &pId, &sigNum, &msgName) == 7) {
		sprintf(command, "messageSaved| -t%llu| -n%s| -i%lu| %lu| %d| %s|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), mId, pId, sigNum, Demoddix::messageList[msgName].name.c_str());
	}
	else if (sscanf(buffer, semaphoreCreatedF, &nId, &time, &semName, &stillAvailable, &pId)) {
		sprintf(command, "semaphoreCreated| -t%llu| -s%s| -a%d| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::semaphoreList[semName].name.c_str(), stillAvailable, pId);
	}
	else if (sscanf(buffer, takeAttemptF, &nId, &time, &pName, &semName, &timeout, &pId, &semId)) {
		sprintf(command, "takeAttempt| -t%llu| -n%s| -s%s| -T%d| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::semaphoreList[semName].name.c_str(), timeout, pId, semId);
	}
	else if (sscanf(buffer, takeSuccededF, &nId, &time, &pName, &semName, &stillAvailable, &pId, &semId)) {
		sprintf(command, "takeSucceeded| -t%llu| -n%s| -s%s| -a%d| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::semaphoreList[semName].name.c_str(), stillAvailable, pId, semId);
	}
	else if (sscanf(buffer, takeTimedOutF, &nId, &time, &pName, &semName, &pId, &semId)) {
		sprintf(command, "takeTimedOut| -t%llu| -n%s| -s%s| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::semaphoreList[semName].name.c_str(), pId, semId);
	}
	else if (sscanf(buffer, giveSemF, &nId, &time, &pName, &semName, &pId, &semId)) {
		sprintf(command, "giveSem| -t%llu| -n%s| -s%s| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::semaphoreList[semName].name.c_str(), pId, semId);
	}
	else if (sscanf(buffer, timerStartedF, &nId, &time, &pName, &timerName, &pId, &tId, &timeLeft)) {
		sprintf(command, "timerStarted| -t%llu| -n%s| -T%s| %lu| %lu| %d|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::messageList[timerName].name.c_str(), pId, tId, timeLeft);
	}
	else if (sscanf(buffer, timerCancelledF, &nId, &time, &pName, &timerName, &pId, &tId)) {
		sprintf(command, "timerCancelled| -t%llu| -n%s| -T%s| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::messageList[timerName].name.c_str(), pId, tId);
	}
	else if (sscanf(buffer, timerTimedOutF, &nId, &time, &pName, &timerName, &pId, &tId)) {
		sprintf(command, "timerTimedOut| -t%llu| -n%s| -T%s| %lu| %lu|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), Demoddix::messageList[timerName].name.c_str(), pId, tId);
	}
	else if (sscanf(buffer, taskChangedStateF, &nId, &time, &pName, &pId, &stateName)) {
		sprintf(command, "taskChangedState| -t%llu| -n%s| %lu| %s|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), pId, Demoddix::stateList[stateName].name.c_str());
	}
	else if (sscanf(buffer, informationF, &nId, &time, &pName, &pId, info)) {
		sprintf(command, "information| -t%llu| -n%s| %lu| %s|\n", 
			(time - Demoddix::beginTime) / 1000000, Demoddix::processList[pName].name.c_str(), pId, info);
	}
	else {
		return;
	}
	
	// close socket if send fails 
	if (write(Tracer::tracerList[nId].sock(), command, strlen(command)) < 0) {
		close(Tracer::tracerList[nId].sock());
		Tracer::tracerList[nId].sock(-1);
		Tracer::tracerList[nId].status(Tracer::OPENED);
	}
}
