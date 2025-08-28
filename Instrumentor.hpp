#pragma once

#ifdef PROFILING

#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <assert.h>
#include <iostream>
#include <queue>
#include <mutex>

#include <thread>
#include <functional>

#include <source_location>
#include <stacktrace>

namespace Profiling {

	struct ProfileResult {
		std::string Name;
		long long Start;
		long long End;
		std::size_t ThreadID;
	};

	class Instrumentor {
	public:

		struct SessionRAII {

			SessionRAII(const std::string& name, const std::filesystem::path& path) {
				Instrumentor::Get().BeginSession(name,path);
			}

			~SessionRAII() {
				Instrumentor::Get().EndSession();
			}

		};

	private:
		std::string CurrentSession = "";
		std::ofstream OStream;
		std::size_t ProfileCount = 0;

		std::mutex QueueMutex;
		std::queue<ProfileResult> Queue;

		std::thread mythread;
		std::atomic_bool endSession = false;

		std::mutex cvMutex;
		std::condition_variable cv;

		std::chrono::time_point<std::chrono::high_resolution_clock> EndSessionStartTimepoint;
		
#ifdef PROFILINGQUEUESIZEOUTPUT
		std::chrono::time_point<std::chrono::high_resolution_clock> LastOutput;
#endif

		void WriteHeader() {
			OStream << "{\"otherData\": {},\"traceEvents\":[";
			OStream.flush();
		}

		void WriteFooter() {
			OStream << "]}";
			OStream.flush();
		}

		void WriteProfile(const ProfileResult& result) {
			if (ProfileCount++ > 0) {
				OStream << ",";
			}

			std::string name = result.Name;
			std::replace(name.begin(), name.end(), '"', '\'');
			for (auto i : name) {
				if (i == '\\') {
					std::cout << "Fehler";
				}
			}
			OStream << "{";
			OStream << "\"cat\":\"function\",";
			OStream << "\"dur\":" << (result.End - result.Start) << ',';
			OStream << "\"name\":\"" << name << "\",";
			OStream << "\"ph\":\"X\",";
			OStream << "\"pid\":0,";
			OStream << "\"tid\":" << result.ThreadID << ",";
			OStream << "\"ts\":" << result.Start;
			OStream << "}";

			OStream.flush();
		}

		void Loop() {
			WriteHeader();
			while (true) {
				std::unique_lock UL(cvMutex);
				cv.wait(UL, [this] {
					using namespace std::chrono_literals;
					if (endSession) return true;
					std::unique_lock QL(QueueMutex);
					if (!Queue.empty())return true;
					QL.unlock();
#ifdef PROFILINGQUEUESIZEOUTPUT
					if (std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::high_resolution_clock::now()) - LastOutput > 1s) return true;
#endif
					return false;
					});

#ifdef PROFILINGQUEUESIZEOUTPUT
				using namespace std::chrono_literals;
				if (std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::high_resolution_clock::now()) - LastOutput > 1s) {
					LastOutput = std::chrono::high_resolution_clock::now();
					std::unique_lock QL(QueueMutex);
					std::cout << "Queue.size() = " << Queue.size() << "\n";
				}
#endif

				std::unique_lock QL(QueueMutex);
				if (endSession && Queue.empty()) {
					auto EndSessionendTimepoint = std::chrono::high_resolution_clock::now();

					long long start = std::chrono::time_point_cast<std::chrono::microseconds>(EndSessionStartTimepoint).time_since_epoch().count();
					long long end = std::chrono::time_point_cast<std::chrono::microseconds>(EndSessionendTimepoint).time_since_epoch().count();

					auto id = std::this_thread::get_id();
					std::size_t threadID = *reinterpret_cast<unsigned int*>(&id);
					WriteProfile({ "EndSession " + CurrentSession, start, end, threadID });
					WriteFooter();
					OStream.close();
					return;
				}

				if (!Queue.empty()) {
					ProfileResult pr = Queue.front();
					Queue.pop();
					QL.unlock();

					WriteProfile(pr);
				}
			}
		}

	public:

		Instrumentor() {
#ifdef PROFILINGQUEUESIZEOUTPUT
			using namespace std::chrono_literals;
			LastOutput = std::chrono::high_resolution_clock::now();
#endif
		}

		Instrumentor(const Instrumentor&) = delete;
		Instrumentor(const Instrumentor&&) = delete;
		Instrumentor& operator=(const Instrumentor&) = delete;
		Instrumentor& operator=(const Instrumentor&&) = delete;

		~Instrumentor() {
			assert(CurrentSession == "");
		}

		void BeginSession(const std::string& name, const std::filesystem::path& filepath) {
			assert(CurrentSession == "");
			std::cout << "BeginSession " << name << std::endl;
			ProfileCount = 0;
			OStream.open(filepath);
			CurrentSession = name;
			mythread = std::thread(&Instrumentor::Loop, this);
		}

		void EndSession() {
			EndSessionStartTimepoint = std::chrono::high_resolution_clock::now();

			if (mythread.joinable()) {
				endSession = true;
				cv.notify_one();
				mythread.join();
			}
			std::cout << "EndSession " << CurrentSession << std::endl;

			CurrentSession = "";
		}

		void AddProfileResult(ProfileResult pr) {
			if(CurrentSession == "") {
					std::cerr << "Error profiling without session running:"
					<< "Error on finish of " << pr.Name
					<< std::stacktrace::current() << "\n";
					exit(1);
			}
			{
				std::unique_lock QL(QueueMutex);
				Queue.push(pr);
			}
			cv.notify_one();
		}

		static Instrumentor& Get() {
			static Instrumentor instance;
			return instance;
		}
	};

	class InstrumentationTimer {
	private:
		std::string Name;
		std::chrono::time_point<std::chrono::high_resolution_clock> StartTimepoint;
		bool Stopped = false;
	public:
		InstrumentationTimer(const char* name)
			: Name(name) {
			StartTimepoint = std::chrono::high_resolution_clock::now();
		}
		InstrumentationTimer(const std::string& name)
			: Name(name) {
			StartTimepoint = std::chrono::high_resolution_clock::now();
		}

		~InstrumentationTimer() {
			if (!Stopped)
				Stop();
		}

		void Stop()
		{
			auto endTimepoint = std::chrono::high_resolution_clock::now();

			long long start = std::chrono::time_point_cast<std::chrono::microseconds>(StartTimepoint).time_since_epoch().count();
			long long end = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch().count();

			auto id = std::this_thread::get_id();
			std::size_t threadID = *reinterpret_cast<unsigned int*>(&id);
			ProfileResult pr{ Name, start, end, threadID };
			Instrumentor::Get().AddProfileResult(pr);

			Stopped = true;
		}
	};
}
#define Instumentor_Makro_Combine_Innter(a,b) a##b
#define Instumentor_Makro_Combine(a,b) Instumentor_Makro_Combine_Innter(a,b)


#define PROFILE_SCOPE(name) Profiling::InstrumentationTimer Instumentor_Makro_Combine(timer,__LINE__)(name)
#define PROFILE_SCOPE_ID_START(name,id) Profiling::InstrumentationTimer Instumentor_Makro_Combine(timerId,id)(name)
#define PROFILE_SCOPE_ID_END(id) Instumentor_Makro_Combine(timerId,id).Stop()
#define PROFILE_FUNKTION PROFILE_SCOPE([]() {\
constexpr const std::source_location location = std::source_location::current();\
return location.function_name();\
}())

#define PROFILE_SESSION_START(name,filepath) Profiling::Instrumentor::Get().BeginSession(name,filepath)
#define PROFILE_SESSION_END Profiling::Instrumentor::Get().EndSession()
#define PROFILE_SESSION_RAII(name) Profiling::Instrumentor::SessionRAII Instumentor_Makro_Combine(sessionRAII,__LINE__)(name)
#else

#define PROFILE_SCOPE(name)
#define PROFILE_SCOPE_ID_START(name,id)
#define PROFILE_SCOPE_ID_END(id)
#define PROFILE_FUNKTION
#define PROFILE_SESSION_START(name,filepath)
#define PROFILE_SESSION_END
#define PROFILE_SESSION_RAII(name)
#endif
