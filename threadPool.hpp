#ifndef _MV_THREADPOOL_H_
#define _MV_THREADPOOL_H_

#include <thread>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <list>
#include <iostream>

namespace MV {
	class ThreadPool {
		class Worker;
	public:
		class Job {
			friend Worker;
		public:
			Job(const std::function<void()> &a_action) :
				action(a_action) {
			}
			Job(const std::function<void()> &a_action, const std::function<void()> &a_onFinish) :
				action(a_action),
				onFinish(a_onFinish) {
			}
			Job(const std::function<void()> &a_action, const std::function<void()> &a_onFinish, const std::shared_ptr<std::atomic<size_t>> &a_groupCounter, const std::shared_ptr<std::function<void()>> &a_onGroupFinish) :
				action(a_action),
				onFinish(a_onFinish),
				groupCounter(a_groupCounter),
				onGroupFinish(a_onGroupFinish) {
			}
			Job(const std::function<void()> &a_action, const std::shared_ptr<std::atomic<size_t>> &a_groupCounter, const std::shared_ptr<std::function<void()>> &a_onGroupFinish) :
				action(a_action),
				groupCounter(a_groupCounter),
				onGroupFinish(a_onGroupFinish) {
			}
			Job(Job&& a_rhs) = default;
			Job(const Job& a_rhs) = default;

			void group(const std::shared_ptr<std::atomic<size_t>> &a_groupCounter, const std::shared_ptr<std::function<void()>> &a_onGroupFinish) {
				groupCounter = a_groupCounter;
				onGroupFinish = a_onGroupFinish;
			}

			void operator()() noexcept {
				try { action(); } catch (std::exception &e) { parent->exception(e); }
				bool groupFinished = groupCounter && --(*groupCounter) == 0 && onGroupFinish && *onGroupFinish;
				if (onFinish && !groupFinished) {
					parent->task(onFinish);
				}
				else if (onFinish && groupFinished) {
					parent->task([=] {
						try { onFinish(); } catch (std::exception &e) { parent->exception(e); }
						try { (*onGroupFinish)(); } catch (std::exception &e) { parent->exception(e); }
					});
				}
				else if (groupFinished) {
					parent->task((*onGroupFinish));
				}
			}
		private:
			ThreadPool* parent;

			std::function<void()> action;
			std::function<void()> onFinish;
			std::shared_ptr<std::atomic<size_t>> groupCounter;
			std::shared_ptr<std::function<void()>> onGroupFinish;
		};
		friend Job;
	private:
		class Worker {
		public:
			Worker(ThreadPool* a_parent) :
				parent(a_parent) {
				thread = std::make_unique<std::thread>([=]() { work(); });
			}
			~Worker() { if (thread && thread->joinable()) { thread->join(); } }

			void cleanup() {
				while (!finished) {}
			}
		private:
			void work() {
				while (!parent->stopped) {
					std::unique_lock<std::mutex> guard(parent->lock);
					parent->notify.wait(guard, [=] {return parent->jobs.size() > 0 || parent->stopped; });
					if (parent->stopped) { break; }

					auto job = std::move(parent->jobs.front());
					parent->jobs.pop_front();
					guard.unlock();

					job.parent = parent;
					job();
				}
				finished = true;
			}
			ThreadPool* parent;
			bool finished = false;
			std::unique_ptr<std::thread> thread;
		};
		friend Worker;

	public:
		ThreadPool() :
			ThreadPool(std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1) {
		}

		ThreadPool(size_t a_threads) {
			for (size_t i = 0; i < a_threads; ++i) {
				workers.push_back(std::make_unique<Worker>(this));
			}
		}

		~ThreadPool() {
			{
				std::lock_guard<std::mutex> guard(lock);
				jobs.clear();
				stopped = true;
			}
			notify.notify_all();
			for (auto&& worker : workers) {
				worker->cleanup();
			}
		}

		void task(const Job &a_newWork) {
			{
				std::lock_guard<std::mutex> guard(lock);
				jobs.emplace_back(std::move(a_newWork));
			}
			notify.notify_one();
		}
		void task(const std::function<void()> &a_task) {
			{
				std::lock_guard<std::mutex> guard(lock);
				jobs.emplace_back(a_task);
			}
			notify.notify_one();
		}
		void task(const std::function<void()> &a_task, const std::function<void()> &a_onComplete) {
			{
				std::lock_guard<std::mutex> guard(lock);
				jobs.emplace_back(a_task, a_onComplete);
			}
			notify.notify_one();
		}

		void tasks(const std::vector<Job> &a_tasks, const std::function<void()> &a_onGroupComplete) {
			std::shared_ptr<std::atomic<size_t>> counter = std::make_shared<std::atomic<size_t>>(a_tasks.size());
			std::shared_ptr<std::function<void()>> sharedGroupComplete = std::make_shared<std::function<void()>>(std::move(a_onGroupComplete));
			for (auto&& job : a_tasks)
			{
				{
					std::lock_guard<std::mutex> guard(lock);
					jobs.emplace_back(std::move(job));
					jobs.back().group(counter, sharedGroupComplete);
				}
				notify.notify_one();
			}
		}

		void schedule(const std::function<void()> &a_method) {
			std::lock_guard<std::mutex> guard(scheduleLock);
			scheduled.push_back(a_method);
		}

		void run() {
			for (auto i = scheduled.begin(); i != scheduled.end();) {
				try {
					(*i)();
				} catch (std::exception &e) {
					exception(e);
				}

				std::lock_guard<std::mutex> guard(scheduleLock);
				scheduled.erase(i++);
			}
		}

		void exceptionHandler(std::function<void(std::exception &e)> a_onException) {
			std::lock_guard<std::mutex> guard(exceptionLock);
			onException = a_onException;
		}

		size_t threads() const {
			return workers.size();
		}
	private:
		void exception(std::exception &e) {
			std::lock_guard<std::mutex> guard(exceptionLock);
			if (onException) {
				onException(e);
			} else {
				std::cerr << "Uncaught Exception in Thread Pool: " << e.what() << '\n';
			}
		}

		std::condition_variable notify;
		bool stopped = false;
		std::mutex lock;
		std::mutex scheduleLock;
		std::mutex exceptionLock;
		std::vector<std::unique_ptr<Worker>> workers;
		std::deque<Job> jobs;
		std::list<std::function<void()>> scheduled;
		std::deque<std::string> exceptionMessages;
		std::function<void(std::exception &e)> onException;
	};
}

#endif
