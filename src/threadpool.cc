/*
 * Copyright deipi.com LLC and contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "utils.h"

#include "threadpool.h"


// Function that retrieves a task from a queue, runs it and deletes it
static void *getWork(void * param) {
	Queue<Task *> *wq = (Queue<Task *> *)param;
	Task *mw = NULL;
	while (wq->pop(mw)) {
		mw->run();
		delete mw;
	}
	return NULL;
}


// Allocate a thread pool and set them to work trying to get tasks
ThreadPool::ThreadPool(int n) : numThreads(n) {
	LOG_OBJ(this, "Creating a thread pool with %d threads\n", n);

	threads = new pthread_t[numThreads];
	for (int i = 0; i < numThreads; ++i) {
		pthread_create(&(threads[i]), 0, getWork, &workQueue);
	}
}


// Wait for the threads to finish, then delete them
ThreadPool::~ThreadPool() {
	finish();
	join();
	delete [] threads;
}


void ThreadPool::join() {
	for (int i = 0; i < numThreads; ++i) {
		pthread_join(threads[i], 0);
	}
}


// Add a task
void ThreadPool::addTask(Task *nt) {
	workQueue.push(nt);
}


// Tell the tasks to finish and return
void ThreadPool::finish() {
	workQueue.finish();
}
