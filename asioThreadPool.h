#ifndef _MV_ASIOTHREADPOOL_H_
#define _MV_ASIOTHREADPOOL_H_

#include <thread>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <list>
#include <iostream>

namespace boost { namespace asio { class io_service; } }

namespace MV {
	struct ThreadPoolDetails;

	class AsioThreadPool {
	public:
		class Job {
			friend AsioThreadPool;
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
				} else if (onFinish && groupFinished) {
					parent->task([=]{
						try { onFinish(); } catch (std::exception &e) { parent->exception(e); }
						try { (*onGroupFinish)(); } catch (std::exception &e) { parent->exception(e); }
					});
				} else if (groupFinished) {
					parent->task((*onGroupFinish));
				}
			}
		private:
			AsioThreadPool* parent;

			std::function<void()> action;
			std::function<void()> onFinish;
			std::shared_ptr<std::atomic<size_t>> groupCounter;
			std::shared_ptr<std::function<void()>> onGroupFinish;
		};
		friend Job;
	public:
		AsioThreadPool() :
			AsioThreadPool(std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1) {
		}

		AsioThreadPool(size_t a_threads);

		~AsioThreadPool();

		void task(Job a_newWork);
		void task(const std::function<void()> &a_task) {
			task(Job{ a_task });
		}
		void task(const std::function<void()> &a_task, const std::function<void()> &a_onComplete) {
			task(Job{ a_task, a_onComplete });
		}

		void tasks(const std::vector<Job> &a_tasks, const std::function<void()> &a_onGroupComplete) {
			std::shared_ptr<std::atomic<size_t>> counter = std::make_shared<std::atomic<size_t>>(a_tasks.size());
			std::shared_ptr<std::function<void()>> sharedGroupComplete = std::make_shared<std::function<void()>>(std::move(a_onGroupComplete));
			for (auto&& job : a_tasks) {
				task(Job{job.action, job.onFinish, counter, sharedGroupComplete});
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

		std::shared_ptr<boost::asio::io_service> service() const;
	private:
		void exception(std::exception &e) {
			std::lock_guard<std::mutex> guard(exceptionLock);
			if (onException) {
				onException(e);
			} else {
				std::cerr << "Uncaught Exception in Thread Pool: " << e.what() << '\n';
			}
		}

		std::unique_ptr<ThreadPoolDetails> details;
		std::vector<std::unique_ptr<std::thread>> workers;
		std::mutex lock;
		std::mutex scheduleLock;
		std::mutex exceptionLock;
		std::deque<Job> jobs;
		std::list<std::function<void()>> scheduled;
		std::deque<std::string> exceptionMessages;
		std::function<void(std::exception &e)> onException;
	};
}

#endif