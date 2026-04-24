#include "ThreadPool.h"
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

void print(int i) {
    std::cout << "Task " << i << " is running in thread " << std::this_thread::get_id()
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Task " << i << " finished" << std::endl;
}

int main() {
    ThreadPool *pool = new ThreadPool(4);
    for (int i = 0; i < 8; ++i) {
        pool->add(print, i);
    }
    delete pool;
    return 0;
}