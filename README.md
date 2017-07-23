# MutedThreadPool

A thread pool implemented with standard C++.

threadPool.hpp is a little higher performance on my own use cases and a little less blocking than the boost::asio implementation.

The boost one is split up to asioThreadPool.h and asioThreadPool.cpp to aid in compile time.

```
#include "threadPool.hpp"
#include <chrono>

//group tasks
void scheduleGroup(MV::ThreadPool *pool) {
	pool->tasks({
		{ [] {std::cout << "A"; } },
		{ [] {std::cout << "B"; } },
		{ [] {std::cout << "C"; } },
	}, [=] { 
		pool->schedule([=] { //this calls it on the next call of "run" on the main thread. Remove this line...
			std::cout << "Group Done"; 
			scheduleGroup(pool); 
		}); //... and this line to just call it on the next available thread in the pool.
	});
}

void main() {
	MV::ThreadPool pool;
	for (int i = 0; i < 10; ++i)
		pool.task([=]{std::cout << i; }); //fire and forget tasks.

	for (int i = 0; i < 10; ++i){
		auto result = std::make_shared<int>(0);

		//finish callback. Useful for breaking up bigger tasks and giving the thread pool a chance to breathe and queue other tasks in between.
		pool.task([=]{*result = i;}, [=]{std::cout << *result;});
	}
	scheduleGroup(&pool);
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	pool.run();
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	pool.run();
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	pool.run();
	std::cout << "\nDone!\n";
}
```
